// turbo_test.c — T-series battery for the RFC 0012 Tail-Latency Eradication
// Layer
//
// What this file pins (the CONFORMANCE story; turbo_runner.c owns the
// MEASUREMENT story):
//
//   T1  capability probe — sanity bounds + the honest refusal fields exist
//   T2  turbo ring (mmap/THP/prefault/attach) is BYTE-IDENTICAL to a plain
//       weft_fanout_init ring over a deterministic 10k-frame history —
//       placement/policy change nothing observable (Kernel Freeze proof)
//   T3  prefault gate — minor-fault delta across a 1000-frame hot loop is
//       ZERO (the first-touch memset did its job; Law-2-style gate)
//   T4  prefetch wrappers (turbo begin/publish/claim) are byte- and
//       stat-identical to the plain calls over the same history
//   T5  NT streaming fill (weft_turbo_fill) is byte-identical to
//       weft_fanout_fill — aligned body AND ragged prologue geometries
//   T6  pin/restore affinity roundtrip
//   T7  SCHED_FIFO + mlockall refusals are return codes, not crashes
//   T8  pinned multithreaded torture — 500k frames, writer CPU 0 + readers
//       CPU 1: every fresh claim verifies full payload (torn-acceptance
//       detector), telescoping fresh+drops == lastSeq EXACT per reader,
//       turbo-claim and plain-claim readers agree
//   T9  NUMA bind scope + restore (single-node host: ladder verification)
//
// Build (see core/c/Makefile): make turbo-test        (-O2)
//                               make turbo-test-asan  (-fsanitize=address)
//                               make turbo-test-tsan  (-fsanitize=thread)
//                               make turbo-test-seq   (all-seq_cst regime)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "turbo.h"
#include "fanout.h"
#include "frame_cursor.h"
#include "weft.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        } else {                                                            \
            fprintf(stdout, "ok: %s\n", msg);                               \
        }                                                                    \
    } while (0)

/// Deterministic payload word — the same generator family as fanout_test.c,
/// so cross-file comparisons are exact.
static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

// ---------------------------------------------------------------------------
// T1 — capability probe
// ---------------------------------------------------------------------------

static void t1_caps(void) {
    const weft_turbo_caps_t* c = weft_turbo_caps();
    char report[512];
    weft_turbo_caps_report(report, sizeof(report));
    fputs(report, stdout);

    CHECK(c->probed, "T1 probe ran");
    CHECK(c->page_size >= 4096, "T1 page_size sane");
    CHECK(c->ncpu >= 1, "T1 ncpu sane");
    CHECK(c->node_count >= 1 && c->node_count <= WEFT_TURBO_MAX_NODES,
          "T1 node_count in bounds");
    CHECK(c->node_cpu_count[0] >= 1, "T1 node 0 has CPUs");
    CHECK(c->thp_mode == WEFT_TURBO_THP_ALWAYS || c->thp_mode == WEFT_TURBO_THP_MADVISE ||
          c->thp_mode == WEFT_TURBO_THP_NEVER || c->thp_mode == WEFT_TURBO_THP_UNKNOWN,
          "T1 thp_mode is a declared enum value");
    // Refusal fields are part of the contract: they must be SET when the
    // service is refused (0 only when available).
    CHECK(!c->mlock_ok ? c->mlock_errno != 0 : 1, "T1 mlock refusal carries errno");
    CHECK(!c->sched_fifo_ok ? c->sched_fifo_errno != 0 : 1,
          "T1 sched_fifo refusal carries errno");
}

// ---------------------------------------------------------------------------
// T2 — turbo ring is byte-identical to a plain ring
// ---------------------------------------------------------------------------

static void publish_history(weft_fanout_t* f, uint32_t from, uint32_t to,
                            size_t words, int use_turbo) {
    uint32_t buf[1024];
    for (uint32_t seq = from; seq <= to; seq++) {
        for (size_t w = 0; w < words; w++) buf[w] = tword(seq, (uint32_t)w);
        if (use_turbo) {
            (void)weft_turbo_begin(f);
            CHECK(weft_turbo_fill(f, buf, words * 4) == (int)words,
                  "T2 turbo fill writes every word");
            (void)weft_turbo_publish(f);
        } else {
            (void)weft_fanout_begin(f);
            CHECK(weft_fanout_fill(f, buf, words * 4) == (int)words,
                  "T2 plain fill writes every word");
            (void)weft_fanout_publish(f);
        }
    }
}

