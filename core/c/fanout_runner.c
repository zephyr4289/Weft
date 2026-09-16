// fanout_runner.c — fan-out ring torture + cross-language interop runner
//
// Modes:
//   torture [frames] [slots] [words] [readers]
//       1 writer thread + N reader threads. Every fresh claim is validated
//       word-by-word against the deterministic pattern; per-reader drop
//       accounting must telescope EXACTLY; every reader must converge to the
//       final frame; zero integrity violations tolerated. Exit 1 on any
//       violation. This is the C conformance gate the seqcst/fenced-acqrel
//       regimes and the ASAN/TSAN builds must ALL pass.
//   dump-ring <file> <meta> [frames] [slots] [words]
//       Publish `frames` pattern frames as a C producer and write the ring
//       bytes + a one-line meta file ("payload_bytes slots frames") for a
//       consumer in another port to attach to (fixtures/xlang-fanout).
//   validate-ring <file> <meta>
//       Attach a reader to a foreign ring dump (any port's bytes), claim the
//       final frame, validate every word + the handoff accounting.
//
// Deterministic payload word (shared with the interop fixtures):
//   word(seq, w) = weft_mix32(seq * 2654435761 + w)      [04-LITMUS §0.1 mixer]
//
// LAW 2 in the harness: the writer's pattern buffer and every reader's copy
// buffer are allocated once before the run; the loop allocates nothing.
//
// Build (see core/c/Makefile): make fanout-runner | fanout-runner-asan |
//                               fanout-runner-tsan | fanout-runner-seq

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "fanout.h"
#include "weft.h"

static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------
// torture
// ---------------------------------------------------------------------------

typedef struct {
    weft_fanout_t* f;
    uint64_t frames;
    uint32_t words;
} writer_arg_t;

typedef struct {
    weft_fanout_reader_t* r;
    uint64_t frames;
    uint32_t words;
    uint64_t sum_dropped, fresh_claims, violations, skips, exhausted, claims;
    int converged;
} reader_arg_t;

static void* writer_fn(void* argp) {
    writer_arg_t* a = (writer_arg_t*)argp;
    uint32_t* buf = (uint32_t*)malloc(a->words * 4); // once per RUN (Law 2 in the loop)
    for (uint64_t seq = 1; seq <= a->frames; seq++) {
        for (uint32_t w = 0; w < a->words; w++) buf[w] = tword((uint32_t)seq, w);
        (void)weft_fanout_begin(a->f);
        if (weft_fanout_fill(a->f, buf, a->words * 4) < 0) {
            fprintf(stderr, "writer: fill failed\n");
            exit(2);
        }
        (void)weft_fanout_publish(a->f);
    }
    free(buf);
    return NULL;
}

static void* reader_fn(void* argp) {
    reader_arg_t* a = (reader_arg_t*)argp;
    // Loop until converged (the writer finishes independently — its own
    // thread — so once it joins, the final frame's slot is stable and the
    // next claim necessarily lands it). The cap is a fail-fast guard against
    // a hypothetical livelock, not a protocol bound: exhausting it without
    // converging FAILS the run.
    const uint64_t MAX_CLAIMS = 2000000000ull;
    while (a->r->last_seq != a->frames && a->claims < MAX_CLAIMS) {
        a->claims++;
        const weft_fanout_claim_t* c = weft_fanout_claim(a->r);
        if (c->fresh) {
            a->fresh_claims++;
            a->sum_dropped += c->dropped;
            const uint32_t* v = (const uint32_t*)weft_fanout_view(a->r);
            const uint32_t seq = (uint32_t)c->seq;
            for (uint32_t w = 0; w < a->words; w++) {
                if (v[w] != tword(seq, w)) { a->violations++; break; }
            }
        }
    }
    a->converged = (a->r->last_seq == a->frames);
    weft_fanout_stats_t st;
    weft_fanout_reader_stats(a->r, &st);
    a->skips = st.skipped_mid_overwrite;
    a->exhausted = st.torn_exhausted;
    return NULL;
}

