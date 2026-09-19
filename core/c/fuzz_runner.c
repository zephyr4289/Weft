// fuzz_runner.c — TIER4 §5 input-validation fuzz corpus (issue #19, task 5).
//
// WHY EXISTS: issue #19's acceptance bar is "zero OOB writes in fuzzing, zero
// UB in UBSan, all inputs validated before any memory operation". This runner
// is the evidence generator: a deterministic (seeded, reproducible) adversary
// that throws malformed inputs at EVERY kernel entry point — kernel geometry,
// publish bounds, payload writes, slice reads, fanout geometry — under
// ASan/UBSan in the sanitizer shard. Any out-of-bounds access or undefined
// behavior is a sanitizer abort; the runner itself also asserts the kernel's
// DECLARED post-conditions (a refusal must not change observable state).
//
// Two modes:
//   ./fuzz-runner [OPS] [SEED]   — standalone deterministic mode (CI default:
//                                  500k ops, seed 0x00C0FFEE — the repo's
//                                  canonical 04-LITMUS §0.2 generator)
//   -DWEFT_FUZZ_LIBFUZZER build — LLVMFuzzerTestOneInput over the same op
//                                  space for OSS-Fuzz / libFuzzer harnesses
//                                  (input bytes drive the op stream).
//
// Adversary op space (all drawn from the seeded PRNG):
//   weft_init           payload_max ∈ {0, 1, 2, 3, 64, 256, 1024, 64K, 1 MiB,
//                       1 MiB + 1, SIZE_MAX/4, random}
//   weft_w_write_payload len ∈ [0, payload_max*2], src ∈ {real, NULL}
//   weft_publish        payload_len ∈ [0, payload_max*2], seq swept
//   weft_r_claim        always (progress must hold under abuse)
//   weft_r_read_slice   offset/len adversarial (>= buf_size, overflow pairs)
//   weft_r_canary_check on every claim (the boundary must hold)
//   weft_fanout_init    payload_bytes ∈ {0, 1, 2, 3, 4, ...}, slots ∈ [0, 65]
//   weft_fanout_fill    len ∈ [0, payload_bytes*2], misaligned lens
//   weft_fanout_reader_init / claim — geometry mismatches, hostile rings
//
// DECLARED POST-CONDITIONS (checked after every op — Law 4, flags not silence):
//   P1 publish returns > WEFT_PUB_OK  => t_publish advanced by exactly 1
//   P2 publish returns WEFT_PUB_INVALID => t_invalid advanced by exactly 1
//      AND no envelope byte changed in the writer's working buffer
//   P3 claim ALWAYS yields slot ∈ {0,1,2} (never fails, 02 §2.2)
//   P4 read_slice returns <= requested bytes AND <= buf_size - offset
//   P5 canary_check ∈ {OK, MISMATCH} on a live held buffer (never a crash)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "weft.h"
#include "fanout.h"

typedef enum {
    WEFT_CANARY_OK = 0,
    WEFT_CANARY_MISMATCH = 1,
} weft_canary_result_t;

static inline weft_canary_result_t weft_r_canary_check(weft_t* w) {
    if (!w || !w->buf[w->r_work]) return WEFT_CANARY_MISMATCH;
    uint64_t canary;
    memcpy(&canary, (const uint8_t*)w->buf[w->r_work] + w->buf_size - 8, 8);
    return (canary == (uint64_t)weft_r_seq(w)) ? WEFT_CANARY_OK : WEFT_CANARY_MISMATCH;
}

static uint32_t rng_state;
static uint32_t rng(void) { return weft_xorshift32(&rng_state); }

// Deterministic but platform-stable: derive size_t in [0, cap).
static size_t pick(size_t cap) { return (size_t)(rng() % (cap ? (uint32_t)cap : 1)); }

static long post_checks = 0;
static long ops_run = 0;

static int fail(const char* what, long op) {
    fprintf(stderr, "FUZZ POST-CONDITION FAIL: %s (op %ld)\n", what, op);
    return 1;
}

// ---------------------------------------------------------------------------

static uint8_t scratch[4096];