static void t2_ring_equivalence(void) {
    enum { WORDS = 64, M = 8 };            // 256-byte payloads, 8 slots
    const size_t PB = WORDS * 4;

    weft_fanout_t plain;
    CHECK(weft_fanout_init(&plain, PB, M) == 0, "T2 plain init succeeds");

    weft_turbo_ring_opts_t o;
    weft_turbo_ring_opts_default(&o);
    o.slot_count = M;
    o.payload_bytes = PB;
    weft_fanout_t tf;
    weft_turbo_ring_t tr;
    CHECK(weft_turbo_fanout_create(&tf, &tr, &o) == 0, "T2 turbo ring create succeeds");
    printf("  T2 ring: %zu B, thp_advised=%d locked=%d prefaulted=%d numa_node=%d\n",
           tr.ring_bytes, tr.thp_advised, tr.locked, tr.prefaulted, tr.numa_node);
    CHECK(tr.ring_bytes == weft_fanout_ring_bytes(PB, M),
          "T2 turbo ring geometry matches the interop formula");
    CHECK(((uintptr_t)tr.ring & 63u) == 0, "T2 turbo ring is 64-byte aligned");
    CHECK(tr.locked == 0 || tr.locked == EPERM || tr.locked == ENOMEM,
          "T2 mlock ladder: locked-or-refused-with-known-errno");

    // Same deterministic history on both rings.
    publish_history(&plain, 1, 10000, WORDS, 0);
    publish_history(&tf, 1, 10000, WORDS, 1);

    CHECK(memcmp(plain.ring, tr.ring, tr.ring_bytes) == 0,
          "T2 ring BYTES IDENTICAL across 10k frames (placement changes nothing observable)");

    // And the frozen reader sees the same story over the turbo ring.
    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, tr.ring, tr.ring_bytes, PB, M) == 0,
          "T2 reader attaches to the turbo ring");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == 10000 && c->dropped == 9999,
          "T2 latest frame claimable from turbo ring with exact drop accounting");
    const uint32_t* view = (const uint32_t*)weft_fanout_view(&r);
    int ok = 1;
    for (size_t w = 0; w < WORDS; w++) {
        if (view[w] != tword(10000, (uint32_t)w)) { ok = 0; break; }
    }
    CHECK(ok, "T2 turbo ring payload bytes verify against the pattern");
    weft_fanout_reader_destroy(&r);

    weft_turbo_fanout_destroy(&tf, &tr);
    weft_fanout_destroy(&plain);
    CHECK(tr.map_base == NULL, "T2 destroy clears the mapping record");
}

// ---------------------------------------------------------------------------
// T3 — prefault gate: zero minor faults in the hot loop
// ---------------------------------------------------------------------------

static unsigned long stat_min_flt(void) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return (unsigned long)-1;
    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return (unsigned long)-1; }
    fclose(f);
    // Field 10 (1-based) is minflt; the comm field may contain spaces — skip
    // past the closing ')' then count.
    char* p = strrchr(line, ')');
    if (!p) return (unsigned long)-1;
    p += 2;  // past ") "
    unsigned long minflt = 0;
    // p now points at field 3 (state). Fields 4..10 follow; minflt is the
    // 8th token from here (fields: state, ppid, pgrp, session, tty_nr,
    // tpgid, flags, minflt).
    int tok = 0;
    while (*p && tok < 8) {
        while (*p == ' ') p++;
        char* end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (tok == 7) { minflt = v; break; }
        p = end;
        tok++;
    }
    return minflt;
}

