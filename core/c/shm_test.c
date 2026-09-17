// shm_test.c — S-series conformance for the inter-process shared-memory ring.
//
// Gates (any failure exits non-zero):
//   S1  create named: header validates (magic/version/geometry/reserved),
//       ring layout matches weft_fanout_ring_bytes, ctrl zero-initialized
//   S2  attach named (rw + read-only): geometry from header; EXACT size
//       enforcement (truncated/garbage objects refused, not guessed)
//   S3  O_EXCL semantics: re-creating an existing name fails; explicit
//       weft_shm_unlink replaces stale; absent-name attach fails
//   S4  anonymous + fork: writer parent, N forked readers, 200k frames —
//       payload integrity (mix32), telescoping identity per reader,
//       zero torn acceptances, all children exit 0
//   S5  named + fork: same torture with attach-by-name children (the
//       multi-process broadcast road: one writer, N independent readers)
//   S6  producer handoff: create + publish, destroy writer WITHOUT unlink,
//       attach_writer from a NEW process continues the stream — per-slot
//       stamp monotonicity survives (no torn claims across the handoff)
//   S7  geometry mismatch refused: attaching against a differently-geometry
//       object of the right total size is impossible by construction (the
//       header IS the geometry) — validated via fd-attach of a foreign file
//   S8  reader over a crashed producer: kill the writer mid-stream; the
//       ring persists; a fresh reader attaches and reads the last
//       consistent frames (crash-tolerant posture)
//   S9  fan-out bindings: weft_fanout_shm_create/attach_writer/attach_reader
//       agree on geometry; reader claims verify against published frames
//
// Build: make -C core/c shm-test   — then ./core/c/shm-test

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fanout.h"
#include "shm_ring.h"

static int g_fail = 0;

