// weft_dmabuf_test.c — DB-series conformance gates (RFC-0016 §3).
//
// The honest two-leg design (the uring_rx/gpu_ring pattern):
//   * Every host runs the SUBSTRATE gates: geometry, the WFSH dialect
//     byte-identity, the mmap-aliasing zero-copy proof (memfd stand-in —
//     one physical allocation, two mappings, live fanout traffic), the
//     foreign-import roads, cache-sync refusal, and the fork torture.
//   * Hosts WITH /dev/dma_heap additionally run the heap-alloc leg inside
//     DB3 (alloc -> validate -> publish/claim -> free). Hosts WITHOUT it
//     assert the honest refusal instead. Either leg passes; the evidence
//     line records which one ran.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fanout.h"
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

static void mixer_fill(uint32_t* words, size_t n, uint64_t seq) {
    for (size_t i = 0; i < n; i++)
        words[i] = (uint32_t)(0x9E3779B9u * (seq + 1) + 0x85EBCA6Bu * i);
}

int main(void) {
    printf("# DB-series: weft_dmabuf conformance (RFC-0016 s3)\n");

    // ---- DB1: capability probe honesty ---------------------------------
    printf("## DB1 capability probe\n");
    {
        const weft_dmabuf_probe_t* p = weft_dmabuf_probe();
        char line[192];
        weft_dmabuf_report(line, sizeof(line));
        printf("  %s\n", line);
        CHECK(p->probed, "probe ran once", "probed=%d", p->probed);
        CHECK(p->page_size > 0, "page size reported", "%ld", p->page_size);
        if (p->caps == WEFT_DMABUF_HEAP_SYSTEM) {
            CHECK(p->heap_path[0] != '\0', "heap path recorded", "empty");
            printf("  (heap leg LIVE on this host: %s)\n", p->heap_path);
        } else {
            CHECK(p->caps == WEFT_DMABUF_UNSUPPORTED, "refusal is UNSUPPORTED",
                  "caps=%d", p->caps);
            CHECK(p->heap_path[0] == '\0', "no phantom heap path", "'%s'", p->heap_path);
        }
    }

    // ---- DB2: geometry identities --------------------------------------
    printf("## DB2 geometry\n");
    {
        static const struct { size_t pb; unsigned sc; } geo[] = {
            { 256, 4 }, { 1024, 8 }, { 4096, 16 }, { 96, 64 },
        };
        int ok = 1, ok_bad = 1;
        for (size_t i = 0; i < sizeof(geo) / sizeof(geo[0]); i++) {
            const size_t rb = weft_fanout_ring_bytes(geo[i].pb, geo[i].sc);
            if (weft_dmabuf_span_bytes(geo[i].pb, geo[i].sc) != 64u + rb) ok = 0;
        }
        CHECK(ok, "span = 64 + ring_bytes across geometries", "%d", ok);
        if (weft_dmabuf_span_bytes(0, 8) != 0 || weft_dmabuf_span_bytes(3, 8) != 0 ||
            weft_dmabuf_span_bytes(256, 0) != 0 || weft_dmabuf_span_bytes(256, 1) != 0 ||
            weft_dmabuf_span_bytes(256, 65) != 0)
            ok_bad = 0;
        CHECK(ok_bad, "bad geometries refused (span 0)", "%d", ok_bad);
    }

    // ---- DB3: heap allocation (live leg) or honest refusal --------------
    printf("## DB3 heap allocation / honest refusal\n");
    {
        const weft_dmabuf_probe_t* p = weft_dmabuf_probe();
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        if (p->caps == WEFT_DMABUF_HEAP_SYSTEM) {
            int rc = weft_dmabuf_ring_alloc(&r, 512, 8);
            CHECK(rc == 0, "heap alloc ok", "rc=%d errno=%d", rc, errno);
            CHECK(r.fd >= 0 && r.base != NULL && r.ring == r.base + 64,
                  "session view wired", "fd=%d", r.fd);
            CHECK(r.map_bytes >= r.span_bytes, "page-rounded map covers span",
                  "map=%zu span=%zu", r.map_bytes, r.span_bytes);
            // fresh-ring invariants through the fanout API
            weft_fanout_t f;
            memset(&f, 0, sizeof(f));
            CHECK(weft_fanout_attach_writer(&f, r.ring,
                      weft_fanout_ring_bytes(r.payload_bytes, r.slot_count),
                      r.payload_bytes, r.slot_count) == 0,
                  "writer attach over heap allocation", "errno=%d", errno);
            weft_fanout_reader_t rd;
            CHECK(weft_fanout_reader_init(&rd, r.ring,
                      weft_fanout_ring_bytes(r.payload_bytes, r.slot_count),
                      r.payload_bytes, r.slot_count) == 0,
                  "reader attach over heap allocation", "errno=%d", errno);
            int fresh_ok = 1, exact = 1;
            uint64_t last = 0, drops = 0;
            for (uint64_t s = 1; s <= 1000; s++) {
                uint8_t* cur = weft_fanout_begin(&f);
                mixer_fill((uint32_t*)cur, 128, s);
                weft_fanout_publish(&f);
                const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
                if (c->fresh) {
                    if (memcmp(weft_fanout_view(&rd), (const void*)cur, 512) != 0)
                        fresh_ok = 0;
                    if (c->seq != s) exact = 0;
                    drops += c->dropped;
                    last = c->seq;
                }
            }
            CHECK(fresh_ok, "1000 frames: claims bit-exact", "%d", fresh_ok);
            CHECK(exact && drops == 0 && last == 1000,
                  "telescoping exact (no drops single reader)",
                  "exact=%d drops=%llu last=%llu", exact,
                  (unsigned long long)drops, (unsigned long long)last);
            weft_dmabuf_ring_free(&r);
            CHECK(r.base == NULL && r.fd == -1, "free resets handle", "?");
        } else {
            int rc = weft_dmabuf_ring_alloc(&r, 512, 8);
            CHECK(rc == -1, "alloc refused without heap", "rc=%d", rc);
            CHECK(errno == ENOTSUP, "refusal errno ENOTSUP", "errno=%d", errno);
        }
    }

    // ---- DB4: WFSH dialect byte-identity --------------------------------
    printf("## DB4 WFSH dialect byte-identity\n");
    {
        int fd = memfd_create("weft-dmabuf-dialect", 0);
        CHECK(fd >= 0, "memfd substrate created", "fd=%d", fd);
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        const size_t span = weft_dmabuf_span_bytes(384, 6);
        CHECK(ftruncate(fd, (off_t)span) == 0, "substrate sized to span", "%s",
              strerror(errno));
        CHECK(weft_dmabuf_ring_bind_fd(&r, fd, 384, 6) == 0,
              "bind_fd initializes session", "errno=%d", errno);

        weft_shm_map_t ref;
        memset(&ref, 0, sizeof(ref));
        char name[80];
        snprintf(name, sizeof(name), "dmabuf-dialect-%d", (int)getpid());
        CHECK(weft_shm_create_named(name, 384, 6, &ref) == 0,
              "shm reference session created", "errno=%d", errno);

        // deterministic fields (0..27) + reserved zeros (40..64) must match
        int det = memcmp(r.base, ref.base, 28) == 0;
        CHECK(det, "deterministic header bytes identical (0..27]", "%d", det);
        int zeros = 1;
        for (size_t i = 40; i < 64; i++)
            if (r.base[i] != 0 || ref.base[i] != 0) zeros = 0;
        CHECK(zeros, "reserved [40,64) zero in both dialects", "%d", zeros);
        const uint32_t rb_field = (uint32_t)r.base[20] | ((uint32_t)r.base[21] << 8) |
                                  ((uint32_t)r.base[22] << 16) | ((uint32_t)r.base[23] << 24);
        CHECK(rb_field == (uint32_t)weft_fanout_ring_bytes(384, 6),
              "ring_bytes field matches geometry", "%u", rb_field);

        // cross-dialect attach: a shm attacher accepts the dmabuf-substrate
        // bytes (exact-size road) — the two modules share ONE contract.
        // NOTE OWNERSHIP: weft_shm_attach_fd ADOPTS the fd (destroy closes
        // it) — hand it a dup so the substrate fd stays ours.
        weft_shm_map_t cross;
        memset(&cross, 0, sizeof(cross));
        CHECK(weft_shm_attach_fd(dup(fd), &cross, 0) == 0,
              "shm attach_fd accepts the substrate session", "errno=%d", errno);
        CHECK(weft_shm_payload_bytes(&cross) == 384 && weft_shm_slot_count(&cross) == 6,
              "cross-module geometry agrees", "pb=%zu sc=%u",
              weft_shm_payload_bytes(&cross), weft_shm_slot_count(&cross));

        weft_shm_destroy(&cross);
        weft_shm_destroy(&ref);
        weft_dmabuf_ring_free(&r);
        close(fd);
    }

    // ---- DB5: mmap-aliasing zero-copy substrate ---------------------------
    printf("## DB5 aliasing substrate (one allocation, two mappings)\n");
    {
        int fd = memfd_create("weft-dmabuf-alias", 0);
        CHECK(fd >= 0, "substrate created", "fd=%d", fd);
        const size_t pb = 512;
        const unsigned sc = 8;
        const size_t span = weft_dmabuf_span_bytes(pb, sc);
        CHECK(ftruncate(fd, (off_t)span) == 0, "substrate sized", "%s", strerror(errno));

        weft_dmabuf_ring_t a;
        memset(&a, 0, sizeof(a));
        CHECK(weft_dmabuf_ring_bind_fd(&a, fd, pb, sc) == 0, "view A bound", "?");

        // view B: an independent mapping of the SAME allocation — what a
        // second process (or the same process via a fresh mmap) sees.
        uint8_t* b = (uint8_t*)mmap(NULL, span, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
        CHECK(b != MAP_FAILED, "view B mapped", "%s", strerror(errno));

        // raw alias probe: one physical page, two virtual windows
        a.ring[100] = 0xAB;
        uint8_t observed = b[64 + 100];
        CHECK(observed == 0xAB, "byte written via A visible at B", "0x%02X", observed);

        // live fanout traffic across the two views
        weft_fanout_t f;
        memset(&f, 0, sizeof(f));
        CHECK(weft_fanout_attach_writer(&f, a.ring,
                  weft_fanout_ring_bytes(pb, sc), pb, sc) == 0,
              "writer on view A", "errno=%d", errno);
        weft_fanout_reader_t rd;
        CHECK(weft_fanout_reader_init(&rd, b + 64,
                  weft_fanout_ring_bytes(pb, sc), pb, sc) == 0,
              "reader on view B", "errno=%d", errno);
        int exact = 1;
        uint64_t seen = 0;
        for (uint64_t s = 1; s <= 2000; s++) {
            uint8_t* cur = weft_fanout_begin(&f);
            mixer_fill((uint32_t*)cur, pb / 4, s);
            weft_fanout_publish(&f);
            if ((s & 63u) == 0 || s < 8) {
                const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
                if (c->fresh) {
                    if (memcmp(weft_fanout_view(&rd), cur, pb) != 0) exact = 0;
                    seen = c->seq;
                }
            }
        }
        CHECK(exact, "2000 frames: cross-view claims bit-exact", "%d", exact);
        CHECK(seen == 1984, "reader tracked the stream (last sampled claim s=1984)",
              "last=%llu", (unsigned long long)seen);

        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
        munmap(b, span);
        weft_dmabuf_ring_free(&a);
        close(fd);
    }

    // ---- DB6: foreign import roads ---------------------------------------
    printf("## DB6 foreign import\n");
    {
        // session-carrying buffer
        int fd = memfd_create("weft-dmabuf-imp-session", 0);
        const size_t pb = 256;
        const unsigned sc = 4;
        ftruncate(fd, (off_t)weft_dmabuf_span_bytes(pb, sc));
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        weft_dmabuf_ring_bind_fd(&r, fd, pb, sc);
        weft_dmabuf_import_t imp;
        memset(&imp, 0, sizeof(imp));
        CHECK(weft_dmabuf_import_fd(&imp, fd, 0) == 0, "session import maps", "?");
        CHECK(imp.has_session == 1, "session detected + validated", "%d", imp.has_session);
        CHECK(imp.payload_bytes == pb && imp.slot_count == sc,
              "imported geometry agrees", "pb=%zu sc=%u", imp.payload_bytes, imp.slot_count);
        CHECK(imp.ring == imp.base + 64, "imported ring view wired", "?");
        weft_dmabuf_import_free(&imp);
        weft_dmabuf_ring_free(&r);
        close(fd);

        // raw buffer (a camera-frame-shaped payload: no WFSH magic)
        int rfd = memfd_create("weft-dmabuf-imp-raw", 0);
        ftruncate(rfd, 65536);
        uint8_t* raw = (uint8_t*)mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, rfd, 0);
        memset(raw, 0x5A, 65536);
        munmap(raw, 65536);
        weft_dmabuf_import_t raw_imp;
        memset(&raw_imp, 0, sizeof(raw_imp));
        CHECK(weft_dmabuf_import_fd(&raw_imp, rfd, 4096) == 0,
              "raw import maps (clamped)", "?");
        CHECK(raw_imp.has_session == 0, "raw buffer honestly not a session", "%d",
              raw_imp.has_session);
        CHECK(raw_imp.map_bytes == 4096 && raw_imp.base[0] == 0x5A,
              "raw bytes visible through the import", "map=%zu", raw_imp.map_bytes);
        weft_dmabuf_import_free(&raw_imp);
        close(rfd);

        // refusal road: bad fd
        weft_dmabuf_import_t bad;
        memset(&bad, 0, sizeof(bad));
        CHECK(weft_dmabuf_import_fd(&bad, -1, 0) == -1, "bad fd refused", "?");
    }

    // ---- DB7: cache-sync refusal is information ---------------------------
    printf("## DB7 cache sync honesty\n");
    {
        int fd = memfd_create("weft-dmabuf-sync", 0);
        ftruncate(fd, 4096);
        CHECK(weft_dmabuf_sync(fd, 1, 1) == -1,
              "sync refused on non-dma-buf (recorded, not fatal)", "?");
        close(fd);
    }

    // ---- DB8: fork torture over the substrate -----------------------------
    printf("## DB8 fork torture (substrate survives a process boundary)\n");
    {
        int fd = memfd_create("weft-dmabuf-fork", 0);
        const size_t pb = 512;
        const unsigned sc = 8;
        const size_t span = weft_dmabuf_span_bytes(pb, sc);
        ftruncate(fd, (off_t)span);
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        weft_dmabuf_ring_bind_fd(&r, fd, pb, sc);

        uint8_t* parent_view = (uint8_t*)mmap(NULL, span, PROT_READ | PROT_WRITE,
                                              MAP_SHARED, fd, 0);
        CHECK(parent_view != MAP_FAILED, "parent second view", "%s", strerror(errno));

        pid_t pid = fork();
        if (pid == 0) {
            // child: the producer (fd + mapping inherited — same pages)
            weft_fanout_t f;
            memset(&f, 0, sizeof(f));
            if (weft_fanout_attach_writer(&f, r.ring,
                    weft_fanout_ring_bytes(pb, sc), pb, sc) != 0) _exit(42);
            for (uint64_t s = 1; s <= 500; s++) {
                uint8_t* cur = weft_fanout_begin(&f);
                mixer_fill((uint32_t*)cur, pb / 4, s);
                weft_fanout_publish(&f);
            }
            _exit(0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child producer exited clean",
              "st=%d", st);

        weft_fanout_reader_t rd;
        CHECK(weft_fanout_reader_init(&rd, parent_view + 64,
                  weft_fanout_ring_bytes(pb, sc), pb, sc) == 0,
              "parent reader over second view", "?");
        const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
        CHECK(c->fresh && c->seq == 500, "parent claims the child's last frame",
              "fresh=%d seq=%llu", c->fresh, (unsigned long long)c->seq);
        // bit-exactness against the mixer family for the claimed frame
        uint32_t expect[128];
        mixer_fill(expect, 128, 500);
        CHECK(memcmp(weft_fanout_view(&rd), expect, pb) == 0,
              "claimed frame bit-exact across the process boundary", "?");
        CHECK(c->dropped == 500 - 1, "telescoping exact across fork",
              "dropped=%llu", (unsigned long long)c->dropped);

        weft_fanout_reader_destroy(&rd);
        munmap(parent_view, span);
        weft_dmabuf_ring_free(&r);
        close(fd);
    }

    // ---- DB9: fd ownership discipline -------------------------------------
    printf("## DB9 ownership discipline\n");
    {
        int fd = memfd_create("weft-dmabuf-own", 0);
        ftruncate(fd, (off_t)weft_dmabuf_span_bytes(128, 4));
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        weft_dmabuf_ring_bind_fd(&r, fd, 128, 4);
        weft_dmabuf_ring_free(&r);
        struct stat st;
        CHECK(fstat(fd, &st) == 0,
              "borrowed fd survives ring_free (caller owns it)", "errno=%d", errno);
        close(fd);
    }

    printf("verdict: %s (%d passed, %d failed)\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