static void t3_prefault(void) {
    enum { WORDS = 64, M = 8, FRAMES = 1000 };
    const size_t PB = WORDS * 4;

    weft_turbo_ring_opts_t o;
    weft_turbo_ring_opts_default(&o);
    o.slot_count = M;
    o.payload_bytes = PB;
    weft_fanout_t f;
    weft_turbo_ring_t tr;
    CHECK(weft_turbo_fanout_create(&f, &tr, &o) == 0, "T3 turbo ring create succeeds");

    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, tr.ring, tr.ring_bytes, PB, M) == 0,
          "T3 reader init");

    // Warm EVERYTHING (reader target, stdio, pattern tables) before the
    // snapshot so the measured window contains ring traffic only.
    publish_history(&f, 1, 64, WORDS, 1);
    (void)weft_turbo_claim(&r);

    const unsigned long before = stat_min_flt();
    uint32_t buf[WORDS];
    for (uint32_t seq = 65; seq < 65 + FRAMES; seq++) {
        for (size_t w = 0; w < WORDS; w++) buf[w] = tword(seq, (uint32_t)w);
        (void)weft_turbo_begin(&f);
        (void)weft_turbo_fill(&f, buf, PB);
        (void)weft_turbo_publish(&f);
        (void)weft_turbo_claim(&r);
    }
    const unsigned long after = stat_min_flt();

    CHECK(before != (unsigned long)-1 && after != (unsigned long)-1,
          "T3 min_flt readable");
    printf("  T3 minor faults: before=%lu after=%lu delta=%lu over %d frames\n",
           before, after, after - before, FRAMES);
    CHECK(after - before == 0,
          "T3 ZERO minor faults in the prefaulted hot loop (first-touch did its job)");

    weft_fanout_reader_destroy(&r);
    weft_turbo_fanout_destroy(&f, &tr);
}

// ---------------------------------------------------------------------------
// T4 — prefetch wrappers are semantics-neutral
// ---------------------------------------------------------------------------

static void t4_prefetch_wrappers(void) {
    enum { WORDS = 64, M = 8, N = 5000 };
    const size_t PB = WORDS * 4;

    weft_fanout_t plain, tf;
    CHECK(weft_fanout_init(&plain, PB, M) == 0, "T4 plain init");
    weft_turbo_ring_opts_t o;
    weft_turbo_ring_opts_default(&o);
    o.slot_count = M;
    o.payload_bytes = PB;
    weft_turbo_ring_t tr;
    CHECK(weft_turbo_fanout_create(&tf, &tr, &o) == 0, "T4 turbo ring create");

    // Interleave: publish 5 frames on each ring, claim on each ring, repeat.
    weft_fanout_reader_t ra, rb;
    CHECK(weft_fanout_reader_init(&ra, plain.ring, weft_fanout_ring_bytes(PB, M), PB, M) == 0,
          "T4 plain reader init");
    CHECK(weft_fanout_reader_init(&rb, tr.ring, tr.ring_bytes, PB, M) == 0,
          "T4 turbo reader init");

    uint32_t buf[WORDS];
    for (uint32_t seq = 1; seq <= N; seq += 5) {
        for (int k = 0; k < 5; k++) {
            uint32_t s = seq + (uint32_t)k;
            for (size_t w = 0; w < WORDS; w++) buf[w] = tword(s, (uint32_t)w);
            (void)weft_fanout_begin(&plain);
            (void)weft_fanout_fill(&plain, buf, PB);
            (void)weft_fanout_publish(&plain);
            (void)weft_turbo_begin(&tf);
            (void)weft_turbo_fill(&tf, buf, PB);
            (void)weft_turbo_publish(&tf);
        }
        const weft_fanout_claim_t* ca = weft_fanout_claim(&ra);
        const weft_fanout_claim_t* cb = weft_turbo_claim(&rb);
        CHECK(ca->fresh == cb->fresh && ca->seq == cb->seq && ca->dropped == cb->dropped,
              "T4 claim records agree (turbo prefetch changes no result)");
        if (ca->fresh) {
            CHECK(memcmp(weft_fanout_view(&ra), weft_fanout_view(&rb), PB) == 0,
                  "T4 claimed payloads byte-identical");
        }
    }

    CHECK(memcmp(plain.ring, tr.ring, tr.ring_bytes) == 0,
          "T4 ring bytes identical with wrappers active");
    weft_fanout_stats_t sa, sb;
    weft_fanout_reader_stats(&ra, &sa);
    weft_fanout_reader_stats(&rb, &sb);
    CHECK(sa.fresh == sb.fresh && sa.drops == sb.drops && sa.reads == sb.reads,
          "T4 reader stats identical");

    weft_fanout_reader_destroy(&ra);
    weft_fanout_reader_destroy(&rb);
    weft_fanout_destroy(&plain);
    weft_turbo_fanout_destroy(&tf, &tr);
}