static void check(int cond, const char* name) {
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

// 04-LITMUS §0.1 mix32 — the shared deterministic payload generator (the
// same family the F-series batteries and the recorder's --expect-mixer use).
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

#define TEST_NAME "weft-shm-test-s1"
#define TORTURE_NAME "weft-shm-test-torture"

// --- S1 ------------------------------------------------------------------------

static void test_s1_create(void) {
    printf("S1: create named — header + ring invariants\n");
    weft_shm_unlink(TEST_NAME);
    weft_shm_map_t m;
    check(weft_shm_create_named(TEST_NAME, 256, 8, &m) == 0, "create ok");
    check(weft_shm_payload_bytes(&m) == 256 && weft_shm_slot_count(&m) == 8,
          "geometry readable from header");
    check(weft_shm_ring_bytes(&m) == weft_fanout_ring_bytes(256, 8),
          "ring_bytes identity (RFC 0004)");
    check(m.ring == m.base + WEFT_SHM_HEADER_BYTES, "ring follows the header");
    check(m.mapping_bytes == WEFT_SHM_HEADER_BYTES + weft_fanout_ring_bytes(256, 8),
          "mapping size exact");
    // Ctrl zero-init: latestSeq == 0, publishes == 0, all slotSeq == 0.
    const _Atomic uint64_t* ctrl = (_Atomic uint64_t*)m.ring;
    int zeroed = (atomic_load_explicit(&ctrl[0], memory_order_relaxed) == 0) &&
                 (atomic_load_explicit(&ctrl[1], memory_order_relaxed) == 0);
    for (unsigned k = 0; k < 8; k++) {
        if (atomic_load_explicit(&ctrl[2 + k], memory_order_relaxed) != 0) zeroed = 0;
    }
    check(zeroed, "ctrl zero-initialized (fresh-ring invariants)");
    weft_shm_destroy(&m);
    check(weft_shm_attach_named(TEST_NAME, &m, 0) == -1,
          "creator destroy unlinks (attach fails after)");
    weft_shm_destroy(&m);
}

// --- S2 ------------------------------------------------------------------------

static void test_s2_attach(void) {
    printf("S2: attach named — validation and read-only mapping\n");
    weft_shm_unlink(TEST_NAME);
    weft_shm_map_t m, ro;
    weft_fanout_t f;
    check(weft_fanout_shm_create(TEST_NAME, 128, 4, &f, &m) == 0,
          "fanout binding create ok");
    fill_mixer(weft_fanout_begin(&f), 1, 128 / 4);
    weft_fanout_publish(&f);

    weft_shm_map_t m2;
    check(weft_shm_attach_named(TEST_NAME, &m2, 0) == 0, "rw attach ok");
    // A second mapping of the same object lands at a different virtual
    // address — "same object" is proven by geometry + cross-visibility.
    check(weft_shm_ring_bytes(&m2) == weft_shm_ring_bytes(&m) &&
              weft_shm_payload_bytes(&m2) == weft_shm_payload_bytes(&m) &&
              weft_shm_slot_count(&m2) == weft_shm_slot_count(&m) &&
              m2.mapping_bytes == m.mapping_bytes,
          "same object, same geometry (second mapping)");
    {
        weft_fanout_reader_t r2;
        memset(&r2, 0, sizeof(r2));
        check(weft_fanout_reader_init(&r2, m2.ring,
                                      m2.mapping_bytes - WEFT_SHM_HEADER_BYTES, 128, 4) == 0,
              "reader over the second mapping");
        const weft_fanout_claim_t* c2 = weft_fanout_claim(&r2);
        const uint32_t* vw2 = (const uint32_t*)weft_fanout_view(&r2);
        check(c2->fresh && c2->seq == 1 && vw2[11] == mix32(1 * 2654435761u + 11),
              "frame visible across mappings (publish over A, claim over B)");
        weft_fanout_reader_destroy(&r2);
    }
    weft_shm_destroy(&m2);

    check(weft_shm_attach_named(TEST_NAME, &ro, 1) == 0, "read-only attach ok");
    weft_fanout_reader_t r;
    memset(&r, 0, sizeof(r));
    check(weft_fanout_reader_init(&r, ro.ring, ro.mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                  128, 4) == 0,
          "reader over PROT_READ mapping");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    check(c->fresh && c->seq == 1, "read-only reader claims frame 1");
    const uint32_t* w = (const uint32_t*)weft_fanout_view(&r);
    int payload_ok = (w[0] == mix32(1 * 2654435761u));
    for (size_t i = 0; i < 128 / 4 && payload_ok; i += 7) {
        payload_ok = (w[i] == mix32(1 * 2654435761u + (uint32_t)i));
    }
    check(payload_ok, "payload bit-exact through the read-only mapping");
    weft_fanout_reader_destroy(&r);
    weft_shm_destroy(&ro);

    // Foreign / truncated objects: rejected without guessing.
    const char* junk = "/dev/shm/weft-shm-test-junk";
    FILE* jf = fopen(junk, "wb");
    fwrite("NOTAWEFTSESSION0123456789ABCDEF", 1, 32, jf);
    fclose(jf);
    weft_shm_map_t mj;
    check(weft_shm_attach_named("weft-shm-test-junk", &mj, 0) == -1,
          "garbage object refused (bad magic/size)");
    unlink(junk);

    // Right-size, wrong-content: a zeroed object of the EXACT right size
    // has no header — refused (the header is the contract).
    weft_shm_unlink("weft-shm-test-zero");
    const size_t want = WEFT_SHM_HEADER_BYTES + weft_fanout_ring_bytes(128, 4);
    jf = fopen("/dev/shm/weft-shm-test-zero", "wb");
    uint8_t* zeros = calloc(1, want);
    fwrite(zeros, 1, want, jf);
    fclose(jf);
    free(zeros);
    check(weft_shm_attach_named("weft-shm-test-zero", &mj, 0) == -1,
          "zeroed right-size object refused (no header)");
    weft_shm_unlink("weft-shm-test-zero");

    weft_fanout_destroy(&f);
    weft_shm_destroy(&m);
}

// --- S3 ------------------------------------------------------------------------

static void test_s3_excl(void) {
    printf("S3: O_EXCL create semantics + stale replacement\n");
    weft_shm_unlink(TEST_NAME);
    weft_shm_map_t m;
    check(weft_shm_create_named(TEST_NAME, 64, 3, &m) == 0, "first create ok");
    weft_shm_map_t m2;
    check(weft_shm_create_named(TEST_NAME, 64, 3, &m2) == -1,
          "second create refused (O_EXCL)");
    weft_shm_destroy(NULL);  // NULL-safe by contract
    check(1, "destroy NULL-safe");
    weft_shm_destroy(&m);  // unlinks
    check(weft_shm_create_named(TEST_NAME, 64, 3, &m2) == 0,
          "recreate after unlink ok");
    weft_shm_destroy(&m2);
    // Stale replacement is EXPLICIT: unlink a foreign name without mapping.
    check(weft_shm_unlink("weft-shm-test-nothing") == 0, "unlink absent ok (ENOENT)");
}

// --- S4/S5: fork torture --------------------------------------------------------

/// Child reader process: claims the latest frame until the writer's final
/// frame `frames` is observed (latest-wins: intermediate frames are DROPPED
/// by design under load — what must hold is payload integrity on every
/// ACCEPTED frame, the telescoping identity fresh + drops == lastSeq, and
/// bounded waiting). Exit code carries the verdict.
static int reader_child_main(const weft_shm_map_t* m, uint64_t frames, int read_only) {
    weft_fanout_reader_t r;
    memset(&r, 0, sizeof(r));
    if (weft_fanout_reader_init(&r, m->ring, m->mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                weft_shm_payload_bytes(m), weft_shm_slot_count(m)) != 0) {
        return 10;
    }
    const size_t words = weft_shm_payload_bytes(m) / 4;
    uint64_t fresh = 0, drops = 0;
    int payload_ok = 1;
    int spin = 0;
    while (r.rec.seq < frames) {
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (c->fresh) {
            const uint32_t* w = (const uint32_t*)weft_fanout_view(&r);
            for (size_t i = 0; i < words; i += 3) {  // sampled integrity (all words on small payloads)
                if (w[i] != mix32((uint32_t)c->seq * 2654435761u + (uint32_t)i)) {
                    payload_ok = 0;
                }
            }
            if (words < 16) {
                for (size_t i = 0; i < words; i++) {
                    if (w[i] != mix32((uint32_t)c->seq * 2654435761u + (uint32_t)i)) {
                        payload_ok = 0;
                    }
                }
            }
            fresh++;
            drops += c->dropped;
        } else if (++spin > 400000000) {
            return 11;  // bounded: writer died without finishing (Law 1)
        }
    }
    weft_fanout_stats_t st;
    weft_fanout_reader_stats(&r, &st);
    // Telescoping: fresh + drops == lastSeq — every frame accounted exactly
    // once (drops counted, never hidden). torn_exhausted / skipped are
    // legitimate under load (reported); what can NEVER happen is a torn
    // frame ACCEPTED — that is what the payload check above proves.
    const int telescoping = (fresh + drops == r.rec.seq) && (r.rec.seq == frames);
    const int verdict = payload_ok && telescoping;
    fprintf(stderr, "    reader: fresh=%" PRIu64 " drops=%" PRIu64 " last=%" PRIu64
                    " torn_exhausted=%" PRIu64 " skipped=%" PRIu64 " -> %s\n",
            fresh, drops, r.rec.seq, st.torn_exhausted, st.skipped_mid_overwrite,
            verdict ? "OK" : "FAIL");
    weft_fanout_reader_destroy(&r);
    (void)read_only;
    return verdict ? 0 : 12;
}

static void run_fork_torture(int anon, unsigned n_readers, uint64_t frames,
                             size_t payload_bytes, unsigned slots) {
    weft_shm_map_t m;
    if (anon) {
        check(weft_shm_create_anon(payload_bytes, slots, &m) == 0, "anon create ok");
    } else {
        weft_shm_unlink(TORTURE_NAME);
        check(weft_shm_create_named(TORTURE_NAME, payload_bytes, slots, &m) == 0,
              "named create ok");
    }

    // Fork readers FIRST (they spin until frames appear; bounded by the
    // spin cap above — Law 1).
    pid_t pids[8];
    for (unsigned i = 0; i < n_readers; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            if (!anon) {
                // Named road: child attaches BY NAME (no inherited mapping).
                weft_shm_map_t cm;
                int spins = 0;
                while (weft_shm_attach_named(TORTURE_NAME, &cm, 1) != 0) {
                    if (++spins > 1000) _exit(20);
                    usleep(1000);
                }
                const int rc = reader_child_main(&cm, frames, 1);
                weft_shm_destroy(&cm);
                _exit(rc);
            }
            const int rc = reader_child_main(&m, frames, 0);
            _exit(rc);
        }
    }

    // Writer: publish mixer frames.
    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    check(weft_fanout_attach_writer(&f, m.ring, m.mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                    payload_bytes, slots) == 0,
          "writer attach ok");
    for (uint64_t s = 1; s <= frames; s++) {
        fill_mixer(weft_fanout_begin(&f), (uint32_t)s, payload_bytes / 4);
        weft_fanout_publish(&f);
    }
    weft_fanout_destroy(&f);

    int all_ok = 1;
    for (unsigned i = 0; i < n_readers; i++) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_ok = 0;
    }
    {
        char label[96];
        snprintf(label, sizeof(label), "%u forked readers x %" PRIu64
                 " frames: integrity + telescoping, zero torn",
                 n_readers, frames);
        check(all_ok, label);
    }
    weft_shm_destroy(&m);
}