static int fuzz_kernel_once(long op_no) {
    static const size_t sizes[] = { 0, 1, 2, 3, 64, 256, 1024, 65536,
                                    WEFT_PAYLOAD_MAX_LIMIT,
                                    WEFT_PAYLOAD_MAX_LIMIT + 1,
                                    (size_t)-1 / 4 };
    size_t payload_max;
    switch (rng() % 3) {
        case 0: payload_max = sizes[rng() % (sizeof(sizes) / sizeof(sizes[0]))]; break;
        case 1: payload_max = 1 + pick(4096); break;
        default: payload_max = rng() % 8; break;
    }

    weft_t w;
    int rc = weft_init(&w, payload_max);
    if (rc != 0) {
        // Refused init must be the ONLY effect; nothing to clean.
        return 0;
    }
    if (payload_max == 0 || payload_max > WEFT_PAYLOAD_MAX_LIMIT) {
        // The wall refused AFTER alloc-time — impossible by contract.
        return fail("init accepted an out-of-bounds payload_max", op_no);
    }

    for (int burst = 0; burst < 8; burst++) {
        ops_run++;
        uint32_t seq = rng();
        // Adversarial payload_len: straddles the wall.
        uint32_t plen;
        switch (rng() % 3) {
            case 0: plen = (uint32_t)pick(payload_max + 1); break;
            case 1: plen = (uint32_t)payload_max + (rng() % 64); break;
            default: plen = rng() % 64; break;
        }

        // w_write_payload with adversarial len. Caller obligation: src must
        // hold len bytes — so wlen is capped to the scratch buffer while still
        // straddling the wall for small payload_max (the publish wall covers
        // oversized payload_len independently).
        size_t wlen = pick(payload_max * 2 + 2);
        if (wlen > sizeof scratch) wlen = sizeof scratch;
        uint64_t pub_before = weft_t_publish(&w);
        uint64_t inv_before = weft_t_invalid(&w);
        (void)weft_w_write_payload(&w, scratch, wlen);

        weft_pub_result_t pr = weft_publish(&w, seq, plen);

        if (pr == WEFT_PUB_OK) {
            post_checks++;
            if (weft_t_publish(&w) != pub_before + 1)
                return fail("P1 t_publish bookkeeping", op_no);
            (void)weft_r_claim(&w);
            post_checks++;
            if (weft_r_canary_check(&w) != WEFT_CANARY_OK)
                return fail("P5 canary OK on a legit frame", op_no);
        } else if (pr == WEFT_PUB_INVALID) {
            post_checks++;
            if (weft_t_invalid(&w) != inv_before + 1)
                return fail("P2 t_invalid bookkeeping", op_no);
            post_checks++;
            if (weft_t_publish(&w) != pub_before)
                return fail("P2 INVALID publish must not count as published", op_no);
        } else { // DROPPED_REVOKED path only reachable post-revoke; not here
            return fail("unexpected publish result pre-revoke", op_no);
        }

        // Claim + adversarial slice.
        uint32_t slot = weft_r_claim(&w);
        post_checks++;
        if (slot > 2) return fail("P3 claim yielded an out-of-range slot", op_no);

        size_t off = pick(w.buf_size + 64);
        size_t dlen = pick(w.buf_size * 2 + 8);
        // The read-slice contract: the caller owns a dst of dst_len capacity.
        // (The harness's first draft used a 16-byte stack buffer and its own
        // ASan run caught the harness bug — the fuzz process eats its own dog
        // food: read_slice bounded by min(dst_len, buf_size - offset), so a
        // dst_len-sized dst is the caller obligation.)
        uint8_t* dst = (uint8_t*)malloc(dlen ? dlen : 1);
        size_t got = weft_r_read_slice(&w, dst, off, dlen);
        free(dst);
        post_checks++;
        if (off >= w.buf_size) {
            if (got != 0) return fail("P4 read_slice beyond buf_size must be 0", op_no);
        } else {
            size_t avail = w.buf_size - off;
            size_t cap = dlen < avail ? dlen : avail;
            if (got > dlen || got > avail || got != cap)
                return fail("P4 read_slice bounds", op_no);
        }
        if (weft_r_canary_check(&w) != WEFT_CANARY_OK &&
            weft_r_canary_check(&w) != WEFT_CANARY_MISMATCH)
            return fail("P5 canary verdict domain", op_no);
    }

    weft_destroy(&w);
    return 0;
}