// ---------------------------------------------------------------------------
// T5 — NT streaming fill equivalence (aligned + ragged geometries)
// ---------------------------------------------------------------------------

static void t5_nt_fill_for(unsigned M) {
    enum { WORDS = 64, N = 2000 };
    const size_t PB = WORDS * 4;

    weft_fanout_t a, b;
    CHECK(weft_fanout_init(&a, PB, M) == 0, "T5 plain init");
    CHECK(weft_fanout_init(&b, PB, M) == 0, "T5 nt init");

    uint32_t buf[WORDS];
    for (uint32_t seq = 1; seq <= N; seq++) {
        for (size_t w = 0; w < WORDS; w++) buf[w] = tword(seq, (uint32_t)w);
        (void)weft_fanout_begin(&a);
        CHECK(weft_fanout_fill(&a, buf, PB) == (int)WORDS, "T5 frozen fill ok");
        (void)weft_fanout_publish(&a);
        (void)weft_fanout_begin(&b);
        CHECK(weft_turbo_fill(&b, buf, PB) == (int)WORDS, "T5 turbo fill ok");
        (void)weft_fanout_publish(&b);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "T5 (M=%u) NT fill byte-identical to frozen fill", M);
    CHECK(memcmp(a.ring, b.ring, weft_fanout_ring_bytes(PB, M)) == 0, msg);

    // And a concurrent-style single reader still sees exact frames.
    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, b.ring, weft_fanout_ring_bytes(PB, M), PB, M) == 0,
          "T5 reader init");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == N && c->dropped == N - 1, "T5 latest claim exact");
    const uint32_t* view = (const uint32_t*)weft_fanout_view(&r);
    int ok = 1;
    for (size_t w = 0; w < WORDS; w++) if (view[w] != tword(N, (uint32_t)w)) { ok = 0; break; }
    snprintf(msg, sizeof(msg), "T5 (M=%u) NT-filled payload verifies", M);
    CHECK(ok, msg);

    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&a);
    weft_fanout_destroy(&b);
}

// ---------------------------------------------------------------------------
// T6/T7/T9 — thread services and the honesty ladder
// ---------------------------------------------------------------------------

static void t6_pin(void) {
    const weft_turbo_caps_t* caps = weft_turbo_caps();
    weft_turbo_affinity_t saved;
    CHECK(weft_turbo_save_affinity(&saved) == 0, "T6 save affinity");
    int rc = weft_turbo_pin_cpu(caps->ncpu - 1);
    CHECK(rc == 0 || rc == -EPERM || rc == -EINVAL,
          "T6 pin returns 0 or a documented refusal");
    if (rc == 0) {
        CHECK(weft_turbo_restore_affinity(&saved) == 0, "T6 restore affinity");
        cpu_set_t cur;
        CHECK(sched_getaffinity(0, sizeof(cur), &cur) == 0, "T6 affinity readable");
        CHECK(memcmp(&cur, saved.raw, sizeof(cpu_set_t)) == 0,
              "T6 restored mask equals the saved mask");
    }
}

static void t7_refusals(void) {
    int rt = weft_turbo_rt(10);
    printf("  T7 SCHED_FIFO(10) -> %d (%s)\n", rt,
           rt == 0 ? "granted" : strerror(-rt));
    CHECK(rt == 0 || rt == -EPERM || rt == -ENOSYS || rt == -EINVAL,
          "T7 SCHED_FIFO: granted or documented refusal, never a crash");
    int ml = weft_turbo_mlockall();
    printf("  T7 mlockall -> %d (%s)\n", ml,
           ml == 0 ? "granted" : strerror(-ml));
    CHECK(ml == 0 || ml == -EPERM || ml == -ENOMEM || ml == -ENOSYS || ml == -EACCES,
          "T7 mlockall: granted or documented refusal, never a crash");
}