static int run_torture(uint64_t frames, unsigned slots, uint32_t words, unsigned readers) {
    weft_fanout_t f;
    if (weft_fanout_init(&f, (size_t)words * 4, slots) != 0) {
        fprintf(stderr, "torture: bad geometry\n");
        return 2;
    }
    const size_t rb = weft_fanout_ring_bytes((size_t)words * 4, slots);
    weft_fanout_reader_t* rs = (weft_fanout_reader_t*)calloc(readers, sizeof(*rs));
    reader_arg_t* ra = (reader_arg_t*)calloc(readers, sizeof(*ra));
    for (unsigned i = 0; i < readers; i++) {
        if (weft_fanout_reader_init(&rs[i], f.ring, rb, (size_t)words * 4, slots) != 0) {
            fprintf(stderr, "torture: reader init failed\n");
            return 2;
        }
        ra[i].r = &rs[i];
        ra[i].frames = frames;
        ra[i].words = words;
    }
    writer_arg_t wa = { &f, frames, words };

    const double t0 = now_ms();
    pthread_t wth;
    if (pthread_create(&wth, NULL, writer_fn, &wa) != 0) { perror("pthread_create"); return 2; }
    pthread_t* rths = (pthread_t*)calloc(readers, sizeof(*rths));
    for (unsigned i = 0; i < readers; i++) {
        if (pthread_create(&rths[i], NULL, reader_fn, &ra[i]) != 0) { perror("pthread_create"); return 2; }
    }
    pthread_join(wth, NULL);
    for (unsigned i = 0; i < readers; i++) pthread_join(rths[i], NULL);
    const double dt = now_ms() - t0;

    int failures = 0;
    uint64_t pub = 0;
    {
        weft_fanout_debug_t d;
        weft_fanout_debug_stats(&f, &d);
        pub = d.publishes;
    }
    if (pub != frames) { fprintf(stderr, "FAIL: publishes=%" PRIu64 " expected=%" PRIu64 "\n", pub, frames); failures++; }
    for (unsigned i = 0; i < readers; i++) {
        const uint64_t expect_drops = rs[i].last_seq - ra[i].fresh_claims;
        if (ra[i].violations != 0) {
            fprintf(stderr, "FAIL: reader %u: %" PRIu64 " payload integrity violation(s)\n", i, ra[i].violations);
            failures++;
        }
        if (ra[i].sum_dropped != expect_drops) {
            fprintf(stderr, "FAIL: reader %u: telescoping sum=%" PRIu64 " expected=%" PRIu64 "\n",
                    i, ra[i].sum_dropped, expect_drops);
            failures++;
        }
        if (!ra[i].converged || rs[i].last_seq != frames) {
            fprintf(stderr, "FAIL: reader %u: not converged (last_seq=%" PRIu64 ")\n", i, rs[i].last_seq);
            failures++;
        }
        printf("reader %u: fresh=%" PRIu64 " dropped=%" PRIu64 " claims=%" PRIu64
               " skipped=%" PRIu64 " torn_exhausted=%" PRIu64 " last_seq=%" PRIu64 " identity=OK\n",
               i, ra[i].fresh_claims, ra[i].sum_dropped, ra[i].claims, ra[i].skips,
               ra[i].exhausted, rs[i].last_seq);
    }
    printf("torture: frames=%" PRIu64 " slots=%u words=%u readers=%u publishes=%" PRIu64
           " elapsed_ms=%.1f rate=%" PRIu64 "/s verdict=%s\n",
           frames, slots, words, readers, pub, dt,
           dt > 0 ? (uint64_t)((double)frames / (dt / 1000.0)) : 0,
           failures == 0 ? "PASS" : "FAIL");

    for (unsigned i = 0; i < readers; i++) weft_fanout_reader_destroy(&rs[i]);
    weft_fanout_destroy(&f);
    free(rs); free(ra); free(rths);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// dump-ring / validate-ring (cross-language interop)
// ---------------------------------------------------------------------------

static int write_all(FILE* fp, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n > 0) {
        const size_t w = fwrite(p, 1, n, fp);
        if (w == 0) return -1;
        p += w; n -= w;
    }
    return 0;
}