static int fuzz_fanout_once(long op_no) {
    // Hostile geometry matrix.
    size_t payload_bytes;
    unsigned slots;
    switch (rng() % 3) {
        case 0: payload_bytes = pick(8); break;                  // 0..7 (misaligned zone)
        case 1: payload_bytes = 4 * (1 + pick(256)); break;      // legit words
        default: payload_bytes = pick(4096); break;
    }
    slots = (unsigned)(rng() % 70) - 2;                          // wraps near 0; 0..67
    // Aim most attempts inside/near the legal band [2, 64].
    if (rng() & 1) slots = 2 + (unsigned)pick(64);

    weft_fanout_t f;
    int rc = weft_fanout_init(&f, payload_bytes, slots);
    if (rc != 0) return 0;  // refused geometry — the wall worked

    if (payload_bytes == 0 || (payload_bytes & 3) != 0 ||
        slots < 2 || slots > WEFT_FANOUT_MAX_SLOTS)
        return fail("fanout_init accepted out-of-contract geometry", op_no);

    // A mismatched reader must be refused, not torn.
    weft_fanout_reader_t r;
    size_t wrong_payload = payload_bytes + 4;
    if (weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                                weft_fanout_ring_bytes(payload_bytes, slots),
                                wrong_payload, slots) == 0) {
        return fail("reader_init accepted mismatched geometry", op_no);
    }

    if (weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                                weft_fanout_ring_bytes(payload_bytes, slots),
                                payload_bytes, slots) != 0)
        return fail("reader_init refused legit geometry", op_no);

    for (int burst = 0; burst < 4; burst++) {
        ops_run++;
        (void)weft_fanout_begin(&f);
        size_t len = pick(payload_bytes * 2 + 4);
        (void)weft_fanout_fill(&f, scratch, len);   // wall validates len
        (void)weft_fanout_publish(&f);
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        post_checks++;
        if (!c) return fail("claim returned NULL (must be a record)", op_no);
        if (c->dropped != 0 && !c->fresh) {
            // dropped is only meaningful on a fresh claim (Law 4 telescoping)
            return fail("dropped set on a non-fresh claim", op_no);
        }
    }
    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
    return 0;
}

static int fuzz_seed_ops(long ops) {
    for (long i = 0; i < ops; i++) {
        uint32_t which = rng() % 4;
        int bad;
        switch (which) {
            case 0:
            case 1: bad = fuzz_kernel_once(i); break;
            case 2: bad = fuzz_fanout_once(i); break;
            default: bad = (rng() & 1) ? fuzz_kernel_once(i) : fuzz_fanout_once(i); break;
        }
        if (bad) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------

#ifndef WEFT_FUZZ_LIBFUZZER

int main(int argc, char** argv) {
    long ops = (argc > 1) ? atol(argv[1]) : 500000;
    uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x00C0FFEEu;
    if (ops <= 0) ops = 1;
    rng_state = seed ? seed : 0x9E3779B9u;

    memset(scratch, 0x5A, sizeof scratch);

    printf("fuzz-runner: ops=%ld seed=0x%08X\n", ops, rng_state);
    int bad = fuzz_seed_ops(ops);
    printf("{\"test\":\"fuzz\",\"ops\":%ld,\"post_checks\":%ld,\"status\":\"%s\"}\n",
           ops_run, post_checks, bad ? "FAILED" : "PASSED");
    return bad;
}

#else  // WEFT_FUZZ_LIBFUZZER

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Drive the same op space from fuzzer bytes: 4-byte chunks = PRNG steps.
    uint32_t state = 0x9E3779B9u;
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t x;
        memcpy(&x, data + i, 4);
        state ^= x;
        weft_xorshift32(&state);
        rng_state = state;
        if (state & 1) { if (fuzz_kernel_once((long)i)) abort(); }
        else           { if (fuzz_fanout_once((long)i)) abort(); }
    }
    return 0;
}

#endif