static void t9_numa(void) {
    weft_turbo_affinity_t saved;
    int rc = weft_turbo_numa_bind(0, &saved);
    printf("  T9 numa_bind(node 0) -> %d (single-node host: ladder check)\n", rc);
    CHECK(rc == 0 || rc == -ENOSYS || rc == -EINVAL,
          "T9 numa_bind(0) succeeds or refuses with a documented code");
    if (rc == 0) {
        CHECK(weft_turbo_restore_affinity(&saved) == 0, "T9 numa restore");
    }
    CHECK(weft_turbo_numa_bind(WEFT_TURBO_MAX_NODES + 4, NULL) == -EINVAL,
          "T9 out-of-range node refused");
}

// ---------------------------------------------------------------------------
// T8 — pinned torture (writer CPU 0, readers CPU 1)
// ---------------------------------------------------------------------------

enum { T8_FRAMES = 500000, T8_WORDS = 64, T8_M = 8 };
static const size_t T8_PB = T8_WORDS * 4;

typedef struct {
    weft_fanout_t* fan;
    _Atomic int* done;
    int pin_cpu;   // -1 = no pin request
} t8_warg_t;

typedef struct {
    weft_fanout_reader_t* r;
    int use_turbo_claim;
    int pin_cpu;              // -1 = no pin request
    _Atomic int* done;
    uint64_t torn_accepts;
    uint64_t fresh;
    uint64_t drops;
    uint64_t last_seq;
    uint64_t reads;
} t8_rarg_t;

static void* t8_writer_entry(void* arg) {
    t8_warg_t* wa = (t8_warg_t*)arg;
    if (wa->pin_cpu >= 0) (void)weft_turbo_pin_cpu(wa->pin_cpu);
    uint32_t buf[T8_WORDS];
    for (uint32_t seq = 1; seq <= T8_FRAMES; seq++) {
        for (size_t w = 0; w < T8_WORDS; w++) buf[w] = tword(seq, (uint32_t)w);
        (void)weft_turbo_begin(wa->fan);
        (void)weft_turbo_fill(wa->fan, buf, T8_PB);
        (void)weft_turbo_publish(wa->fan);
    }
    atomic_store_explicit(wa->done, 1, memory_order_release);
    return NULL;
}

static void* t8_reader_entry(void* arg) {
    t8_rarg_t* ra = (t8_rarg_t*)arg;
    if (ra->pin_cpu >= 0) (void)weft_turbo_pin_cpu(ra->pin_cpu);
    for (;;) {
        const weft_fanout_claim_t* c = ra->use_turbo_claim
                                     ? weft_turbo_claim(ra->r)
                                     : weft_fanout_claim(ra->r);
        ra->reads++;
        if (c->fresh) {
            ra->fresh++;
            ra->drops += c->dropped;
            ra->last_seq = c->seq;
            const uint32_t* view = (const uint32_t*)weft_fanout_view(ra->r);
            for (size_t w = 0; w < T8_WORDS; w++) {
                if (view[w] != tword((uint32_t)c->seq, (uint32_t)w)) {
                    ra->torn_accepts++;  // a torn frame slipped THROUGH: fatal
                    break;
                }
            }
        }
        // Exit only when the writer is done AND this claim was not fresh AND
        // we are genuinely caught up (latestSeq == our last_seq). The bare
        // done&&!fresh condition is RACY: a mid-overwrite skip that lands
        // just before the writer's final store would exit un-caught-up (the
        // frozen skip path is correct behavior; this discriminator is the
        // harness's job — latestSeq is the documented advisory ctrl read).
        if (atomic_load_explicit(ra->done, memory_order_acquire) && !c->fresh) {
            const uint64_t latest =
                atomic_load_explicit(ra->r->ctrl, memory_order_acquire);
            if (latest == ra->last_seq) break;
        }
    }
    return NULL;
}