static int dump_ring(const char* path, const char* meta_path, uint64_t frames,
                     unsigned slots, uint32_t words) {
    weft_fanout_t f;
    if (weft_fanout_init(&f, (size_t)words * 4, slots) != 0) return 2;
    uint32_t* buf = (uint32_t*)malloc((size_t)words * 4);
    for (uint64_t seq = 1; seq <= frames; seq++) {
        for (uint32_t w = 0; w < words; w++) buf[w] = tword((uint32_t)seq, w);
        (void)weft_fanout_begin(&f);
        (void)weft_fanout_fill(&f, buf, (size_t)words * 4);
        (void)weft_fanout_publish(&f);
    }
    const size_t rb = weft_fanout_ring_bytes((size_t)words * 4, slots);
    FILE* fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); return 2; }
    if (write_all(fp, f.ring, rb) != 0) { fprintf(stderr, "write failed\n"); return 2; }
    fclose(fp);
    FILE* mp = fopen(meta_path, "w");
    if (!mp) { perror("fopen"); return 2; }
    fprintf(mp, "%zu %u %" PRIu64 "\n", (size_t)words * 4, slots, frames);
    fclose(mp);
    printf("dump-ring: wrote %s (%zu bytes) + %s — frames=%" PRIu64 " slots=%u words=%u\n",
           path, rb, meta_path, frames, slots, words);
    free(buf);
    weft_fanout_destroy(&f);
    return 0;
}

static int validate_ring(const char* path, const char* meta_path) {
    size_t payload_bytes = 0; unsigned slots = 0; uint64_t frames = 0;
    FILE* mp = fopen(meta_path, "r");
    if (!mp || fscanf(mp, "%zu %u %" SCNu64, &payload_bytes, &slots, &frames) != 3) {
        fprintf(stderr, "validate-ring: bad meta file\n");
        return 2;
    }
    fclose(mp);
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slots);
    if (rb == 0) { fprintf(stderr, "validate-ring: bad geometry in meta\n"); return 2; }
    uint8_t* ring = (uint8_t*)malloc(rb);
    FILE* fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return 2; }
    if (fread(ring, 1, rb, fp) != rb) { fprintf(stderr, "validate-ring: short read\n"); return 2; }
    fclose(fp);

    weft_fanout_reader_t r;
    if (weft_fanout_reader_init(&r, ring, rb, payload_bytes, slots) != 0) {
        fprintf(stderr, "validate-ring: attach failed\n");
        return 2;
    }
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    int failures = 0;
    if (!c->fresh || c->seq != frames) {
        fprintf(stderr, "FAIL: attach claim fresh=%d seq=%" PRIu64 " expected=%" PRIu64 "\n",
                c->fresh, c->seq, frames);
        failures++;
    }
    if (c->dropped != frames - 1) {
        fprintf(stderr, "FAIL: handoff dropped=%" PRIu64 " expected=%" PRIu64 "\n",
                c->dropped, frames - 1);
        failures++;
    }
    const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
    const uint32_t words = (uint32_t)(payload_bytes / 4);
    for (uint32_t w = 0; w < words; w++) {
        if (v[w] != tword((uint32_t)frames, w)) {
            fprintf(stderr, "FAIL: word %u torn across ports (got %08x expected %08x)\n",
                    w, v[w], tword((uint32_t)frames, w));
            failures++;
            break;
        }
    }
    printf("validate-ring: %s — fresh seq=%" PRIu64 " dropped=%" PRIu64
           " payload_words=%u verdict=%s\n",
           path, c->seq, c->dropped, words, failures == 0 ? "PASS" : "FAIL");
    weft_fanout_reader_destroy(&r);
    free(ring);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s torture [frames] [slots] [words] [readers]\n"
                        "       %s dump-ring <file> <meta> [frames] [slots] [words]\n"
                        "       %s validate-ring <file> <meta>\n",
                        argv[0], argv[0], argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "torture") == 0) {
        const uint64_t frames = argc > 2 ? strtoull(argv[2], NULL, 10) : 1000000ull;
        const unsigned slots = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 4u;
        const uint32_t words = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 256u;
        const unsigned readers = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 4u;
        return run_torture(frames, slots, words, readers);
    }
    if (strcmp(argv[1], "dump-ring") == 0) {
        if (argc < 4) { fprintf(stderr, "dump-ring: need <file> <meta>\n"); return 2; }
        const uint64_t frames = argc > 4 ? strtoull(argv[4], NULL, 10) : 5000ull;
        const unsigned slots = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 4u;
        const uint32_t words = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 10) : 64u;
        return dump_ring(argv[2], argv[3], frames, slots, words);
    }
    if (strcmp(argv[1], "validate-ring") == 0) {
        if (argc < 4) { fprintf(stderr, "validate-ring: need <file> <meta>\n"); return 2; }
        return validate_ring(argv[2], argv[3]);
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