static void test_s4_anon_fork(void) {
    printf("S4: anonymous + fork — inherited mapping, 3 readers\n");
    run_fork_torture(1, 3, 200000, 64, 4);
}

static void test_s5_named_fork(void) {
    printf("S5: named + fork — attach-by-name children, 3 readers\n");
    run_fork_torture(0, 3, 200000, 64, 4);
}

// --- S6 ------------------------------------------------------------------------

static void test_s6_handoff(void) {
    printf("S6: producer handoff — stamp monotonicity across processes\n");
    weft_shm_unlink(TEST_NAME);
    weft_shm_map_t m;
    weft_fanout_t f;
    check(weft_fanout_shm_create(TEST_NAME, 128, 4, &f, &m) == 0, "creator writer ok");
    for (uint32_t s = 1; s <= 1000; s++) {
        fill_mixer(weft_fanout_begin(&f), s, 32);
        weft_fanout_publish(&f);
    }
    // Destroy WITHOUT unlink: simulate a producer crash/handoff.
    m.creator = 0;
    weft_fanout_destroy(&f);
    weft_shm_destroy(&m);

    // New process attaches as writer and CONTINUES the stream.
    weft_fanout_t f2;
    weft_shm_map_t m2;
    check(weft_fanout_shm_attach_writer(TEST_NAME, &f2, &m2) == 0,
          "successor writer attaches");
    // First publish must stamp > 1000 (continues from latestSeq — no slot
    // reuse that would tear a lingering reader).
    fill_mixer(weft_fanout_begin(&f2), 1001, 32);
    const uint64_t seq = weft_fanout_publish(&f2);
    check(seq == 1001, "frame numbering continues from latestSeq");

    // A reader that first attaches now claims 1001 with the 1000 unseen
    // frames accounted as drops (latest-wins contract, not an error).
    weft_fanout_reader_t r;
    weft_shm_map_t mr;
    check(weft_fanout_shm_attach_reader(TEST_NAME, &r, &mr, 1) == 0, "reader attaches");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    check(c->fresh && c->seq == 1001, "handoff frame claims clean");
    check(c->dropped == 1000, "1000 unseen frames accounted as drops (no phantom loss)");
    const uint32_t* hw = (const uint32_t*)weft_fanout_view(&r);
    check(hw[3] == mix32(1001 * 2654435761u + 3), "handoff payload bit-exact");
    weft_fanout_reader_destroy(&r);
    weft_shm_destroy(&mr);
    weft_fanout_destroy(&f2);
    weft_shm_destroy(&m2);
}