static void t8_torture(void) {
    weft_turbo_ring_opts_t o;
    weft_turbo_ring_opts_default(&o);
    o.slot_count = T8_M;
    o.payload_bytes = T8_PB;
    weft_fanout_t f;
    weft_turbo_ring_t tr;
    CHECK(weft_turbo_fanout_create(&f, &tr, &o) == 0, "T8 turbo ring create");

    weft_fanout_reader_t r1, r2;
    CHECK(weft_fanout_reader_init(&r1, tr.ring, tr.ring_bytes, T8_PB, T8_M) == 0,
          "T8 reader 1 init (turbo claim)");
    CHECK(weft_fanout_reader_init(&r2, tr.ring, tr.ring_bytes, T8_PB, T8_M) == 0,
          "T8 reader 2 init (plain claim)");

    _Atomic int done;
    atomic_init(&done, 0);
    t8_warg_t wa = { .fan = &f, .done = &done, .pin_cpu = 0 };

    t8_rarg_t ra1, ra2;
    memset(&ra1, 0, sizeof(ra1));
    memset(&ra2, 0, sizeof(ra2));
    ra1.r = &r1; ra1.use_turbo_claim = 1; ra1.done = &done;
    ra2.r = &r2; ra2.use_turbo_claim = 0; ra2.done = &done;

    // Placement (per-thread pins inside the entries — pthread_create
    // inherits the CREATOR's mask, so pins must be set in the threads
    // themselves): writer CPU 0, both readers CPU 1 — the 2-CPU sandbox
    // layout. On 1-CPU hosts the reader pin targets CPU 0 and the torture
    // still runs (just unpinned-separation); refusals are accepted.
    const weft_turbo_caps_t* caps = weft_turbo_caps();
    ra1.pin_cpu = caps->ncpu > 1 ? 1 : 0;
    ra2.pin_cpu = caps->ncpu > 1 ? 1 : 0;

    pthread_t tw, th1, th2;
    CHECK(pthread_create(&tw, NULL, t8_writer_entry, &wa) == 0, "T8 writer thread");
    CHECK(pthread_create(&th1, NULL, t8_reader_entry, &ra1) == 0, "T8 reader 1 thread");
    CHECK(pthread_create(&th2, NULL, t8_reader_entry, &ra2) == 0, "T8 reader 2 thread");
    pthread_join(tw, NULL);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    printf("  T8 reader1(turbo): reads=%lu fresh=%lu drops=%lu last=%lu torn=%lu\n",
           ra1.reads, ra1.fresh, ra1.drops, ra1.last_seq, ra1.torn_accepts);
    printf("  T8 reader2(plain): reads=%lu fresh=%lu drops=%lu last=%lu torn=%lu\n",
           ra2.reads, ra2.fresh, ra2.drops, ra2.last_seq, ra2.torn_accepts);

    CHECK(ra1.torn_accepts == 0, "T8 turbo-claim reader: ZERO torn frames accepted");
    CHECK(ra2.torn_accepts == 0, "T8 plain-claim reader: ZERO torn frames accepted");
    CHECK(ra1.fresh + ra1.drops == ra1.last_seq,
          "T8 turbo-claim telescoping EXACT (fresh+drops == lastSeq)");
    CHECK(ra2.fresh + ra2.drops == ra2.last_seq,
          "T8 plain-claim telescoping EXACT (fresh+drops == lastSeq)");
    CHECK(ra1.last_seq == T8_FRAMES && ra2.last_seq == T8_FRAMES,
          "T8 both readers converged to the final frame");
    CHECK(ra1.reads > 0 && ra2.reads > 0, "T8 both readers made progress");

    weft_fanout_reader_destroy(&r1);
    weft_fanout_reader_destroy(&r2);
    weft_turbo_fanout_destroy(&f, &tr);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// T10 — runtime prefetch distance (issue #17-4)
// ---------------------------------------------------------------------------

static void t10_prefetch_distance(void) {
    const int saved = weft_turbo_prefetch_get_distance();

    // Knob semantics: round-trip, refusal, clamping.
    CHECK(weft_turbo_prefetch_get_distance() == WEFT_TURBO_PREFETCH_MAX_BYTES,
          "T10 startup default == compile-time macro");
    CHECK(weft_turbo_prefetch_set_distance(256) == 256 &&
              weft_turbo_prefetch_get_distance() == 256,
          "T10 set/get round-trip");
    CHECK(weft_turbo_prefetch_set_distance(0) == 0 &&
              weft_turbo_prefetch_get_distance() == 0,
          "T10 0 disables hints");
    CHECK(weft_turbo_prefetch_set_distance(-64) == -1 &&
              weft_turbo_prefetch_get_distance() == 0,
          "T10 negative refused, distance unchanged");
    CHECK(weft_turbo_prefetch_set_distance(1 << 24) ==
              WEFT_TURBO_PREFETCH_DIST_HARD_MAX,
          "T10 runaway values clamp to the hard max");

    // tune() lands on a documented starting point and reports it.
    const int tuned = weft_turbo_prefetch_tune();
    CHECK(tuned == 128 || tuned == 256,
          "T10 tune() picks a documented starting point (128/256)");
    CHECK(weft_turbo_prefetch_get_distance() == tuned,
          "T10 tune() sets what it returns");

    // Ordering-neutrality with hints OFF and at 256: claims byte-identical
    // to a plain ring across an interleave (hints never change results).
    {
        enum { WORDS = 64, M = 8, N = 4000 };
        const size_t PB = WORDS * 4;
        weft_fanout_t plain, tf;
        CHECK(weft_fanout_init(&plain, PB, M) == 0, "T10 plain init");
        weft_turbo_ring_opts_t o;
        weft_turbo_ring_opts_default(&o);
        o.slot_count = M;
        o.payload_bytes = PB;
        weft_turbo_ring_t tr;
        CHECK(weft_turbo_fanout_create(&tf, &tr, &o) == 0, "T10 turbo ring");
        weft_fanout_reader_t ra, rb;
        weft_fanout_reader_init(&ra, plain.ring, weft_fanout_ring_bytes(PB, M), PB, M);
        weft_fanout_reader_init(&rb, tr.ring, tr.ring_bytes, PB, M);
        uint32_t buf[WORDS];
        int all_ok = 1;
        for (int dist_i = 0; dist_i < 2; dist_i++) {
            weft_turbo_prefetch_set_distance(dist_i == 0 ? 0 : 256);
            for (uint32_t seq = 1; seq <= N; seq++) {
                for (size_t w = 0; w < WORDS; w++) buf[w] = tword(seq, (uint32_t)w);
                (void)weft_fanout_begin(&plain);
                (void)weft_fanout_fill(&plain, buf, PB);
                (void)weft_fanout_publish(&plain);
                (void)weft_turbo_begin(&tf);
                (void)weft_turbo_fill(&tf, buf, PB);
                (void)weft_turbo_publish(&tf);
                const weft_fanout_claim_t* ca = weft_fanout_claim(&ra);
                const weft_fanout_claim_t* cb = weft_turbo_claim(&rb);
                if (ca->fresh != cb->fresh || ca->seq != cb->seq ||
                    memcmp(weft_fanout_view(&ra), weft_fanout_view(&rb), PB) != 0) {
                    all_ok = 0;
                }
            }
        }
        CHECK(all_ok, "T10 claims byte-identical at distance 0 and 256");
        weft_fanout_reader_destroy(&ra);
        weft_fanout_reader_destroy(&rb);
        weft_fanout_destroy(&plain);
        weft_turbo_fanout_destroy(&tf, &tr);
    }

    (void)weft_turbo_prefetch_set_distance(saved);  // restore for later tests
}

int main(void) {
    t1_caps();
    t2_ring_equivalence();
    t3_prefault();
    t4_prefetch_wrappers();
    t5_nt_fill_for(8);   // aligned body (payload base 80 ≡ 0 mod 16)
    t5_nt_fill_for(3);   // ragged prologue (payload base 40 ≡ 8 mod 16)
    t6_pin();
    t7_refusals();
    t9_numa();
    t10_prefetch_distance();
    t8_torture();

    if (g_failures == 0) {
        printf("T-SERIES: ALL PASS\n");
        return 0;
    }
    printf("T-SERIES: %d FAILURE(S)\n", g_failures);
    return 1;
}
