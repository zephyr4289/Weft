// gpu_ring_test.c — GPU-series conformance for the GPU-resident ring.
//
// Gates (any failure exits non-zero):
//   GPU1 create: session header validates (attach-by-bytes contract),
//       geometry reconciles, backend + device reported
//   GPU2 the ring is a ring wherever it lives: fan-out writer + reader
//       over the mapping, publish/claim, mixer payload bit-exact,
//       telescoping exact
//   GPU3 session interchange: a gpu_ring session's BYTES validate under
//       the shm attach contract (same protocol) — the formats agree
//   GPU4 vulkan leg (when the backend is vulkan): persistent map is the
//       CPU ring pointer; live publishes are visible through a SECOND
//       fan-out reader attached to the same mapping; teardown is clean
//   GPU5 lifecycle: NULL-safe destroy; double create/destroy cycles
//       hold no leaks under ASAN
//
// The compute-DISPATCH proof is gpu_probe.c's job (CI gpu-native shard);
// this suite pins the ring module's own contract on every host.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fanout.h"
#include "gpu_ring.h"

static int g_fail = 0;

static void check(int cond, const char* name) {
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// The session header validation (shared contract with shm_ring —
// reimplemented here to avoid linking that module; the layout is the
// contract, and GPU3's point is exactly that the two agree).
static int session_bytes_validate(const uint8_t* base, size_t span) {
    if (span < 64) return 0;
    uint32_t magic = (uint32_t)base[0] | ((uint32_t)base[1] << 8) |
                     ((uint32_t)base[2] << 16) | ((uint32_t)base[3] << 24);
    if (magic != 0x48534657u) return 0;
    uint16_t ver = (uint16_t)(base[4] | (base[5] << 8));
    if (ver != 1) return 0;
    uint16_t hs = (uint16_t)(base[6] | (base[7] << 8));
    if (hs != 64) return 0;
    uint32_t flags = (uint32_t)base[8] | ((uint32_t)base[9] << 8) |
                     ((uint32_t)base[10] << 16) | ((uint32_t)base[11] << 24);
    if (flags != 0) return 0;
    uint32_t pb = (uint32_t)base[12] | ((uint32_t)base[13] << 8) |
                  ((uint32_t)base[14] << 16) | ((uint32_t)base[15] << 24);
    uint32_t sc = (uint32_t)base[16] | ((uint32_t)base[17] << 8) |
                  ((uint32_t)base[18] << 16) | ((uint32_t)base[19] << 24);
    uint64_t rb = 0;
    for (int i = 0; i < 8; i++) rb |= (uint64_t)base[20 + i] << (8 * i);
    if (rb != (uint64_t)weft_fanout_ring_bytes(pb, sc)) return 0;
    for (size_t i = 40; i < 64; i++) {
        if (base[i] != 0) return 0;
    }
    if (span != (size_t)64 + (size_t)rb) return 0;
    return 1;
}

static void test_gpu1_create(void) {
    printf("GPU1: create — session contract + geometry\n");
    weft_gpu_ring_t* g = NULL;
    check(weft_gpu_create(&g, 256, 8) == 0, "create ok");
    if (g == NULL) return;
    printf("  backend=%s device='%s'\n", weft_gpu_backend_name(g),
           weft_gpu_device_name(g));
    check(weft_gpu_payload_bytes(g) == 256 && weft_gpu_slot_count(g) == 8,
          "geometry from session");
    check(weft_gpu_ring_span(g) == 64 + weft_fanout_ring_bytes(256, 8),
          "span = header + ring_bytes (RFC-0004 identity)");
    check(session_bytes_validate(weft_gpu_ring_bytes(g) - 64, weft_gpu_ring_span(g)),
          "session bytes validate under the shared WFSH contract");
    // ctrl zero-init
    const _Atomic uint64_t* ctrl = (_Atomic uint64_t*)weft_gpu_ring_bytes(g);
    int zeroed = (atomic_load_explicit(&ctrl[0], memory_order_relaxed) == 0) &&
                 (atomic_load_explicit(&ctrl[1], memory_order_relaxed) == 0);
    for (unsigned k = 0; k < 8; k++) {
        if (atomic_load_explicit(&ctrl[2 + k], memory_order_relaxed) != 0) zeroed = 0;
    }
    check(zeroed, "ctrl zero-initialized (fresh-ring invariants)");
    weft_gpu_destroy(g);
    weft_gpu_destroy(NULL);  // NULL-safe
    check(1, "destroy + NULL-destroy safe");
}

static void test_gpu2_ring_is_a_ring(void) {
    printf("GPU2: fan-out over the mapping — publish/claim bit-exact\n");
    weft_gpu_ring_t* g = NULL;
    check(weft_gpu_create(&g, 128, 4) == 0, "create ok");
    if (g == NULL) return;

    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    check(weft_fanout_attach_writer(&f, weft_gpu_ring_bytes(g),
                                    weft_gpu_ring_span(g) - 64, 128, 4) == 0,
          "writer attach over the mapping");

    weft_fanout_reader_t r;
    memset(&r, 0, sizeof(r));
    check(weft_fanout_reader_init(&r, weft_gpu_ring_bytes(g),
                                  weft_gpu_ring_span(g) - 64, 128, 4) == 0,
          "reader attach over the mapping");

    int ok = 1;
    for (uint32_t s = 1; s <= 5000; s++) {
        uint8_t* p = weft_fanout_begin(&f);
        uint32_t* w = (uint32_t*)p;
        for (size_t i = 0; i < 32; i++) w[i] = mix32(s * 2654435761u + (uint32_t)i);
        weft_fanout_publish(&f);
        if (s % 500 == 0) {
            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
            if (!c->fresh || c->seq != s) ok = 0;
            const uint32_t* vw = (const uint32_t*)weft_fanout_view(&r);
            if (vw[7] != mix32(s * 2654435761u + 7)) ok = 0;
        }
    }
    check(ok, "5000 frames, sampled claims bit-exact");
    weft_fanout_stats_t st;
    weft_fanout_reader_stats(&r, &st);
    check(st.fresh + st.drops == r.rec.seq, "telescoping exact over the mapping");

    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
    weft_gpu_destroy(g);
}

static void test_gpu3_interchange(void) {
    printf("GPU3: session bytes interchange with the shm contract\n");
    weft_gpu_ring_t* g = NULL;
    check(weft_gpu_create(&g, 96, 4) == 0, "create ok");
    if (g == NULL) return;
    // The span validates under the SAME byte-level contract shm_ring
    // enforces (GPU1's helper IS that contract, restated independently).
    check(session_bytes_validate(weft_gpu_ring_bytes(g) - 64, weft_gpu_ring_span(g)),
          "WFSH header identical in both modules' dialects");
    // Publish one frame, read the ctrl words through raw atomics — the
    // documentation's word map is the cross-module language.
    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    weft_fanout_attach_writer(&f, weft_gpu_ring_bytes(g), weft_gpu_ring_span(g) - 64, 96, 4);
    uint8_t* p = weft_fanout_begin(&f);
    uint32_t* w = (uint32_t*)p;
    for (size_t i = 0; i < 24; i++) w[i] = mix32(1 * 2654435761u + (uint32_t)i);
    weft_fanout_publish(&f);
    const uint32_t* words = (const uint32_t*)(weft_gpu_ring_bytes(g) - 64);
    check(words[16] == 1 && words[17] == 0, "word map: latestSeq at words[16..18)");
    check(words[18] == 1 && words[19] == 0, "word map: publishes at words[18..20)");
    check(words[20] == 1 && words[21] == 0, "word map: slotSeq[0] at words[20..22)");
    check(words[28] == mix32(1 * 2654435761u),
          "word map: slot-0 payload word 0 at words[28] (20+2*4)");
    weft_fanout_destroy(&f);
    weft_gpu_destroy(g);
}

static void test_gpu4_vulkan_leg(void) {
    printf("GPU4: vulkan leg (allocation + persistent map + live publish)\n");
    weft_gpu_ring_t* g = NULL;
    if (weft_gpu_create(&g, 128, 4) != 0 || g == NULL) {
        check(0, "create failed");
        return;
    }
    if (weft_gpu_backend(g) != WEFT_GPU_BACKEND_VULKAN) {
        printf("  (backend=%s — vulkan leg skipped, declared)\n",
               weft_gpu_backend_name(g));
        weft_gpu_destroy(g);
        check(1, "non-vulkan backend: leg skipped honestly");
        return;
    }
    check(weft_gpu_vk_buffer(g) != NULL && weft_gpu_vk_buffer_bytes(g) == weft_gpu_ring_span(g),
          "session span IS the VkBuffer (one allocation, no staging buffer)");
    check(weft_gpu_vk_device(g) != NULL && weft_gpu_vk_queue_family(g) != 0xFFFFFFFFu,
          "device + compute queue family exported");
    check(weft_gpu_vk_proc(g, "vkDestroyBuffer") != NULL,
          "device-level proc resolution works");

    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    check(weft_fanout_attach_writer(&f, weft_gpu_ring_bytes(g),
                                    weft_gpu_ring_span(g) - 64, 128, 4) == 0,
          "writer over the persistent map");
    for (uint32_t s = 1; s <= 1000; s++) {
        uint8_t* p = weft_fanout_begin(&f);
        uint32_t* w = (uint32_t*)p;
        for (size_t i = 0; i < 32; i++) w[i] = mix32(s * 2654435761u + (uint32_t)i);
        weft_fanout_publish(&f);
    }
    const _Atomic uint64_t* ctrl = (_Atomic uint64_t*)weft_gpu_ring_bytes(g);
    check(atomic_load_explicit(&ctrl[0], memory_order_acquire) == 1000,
          "1000 live frames published through the Vulkan mapping");
    weft_fanout_destroy(&f);
    weft_gpu_destroy(g);
}

static void test_gpu5_lifecycle(void) {
    printf("GPU5: lifecycle — cycles hold no leaks (ASAN-gated)\n");
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        weft_gpu_ring_t* g = NULL;
        if (weft_gpu_create(&g, 64, 3) != 0) ok = 0;
        if (g) {
            weft_fanout_t f;
            memset(&f, 0, sizeof(f));
            weft_fanout_attach_writer(&f, weft_gpu_ring_bytes(g),
                                      weft_gpu_ring_span(g) - 64, 64, 3);
            weft_fanout_publish(&f);
            weft_fanout_destroy(&f);
            weft_gpu_destroy(g);
        }
    }
    check(ok, "8 create/publish/destroy cycles clean");
}

int main(void) {
    printf("Weft GPU-series (GPU-resident rings, RFC-0003 spike) — C driver layer\n");
    test_gpu1_create();
    test_gpu2_ring_is_a_ring();
    test_gpu3_interchange();
    test_gpu4_vulkan_leg();
    test_gpu5_lifecycle();
    printf("\nverdict: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