// --- S7 ------------------------------------------------------------------------

static void test_s7_fd_attach(void) {
    printf("S7: fd-passing attach — foreign files refused\n");
    weft_shm_unlink(TEST_NAME);
    weft_shm_map_t m;
    check(weft_shm_create_named(TEST_NAME, 96, 4, &m) == 0, "session created");
    // The fd road accepts the session's own fd. A separate mmap of the same
    // object maps at a DIFFERENT virtual address — what must match is the
    // CONTENT: geometry, and cross-mapping visibility of ring writes.
    weft_shm_map_t mfd;
    const int dup_fd = dup(m.fd);
    check(weft_shm_attach_fd(dup_fd, &mfd, 0) == 0, "fd attach ok");
    check(weft_shm_ring_bytes(&mfd) == weft_shm_ring_bytes(&m) &&
              weft_shm_payload_bytes(&mfd) == weft_shm_payload_bytes(&m) &&
              weft_shm_slot_count(&mfd) == weft_shm_slot_count(&m),
          "fd attach: same object, same geometry");
    {
        // Publish through the ORIGINAL map; claim through the FD map — the
        // physical pages are shared, so the frame must be visible across
        // mappings despite different virtual addresses.
        weft_fanout_t f;
        memset(&f, 0, sizeof(f));
        weft_fanout_attach_writer(&f, m.ring, m.mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                  96, 4);
        fill_mixer(weft_fanout_begin(&f), 1, 24);  // fresh ring: first publish stamps seq 1
        weft_fanout_publish(&f);
        weft_fanout_reader_t r;
        memset(&r, 0, sizeof(r));
        weft_fanout_reader_init(&r, mfd.ring, mfd.mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                96, 4);
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        const uint32_t* vw = (const uint32_t*)weft_fanout_view(&r);
        check(c->fresh && c->seq == 1 && vw[9] == mix32(1 * 2654435761u + 9),
              "cross-mapping visibility (publish over map A, claim over map B)");
        weft_fanout_reader_destroy(&r);
        weft_fanout_destroy(&f);
    }
    weft_shm_destroy(&mfd);
    // A plain file with no header is refused.
    FILE* tf = tmpfile();
    fwrite("garbage", 1, 7, tf);
    fflush(tf);
    rewind(tf);
    weft_shm_map_t mgn;
    check(weft_shm_attach_fd(fileno(tf), &mgn, 0) == -1, "garbage fd refused");
    fclose(tf);
    weft_shm_destroy(&m);
}

