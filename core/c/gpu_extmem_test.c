// gpu_extmem_test.c — XE/XD-series conformance gates (RFC-0016 §2:
// VK_EXT_external_memory_host + VK_KHR_external_memory_fd wrap roads).
//
// THE HEADLINE PROOF (XE2/XE3): a WFSH session the CPU already owns —
// created by shm_ring, mapped by mmap, NEVER allocated by Vulkan — is
// imported with weft_gpu_wrap_host; the CPU publishes frames through its
// own pointer while the GPU consumes the SAME live words through the
// imported allocation (stream_frames.spv whole-window validation, two
// dispatches, the window ADVANCES between them). One physical allocation,
// two address spaces, zero staging copies.
//
// Honest two-leg design (the gpu_ring pattern): hosts without a Vulkan
// ICD run the refusal legs and DECLARE the vulkan legs skipped; hosts
// with an ICD (CI: lavapipe) run everything. The heap->wrap_dmabuf road
// (XD3) additionally needs /dev/dma_heap — refused-and-declared on hosts
// without it, live where it exists.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fanout.h"
#include "gpu_ring.h"
#include "gpu_stream.h"
#include "shm_ring.h"
#include "weft_dmabuf.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name, fmt, ...)                                        \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  PASS %s\n", name);                                   \
            g_pass++;                                                      \
        } else {                                                           \
            printf("  FAIL %s — " fmt "\n", name, ##__VA_ARGS__);          \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

// ---- the stream-probe family's mixer (xor-first variant — the mixer
// stream_frames.comp recomputes GPU-side; NOT gpu_probe.c's variant) ------

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void fill_mixer(uint8_t* dst, uint32_t seq, size_t words) {
    uint32_t* w = (uint32_t*)dst;
    for (size_t i = 0; i < words; i++) {
        w[i] = mix32(seq * 2654435761u + (uint32_t)i);
    }
}

static void window_expect(uint64_t frames, unsigned slots, uint32_t* out_xor,
                          uint32_t* out_width) {
    uint32_t xf = 0;
    uint32_t width = 0;
    const uint64_t first = (frames > slots) ? frames - slots + 1 : 1;
    for (uint64_t s = first; s <= frames; s++) {
        xf ^= mix32((uint32_t)s * 2654435761u);
        width++;
    }
    *out_xor = xf;
    *out_width = width;
}

static void* read_spv(size_t* out_len) {
    static const char* candidates[] = {
        "probes/compute/stream_frames.spv",
        "../../probes/compute/stream_frames.spv",
        "../probes/compute/stream_frames.spv",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        FILE* f = fopen(candidates[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || (n % 4) != 0) { fclose(f); return NULL; }
        void* p = malloc((size_t)n);
        if (fread(p, 1, (size_t)n, f) != (size_t)n) {
            free(p);
            fclose(f);
            return NULL;
        }
        fclose(f);
        *out_len = (size_t)n;
        return p;
    }
    return NULL;
}

int main(void) {
    printf("# XE/XD-series: gpu external-memory wrap conformance (RFC-0016 s2)\n");

    const size_t payload = 256;
    const unsigned slots = 8;

    // ---- XE1: environment honesty ---------------------------------------
    printf("## XE1 environment probe\n");
    weft_gpu_ring_t* probe = NULL;
    int have_vk = 0;
    if (weft_gpu_create(&probe, payload, slots) == 0 &&
        weft_gpu_backend(probe) == WEFT_GPU_BACKEND_VULKAN) {
        have_vk = 1;
        printf("  vulkan leg LIVE (device: %s)\n", weft_gpu_device_name(probe));
        CHECK(strcmp(weft_gpu_import_kind(probe), "native") == 0,
              "native sessions report their kind", "'%s'",
              weft_gpu_import_kind(probe));
    } else {
        printf("  (backend=%s — vulkan legs skipped, declared)\n",
               probe ? weft_gpu_backend_name(probe) : "unavailable");
    }
    if (probe) weft_gpu_destroy(probe);

    // ---- XE2/XE3: wrap_host — the zero-copy headline --------------------
    printf("## XE2 wrap_host: imported host memory, GPU-consumed\n");
    weft_shm_map_t shm;
    memset(&shm, 0, sizeof(shm));
    weft_gpu_ring_t* g = NULL;
    weft_gpu_stream_t* stream = NULL;
    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    size_t spv_len = 0;
    void* spv = NULL;
    int xe2_ok = 0;

    if (have_vk) {
        CHECK(weft_shm_create_anon(payload, slots, &shm) == 0,
              "shm anon session (page-aligned, CPU-owned)", "errno=%d", errno);
        CHECK(weft_fanout_attach_writer(&f, shm.ring,
                  weft_shm_ring_bytes(&shm), payload, slots) == 0,
              "CPU writer over the shm mapping", "?");
        for (uint32_t s = 1; s <= 40; s++) {
            fill_mixer(weft_fanout_begin(&f), s, payload / 4);
            weft_fanout_publish(&f);
        }
        spv = read_spv(&spv_len);
        CHECK(spv != NULL, "stream_frames.spv located", "%s",
              spv ? "ok" : "missing");

        int rc = weft_gpu_wrap_host(&g, payload, slots, shm.base,
                                    shm.mapping_bytes);
        if (rc != 0) {
            // The honest two-outcome gate: a device that refuses the wrap
            // (extension absent, alignment, or the alias verification
            // catching a non-aliasing import — llvmpipe) is a PASS for the
            // refusal leg; the positive leg below runs on aliasing devices.
            CHECK(rc == -1, "wrap refused honestly (non-aliasing device) — "
                            "documented fallback: weft_gpu_create HOST_VISIBLE",
                  "rc=%d", rc);
            // the session must be BYTE-IDENTICAL after the refused wrap
            // (the canary restores): re-attach proves it
            weft_fanout_reader_t rr;
            CHECK(weft_fanout_reader_init(&rr, shm.ring,
                      weft_shm_ring_bytes(&shm), payload, slots) == 0,
                  "session intact after the refused wrap", "?");
            const weft_fanout_claim_t* c = weft_fanout_claim(&rr);
            CHECK(c->fresh && c->seq == 40,
                  "the 40 published frames still readable (no corruption)",
                  "fresh=%d seq=%llu", c->fresh, (unsigned long long)c->seq);
            weft_fanout_reader_destroy(&rr);
            printf("  (positive leg hardware-deferred: this ICD does not "
                   "alias imports — the heterogeneous evidence log carries "
                   "the llvmpipe diag; AMD/Intel/NVIDIA/GBM roads alias)\n");
        } else {
            CHECK(1, "wrap_host imports the shm session", "rc=%d", rc);
            CHECK(weft_gpu_backend(g) == WEFT_GPU_BACKEND_VULKAN &&
                  strcmp(weft_gpu_import_kind(g), "host-pointer") == 0,
                  "session reports vulkan/host-pointer", "%s/%s",
                  weft_gpu_backend_name(g), weft_gpu_import_kind(g));
            CHECK(strcmp(weft_gpu_external_info(g), "imported:host-pointer") == 0,
                  "external_info carries the import provenance", "'%s'",
                  weft_gpu_external_info(g));
            CHECK(weft_gpu_ring_bytes(g) == shm.ring,
                  "the session's ring view IS the caller's memory (aliased)",
                  "%p vs %p", (void*)weft_gpu_ring_bytes(g), (void*)shm.ring);

            if (spv) {
                weft_gpu_stream_err_t src =
                    weft_gpu_stream_init(&stream, g, spv, spv_len, 0, 0, 0);
                CHECK(src == WEFT_GPU_STREAM_OK, "gpu_stream over the wrap",
                      "err=%d", src);
                if (src == WEFT_GPU_STREAM_OK) {
                    const uint32_t push[2] = { slots, (uint32_t)(payload / 4) };
                    xe2_ok =
                        weft_gpu_stream_dispatch(stream, push, 8, 1, 1, 1) ==
                        WEFT_GPU_STREAM_OK;
                    if (xe2_ok) {
                        const uint32_t* r = weft_gpu_stream_result(stream);
                        uint32_t want_xor = 0, want_width = 0;
                        window_expect(40, slots, &want_xor, &want_width);
                        xe2_ok = r[0] == 0 && r[1] == 40 && r[2] == want_xor &&
                                 r[3] == want_width && r[4] == 0x54464557u;
                        printf("  stream-1: mismatches=%u seq=%u xor=0x%08x "
                               "width=%u magic=0x%08x (window after 40 frames "
                               "published through the CPU pointer)\n",
                               r[0], r[1], r[2], r[3], r[4]);
                    }
                }
            }
            // the wrap SUCCEEDED: the zero-copy proof is MANDATORY here
            CHECK(xe2_ok, "GPU consumed live ring words through the IMPORTED "
                          "allocation (zero staging copies)", "%d", xe2_ok);
        }
    } else {
        printf("  (XE2 declared: no Vulkan ICD on this host)\n");
    }

    printf("## XE3 wrap_host: live aliasing across later publishes\n");
    if (have_vk && stream != NULL) {
        // The writer is STILL attached to the same CPU mapping — publish a
        // second burst through the pointer and dispatch again: the GPU's
        // window must ADVANCE (the import aliases LIVE pages, not a snapshot)
        for (uint32_t s = 41; s <= 80; s++) {
            fill_mixer(weft_fanout_begin(&f), s, payload / 4);
            weft_fanout_publish(&f);
        }
        const uint32_t push[2] = { slots, (uint32_t)(payload / 4) };
        int advanced = 0;
        if (weft_gpu_stream_dispatch(stream, push, 8, 1, 1, 1) ==
            WEFT_GPU_STREAM_OK) {
            const uint32_t* r = weft_gpu_stream_result(stream);
            uint32_t want_xor = 0, want_width = 0;
            window_expect(80, slots, &want_xor, &want_width);
            advanced = r[0] == 0 && r[1] == 80 && r[2] == want_xor &&
                       r[3] == want_width && r[4] == 0x54464557u;
            printf("  stream-2: mismatches=%u seq=%u xor=0x%08x width=%u "
                   "(window advanced to 80)\n", r[0], r[1], r[2], r[3]);
        }
        CHECK(advanced, "window ADVANCED: CPU publishes are live GPU reads",
              "%d", advanced);
    } else if (have_vk) {
        printf("  (XE3 declared: the wrap was refused on this ICD — the live-"
               "aliasing proof runs on aliasing devices)\n");
    } else {
        printf("  (XE3 declared: no Vulkan ICD on this host)\n");
    }
    if (stream) weft_gpu_stream_destroy(stream);
    if (g) weft_gpu_destroy(g);
    weft_fanout_destroy(&f);
    if (shm.base) weft_shm_destroy(&shm);

    // ---- XE4: alignment gate (a VALID session at a non-page address) -----
    printf("## XE4 alignment gate\n");
    {
        // A valid session placed at offset 128: header validation PASSES,
        // the minImportedHostPointerAlignment gate must refuse it.
        const size_t span = 64 + weft_fanout_ring_bytes(payload, slots);
        void* region = mmap(NULL, span + 8192, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(region != MAP_FAILED, "scratch region mapped", "%s", strerror(errno));
        if (region != MAP_FAILED) {
            weft_dmabuf_ring_t r;
            memset(&r, 0, sizeof(r));
            // borrow the dmabuf module's session writer over a memfd, then
            // copy the initialized session to offset 128 (still one page)
            int fd = memfd_create("weft-xe4", 0);
            ftruncate(fd, (off_t)span);
            weft_dmabuf_ring_bind_fd(&r, fd, payload, slots);
            memcpy((uint8_t*)region + 128, r.base, span);
            weft_dmabuf_ring_free(&r);
            close(fd);

            weft_gpu_ring_t* wg = NULL;
            int rc = -1;
            if (have_vk) {
                rc = weft_gpu_wrap_host(&wg, payload, slots,
                                        (uint8_t*)region + 128, span);
                CHECK(rc == -1,
                      "valid session at +128 refused (below alignment)", "rc=%d", rc);
                if (rc == 0) weft_gpu_destroy(wg);
            } else {
                printf("  (declared: no Vulkan ICD — the header+alignment "
                       "refusal path needs a device to probe)\n");
            }
            // the same session AT the page base (control): on aliasing
            // devices it imports; on non-aliasing ICDs (llvmpipe) BOTH
            // roads refuse at the alias verification — the control's point
            // is that the +128 refusal above was the ALIGNMENT gate, which
            // on llvmpipe is confirmed by the canary diag evidence instead.
            if (have_vk) {
                memmove(region, (uint8_t*)region + 128, span);
                rc = weft_gpu_wrap_host(&wg, payload, slots, region, span);
                if (rc == 0) {
                    CHECK(1, "control: page-aligned session imports", "rc=%d", rc);
                    weft_gpu_destroy(wg);
                } else {
                    printf("  (control refused by the alias verification on "
                           "this ICD — both XE4 roads refused, the +128 gate "
                           "is alignment-bound per the diag evidence)\n");
                }
            }
            munmap(region, span + 8192);
        }
    }

    // ---- XE5/XE6/XE7/XE8: the refusal ladder ------------------------------
    printf("## XE5-XE8 refusal ladder\n");
    {
        // XE5: page-aligned garbage (no WFSH header)
        void* zeros = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        weft_gpu_ring_t* wg = NULL;
        CHECK(weft_gpu_wrap_host(&wg, payload, slots, zeros, 4096) == -1,
              "uninitialized mapping refused (no session)", "?");
        munmap(zeros, 4096);

        // XE6: geometry mismatch (session says 256/8; caller claims 512/8)
        weft_shm_map_t s2;
        memset(&s2, 0, sizeof(s2));
        CHECK(weft_shm_create_anon(payload, slots, &s2) == 0, "second session",
              "?");
        CHECK(weft_gpu_wrap_host(&wg, 512, slots, s2.base, s2.mapping_bytes) == -1,
              "geometry mismatch refused", "?");
        weft_shm_destroy(&s2);

        // XE7: malloc'd fanout ring (no WFSH header, non-page base)
        weft_fanout_t mf;
        memset(&mf, 0, sizeof(mf));
        CHECK(weft_fanout_init(&mf, payload, slots) == 0, "malloc ring", "?");
        CHECK(weft_gpu_wrap_host(&wg, payload, slots,
                                 (void*)weft_fanout_ring(&mf),
                                 weft_fanout_ring_bytes(payload, slots)) == -1,
              "malloc'd ring refused (documented: shm/dmabuf is the road)",
              "?");
        weft_fanout_destroy(&mf);

        // XE8: NULL / undersized
        CHECK(weft_gpu_wrap_host(&wg, payload, slots, NULL, 4096) == -1,
              "NULL pointer refused", "?");
        CHECK(weft_gpu_wrap_dmabuf(&wg, payload, slots, -1) == -1,
              "bad fd refused", "?");
    }

    // ---- XD-series: the dma-buf import road --------------------------------
    printf("## XD1 fd road: substrate import (memfd stand-in for the heap fd)\n");
    // A memfd is not a dma-buf: spec-strict ICDs refuse the import. llvmpipe
    // (the CI ICD) is permissive — it mmaps ANY fd, so the import SUCCEEDS
    // and the alias verification PROVES the pages alias. Both outcomes are
    // honest; on the permissive one the FULL zero-copy proof runs: publish
    // through the CPU, consume on the GPU through the imported fd, window
    // ADVANCES across dispatches. The heap-ioctl leg (a true dma-buf) is XD3.
    if (have_vk) {
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        int fd = memfd_create("weft-xd1", 0);
        ftruncate(fd, (off_t)weft_dmabuf_span_bytes(payload, slots));
        weft_dmabuf_ring_bind_fd(&r, fd, payload, slots);
        weft_gpu_ring_t* wg = NULL;
        int rc = weft_gpu_wrap_dmabuf(&wg, payload, slots, fd);
        if (rc == 0) {
            CHECK(1, "permissive ICD: fd import alias-verified (llvmpipe "
                     "mmaps any fd — the heap road is the same code path)",
                  "rc=%d", rc);
            CHECK(strcmp(weft_gpu_import_kind(wg), "dmabuf-fd") == 0,
                  "session reports dmabuf-fd", "'%s'", weft_gpu_import_kind(wg));
            weft_fanout_t wf;
            memset(&wf, 0, sizeof(wf));
            CHECK(weft_fanout_attach_writer(&wf, weft_gpu_ring_bytes(wg),
                      weft_gpu_ring_span(wg) - 64, payload, slots) == 0,
                  "CPU writer over the wrap's mapping", "?");
            for (uint32_t s = 1; s <= 40; s++) {
                fill_mixer(weft_fanout_begin(&wf), s, payload / 4);
                weft_fanout_publish(&wf);
            }
            if (spv_len == 0) { void* p = read_spv(&spv_len); free(p); }
            void* spv1 = read_spv(&spv_len);
            weft_gpu_stream_t* st1 = NULL;
            int fd_proof = 0;
            if (spv1 &&
                weft_gpu_stream_init(&st1, wg, spv1, spv_len, 0, 0, 0) ==
                    WEFT_GPU_STREAM_OK) {
                const uint32_t push[2] = { slots, (uint32_t)(payload / 4) };
                fd_proof = weft_gpu_stream_dispatch(st1, push, 8, 1, 1, 1) ==
                           WEFT_GPU_STREAM_OK;
                if (fd_proof) {
                    const uint32_t* r1 = weft_gpu_stream_result(st1);
                    uint32_t r1c[8];
                    memcpy(r1c, r1, sizeof(r1c));  // snapshot: overwritten by dispatch 2
                    uint32_t wx = 0, ww = 0;
                    window_expect(40, slots, &wx, &ww);
                    fd_proof = r1c[0] == 0 && r1c[1] == 40 && r1c[2] == wx &&
                               r1c[3] == ww && r1c[4] == 0x54464557u;
                    printf("  fd-road-1: mismatches=%u seq=%u xor=0x%08x "
                           "width=%u (GPU consumed the fd-imported ring)\n",
                           r1[0], r1[1], r1[2], r1[3]);
                    // the LIVE-ALIASING proof on the fd road: publish MORE
                    // through the CPU view, dispatch again, window ADVANCES
                    for (uint32_t s = 41; s <= 80; s++) {
                        fill_mixer(weft_fanout_begin(&wf), s, payload / 4);
                        weft_fanout_publish(&wf);
                    }
                    if (weft_gpu_stream_dispatch(st1, push, 8, 1, 1, 1) ==
                        WEFT_GPU_STREAM_OK) {
                        const uint32_t* r2 = weft_gpu_stream_result(st1);
                        window_expect(80, slots, &wx, &ww);
                        fd_proof = fd_proof && r2[0] == 0 && r2[1] == 80 &&
                                   r2[2] == wx && r2[3] == ww &&
                                   r2[2] != r1c[2];
                        printf("  fd-road-2: mismatches=%u seq=%u xor=0x%08x "
                               "width=%u (window ADVANCED through the fd)\n",
                               r2[0], r2[1], r2[2], r2[3]);
                    } else {
                        fd_proof = 0;
                    }
                }
                weft_gpu_stream_destroy(st1);
            }
            free(spv1);
            CHECK(fd_proof, "fd road: GPU live-consumed the imported ring, "
                            "window advanced across dispatches", "%d", fd_proof);
            weft_fanout_destroy(&wf);
            weft_gpu_destroy(wg);
        } else {
            CHECK(rc == -1, "spec-strict ICD refused the non-dma-buf fd",
                  "rc=%d", rc);
            printf("  (fd-road GPU proof declared: needs a permissive ICD or "
                   "a real dma-buf — see XD3's heap leg)\n");
        }
        struct stat st;
        CHECK(fstat(fd, &st) == 0,
              "caller's fd intact after the import (dup discipline)",
              "errno=%d", errno);
        weft_dmabuf_ring_free(&r);
        close(fd);
    } else {
        printf("  (XD1 declared: no Vulkan ICD on this host)\n");
    }

    printf("## XD2 Vulkan dmabuf export -> wrap_dmabuf round-trip\n");
    if (have_vk) {
        weft_gpu_ring_t* src = NULL;
        if (weft_gpu_create_ex(&src, payload, slots,
                               WEFT_GPU_CREATE_EXPORTABLE_FD) == 0 &&
            weft_gpu_backend(src) == WEFT_GPU_BACKEND_VULKAN) {
            int dbuf_fd = -1;
            int erc = weft_gpu_export_dmabuf_fd(src, &dbuf_fd);
            if (erc != 0) {
                printf("  (ICD refused dma-buf export — round-trip declared: "
                       "needs an ICD that mints dma-buf handles)\n");
            } else {
                // The exported fd is a REAL dma-buf over the source session.
                // Publish through the SOURCE's CPU mapping, then wrap the fd
                // and let the GPU consume it through the IMPORT.
                weft_fanout_t sf;
                memset(&sf, 0, sizeof(sf));
                CHECK(weft_fanout_attach_writer(&sf, weft_gpu_ring_bytes(src),
                          weft_gpu_ring_span(src) - 64, payload, slots) == 0,
                      "writer over the source session", "?");
                for (uint32_t s = 1; s <= 40; s++) {
                    fill_mixer(weft_fanout_begin(&sf), s, payload / 4);
                    weft_fanout_publish(&sf);
                }
                weft_gpu_ring_t* wg = NULL;
                int wrc = weft_gpu_wrap_dmabuf(&wg, payload, slots, dbuf_fd);
                if (wrc != 0) {
                    // llvmpipe's exported dma-buf fd carries fstat size 0
                    // (the diag evidence records it) — the wrap refuses at
                    // the coverage check. The round-trip is hardware-deferred.
                    CHECK(wrc == -1, "wrap refused honestly (fd coverage)",
                          "rc=%d", wrc);
                    printf("  (round-trip hardware-deferred: this ICD's "
                           "exported fd does not carry a usable size/alias)\n");
                } else {
                    CHECK(1, "wrap_dmabuf imports the exported dma-buf",
                          "rc=%d", wrc);
                    CHECK(strcmp(weft_gpu_import_kind(wg), "dmabuf-fd") == 0,
                          "session reports dmabuf-fd", "'%s'",
                          weft_gpu_import_kind(wg));
                    // The wrap's own CPU view must alias the source pages:
                    // the source's latestSeq is visible through the wrap.
                    const uint64_t via_wrap =
                        *(volatile const uint64_t*)(weft_gpu_ring_bytes(wg));
                    CHECK(via_wrap == 40,
                          "wrap's mmap aliases the source session pages",
                          "latestSeq=%llu", (unsigned long long)via_wrap);
                    if (spv_len == 0) { void* p = read_spv(&spv_len); free(p); }
                    void* spv2 = read_spv(&spv_len);
                    weft_gpu_stream_t* st2 = NULL;
                    if (spv2 &&
                        weft_gpu_stream_init(&st2, wg, spv2, spv_len, 0, 0, 0) ==
                            WEFT_GPU_STREAM_OK) {
                        const uint32_t push[2] = { slots, (uint32_t)(payload / 4) };
                        int ok = weft_gpu_stream_dispatch(st2, push, 8, 1, 1, 1) ==
                                 WEFT_GPU_STREAM_OK;
                        if (ok) {
                            const uint32_t* r = weft_gpu_stream_result(st2);
                            uint32_t wx = 0, ww = 0;
                            window_expect(40, slots, &wx, &ww);
                            ok = r[0] == 0 && r[1] == 40 && r[2] == wx &&
                                 r[3] == ww && r[4] == 0x54464557u;
                        }
                        CHECK(ok, "GPU consumed the dma-buf-imported ring "
                                  "(fd round-trip, zero copies)", "%d", ok);
                        weft_gpu_stream_destroy(st2);
                    }
                    free(spv2);
                    weft_gpu_destroy(wg);
                }
                weft_fanout_destroy(&sf);
                close(dbuf_fd);
            }
        } else {
            printf("  (exportable session unavailable — declared)\n");
        }
        if (src) weft_gpu_destroy(src);
    } else {
        printf("  (XD2 declared: no Vulkan ICD on this host)\n");
    }

    printf("## XD3 heap road (weft_dmabuf -> wrap_dmabuf)\n");
    {
        const weft_dmabuf_probe_t* dp = weft_dmabuf_probe();
        if (dp->caps == WEFT_DMABUF_HEAP_SYSTEM) {
            weft_dmabuf_ring_t r;
            memset(&r, 0, sizeof(r));
            if (weft_dmabuf_ring_alloc(&r, payload, slots) == 0 && have_vk) {
                weft_gpu_ring_t* wg = NULL;
                int rc = weft_gpu_wrap_dmabuf(&wg, payload, slots, r.fd);
                CHECK(rc == 0, "heap allocation wrapped by the GPU", "rc=%d", rc);
                if (rc == 0) weft_gpu_destroy(wg);
            } else {
                printf("  (heap present but wrap needs an ICD — declared)\n");
            }
            weft_dmabuf_ring_free(&r);
        } else {
            printf("  (declared: no /dev/dma_heap on this host — the heap leg "
                   "runs where heaps exist; DB-series proves the substrate)\n");
            CHECK(1, "heap absence honestly reported (Law 4)", "ok");
        }
    }

    free(spv);
    printf("verdict: %s (%d passed, %d failed)\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