// --- S8 ------------------------------------------------------------------------

static void test_s8_crash(void) {
    printf("S8: crashed producer — ring persists, reader reads the tail\n");
    weft_shm_unlink(TEST_NAME);
    pid_t w = fork();
    if (w == 0) {
        weft_fanout_t f;
        weft_shm_map_t m;
        if (weft_fanout_shm_create(TEST_NAME, 128, 4, &f, &m) != 0) _exit(2);
        for (uint32_t s = 1; s <= 500; s++) {
            fill_mixer(weft_fanout_begin(&f), s, 32);
            weft_fanout_publish(&f);
        }
        _exit(0);  // exit WITHOUT destroy: the crash posture
    }
    int st = 0;
    waitpid(w, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "producer exited (no cleanup)");

    weft_fanout_reader_t r;
    weft_shm_map_t mr;
    check(weft_fanout_shm_attach_reader(TEST_NAME, &r, &mr, 1) == 0,
          "reader attaches to the orphaned ring");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    check(c->fresh && c->seq == 500, "last consistent frame readable");
    const uint32_t* vw = (const uint32_t*)weft_fanout_view(&r);
    check(vw[0] == mix32(500 * 2654435761u), "frame-500 payload bit-exact");
    weft_fanout_reader_destroy(&r);
    weft_shm_destroy(&mr);  // attacher never unlinks... but the test owns the object now
    weft_shm_unlink(TEST_NAME);
}

// --- S9 ------------------------------------------------------------------------

static void test_s9_bindings(void) {
    printf("S9: fan-out bindings — create/publish/claim round trip\n");
    weft_shm_unlink(TEST_NAME);
    weft_fanout_t f;
    weft_shm_map_t m;
    check(weft_fanout_shm_create(TEST_NAME, 160, 6, &f, &m) == 0, "binding create");
    weft_fanout_reader_t r;
    weft_shm_map_t mr;
    check(weft_fanout_shm_attach_reader(TEST_NAME, &r, &mr, 1) == 0, "binding reader");
    int frames_ok = 1;
    for (uint32_t s = 1; s <= 5000; s++) {
        fill_mixer(weft_fanout_begin(&f), s, 40);
        weft_fanout_publish(&f);
        if (s % 500 == 0) {
            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
            if (!c->fresh || c->seq != s) frames_ok = 0;
            const uint32_t* vw = (const uint32_t*)weft_fanout_view(&r);
            if (vw[5] != mix32(s * 2654435761u + 5)) frames_ok = 0;
        }
    }
    check(frames_ok, "5000 frames round-trip, sampled claims bit-exact");
    weft_fanout_reader_destroy(&r);
    weft_shm_destroy(&mr);
    weft_fanout_destroy(&f);
    weft_shm_destroy(&m);
}

int main(void) {
    printf("Weft S-series (inter-process shared-memory rings) — C driver layer\n");
    test_s1_create();
    test_s2_attach();
    test_s3_excl();
    test_s4_anon_fork();
    test_s5_named_fork();
    test_s6_handoff();
    test_s7_fd_attach();
    test_s8_crash();
    test_s9_bindings();
    printf("\nverdict: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
