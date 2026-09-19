// ffi_fuzz.c — Issue #16 Tier 1 Task 4: FFI-boundary structured fuzzing.
//
// WHY STRUCTURED, NOT COVERAGE-GUIDED (the honest declaration): this
// sandbox has no clang (libFuzzer) and no AFL++; the harness is a seeded,
// deterministic, structure-aware op-stream fuzzer whose ORACLE is the
// sanitizer pair (ASAN + UBSAN builds) plus the protocol's own invariants.
// Coverage feedback is the declared follow-up for a clang-bearing CI runner
// (the 24h soak the issue asks for belongs to the nightly tier; the cycle
// budgets achieved HERE are committed as evidence, never inflated).
//
// SURFACE 1 (in-process, this binary): the C kernel API — the exact
// functions an FFI consumer (JNI, Dart FFI, wasm-bindgen) calls. Random
// op stream over one weft_t + the envelope pure functions, with lifecycle
// churn (destroy + re-init) at random points:
//   publish(random seq, random len) · w_write_payload(random bytes)
//   claim · r_read_slice(random offset/len) · envelope encode/decode on
//   random (often corrupted) buffers · negotiate(random version sets)
// ORACLES: (a) ASAN/UBSAN — memory safety, the FFI hazard class itself;
// (b) I1 ownership: latest/w_work/r_work pairwise distinct (debug view);
// (c) telemetry monotonicity; (d) decode verdict legality + consistency.
//
// SURFACE 2 (cross-process, driven by ci/scripts/run_ffi_fuzz_shard.sh):
// the C<->TS ring boundary with RANDOM CORRUPTION — rings produced by
// either side, K random bytes flipped anywhere (ctrl AND payload), the
// other side consuming. The oracle: the consumer resolves GRACEFULLY
// (exit 0/1 verdicts), never crashes, never throws across the boundary.
//
// LAW 1: every op class is counted. LAW 4: no clang, no 24h — declared.
//
// Build: make ffi-fuzz | ffi-fuzz-asan (the ASAN build is the oracle leg)
// CLI:   ./ffi-fuzz [ops] [seed]
// Exit:  0 pass / 1 invariant violation / 2 usage.

#define _GNU_SOURCE
#include "weft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct {
    uint64_t ops;                 // total ops executed
    uint64_t n_publish, n_claim, n_slice, n_env_enc, n_env_dec, n_neg;
    uint64_t n_corrupt, n_lifecycle;
    uint64_t violations;          // invariant failures (MUST be 0)
} fuzz_ledger_t;

static uint32_t xs32(uint32_t* s) { return weft_xorshift32(s); }

// Telemetry snapshot for monotonicity
typedef struct { uint64_t p, c, d, ws, rs; } tele_t;

static tele_t tele_of(const weft_t* w) {
    tele_t t = { weft_t_publish((weft_t*)w), weft_t_claim((weft_t*)w),
                 weft_t_drop((weft_t*)w), weft_t_wsteps((weft_t*)w),
                 weft_t_rsteps((weft_t*)w) };
    return t;
}

int main(int argc, char** argv) {
    uint32_t ops_target = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 200000;
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x00C0FFEEu;

    weft_t w;
    if (weft_init(&w, 1024) != 0) return 2;
    uint32_t rng = seed;
    fuzz_ledger_t led = { 0 };
    tele_t prev = tele_of(&w);
    uint8_t scratch[2048];

    for (uint32_t op = 0; op < ops_target; op++) {
        uint32_t kind = xs32(&rng) % 100;
        if (kind < 25) {                                   // publish cycle
            // fill with pattern or random bytes (50/50)
            uint32_t len = xs32(&rng) % (w.payload_max + 1);
            if (xs32(&rng) & 1) {
                uint8_t* cur = weft_w_begin(&w);
                for (uint32_t i = 0; i < len; i++) cur[i] = weft_pat(1, i) ^ (uint8_t)xs32(&rng);
            } else {
                for (uint32_t i = 0; i < (len < sizeof(scratch) ? len : sizeof(scratch)); i++)
                    scratch[i] = weft_pat(7, i);
                (void)weft_w_write_payload(&w, scratch, len);
            }
            // adversarial seq: mostly sequential, sometimes wild
            uint32_t seq = (xs32(&rng) & 7) ? (xs32(&rng) % 4096) : op;
            (void)weft_publish(&w, seq, len);
            led.n_publish++;
        } else if (kind < 45) {                            // claim + slice read
            (void)weft_r_claim(&w);
            led.n_claim++;
            uint32_t off = xs32(&rng) % (uint32_t)w.buf_size;
            uint32_t dl  = xs32(&rng) % 512;
            (void)weft_r_read_slice(&w, scratch, off, dl);
            led.n_slice++;
        } else if (kind < 65) {                            // envelope encode
            uint16_t ver = (uint16_t)(xs32(&rng) % 4);
            uint16_t hs  = (uint16_t)(16 + (xs32(&rng) % 32));
            uint32_t s   = xs32(&rng);
            uint32_t pl  = xs32(&rng) % 2048;
            weft_envelope_encode(scratch, ver, hs, s, pl);
            led.n_env_enc++;
        } else if (kind < 90) {                            // envelope decode (often corrupted)
            size_t avail = 16 + (xs32(&rng) % 64);
            memset(scratch, 0, 128);
            weft_envelope_encode_v1(scratch, xs32(&rng) % 4096, xs32(&rng) % 2048);
            // corrupt 0..3 random bytes inside the window
            uint32_t ncorrupt = xs32(&rng) % 4;
            for (uint32_t k = 0; k < ncorrupt; k++) {
                scratch[xs32(&rng) % avail] ^= (uint8_t)(1u << (xs32(&rng) % 8));
            }
            led.n_corrupt += ncorrupt;
            uint16_t v, hs; uint32_t s, pl;
            weft_decode_result_t r = weft_envelope_decode(scratch, avail, &v, &hs, &s, &pl);
            led.n_env_dec++;
            // ORACLE (d): verdict legality + consistency
            if (r == WEFT_DECODE_OK) {
                if (hs < 16 || hs > avail) { led.violations++; }
                if (pl > avail - hs) { led.violations++; }
                // (version round-trips whatever was encoded — 0 is a legal
                // wire value; negotiate's 0-sentinel is separate semantics)
            } else if (r != WEFT_DECODE_SHORT && r != WEFT_DECODE_BAD_MAGIC
                       && r != WEFT_DECODE_BAD_HEADER) {
                led.violations++;
            }
        } else if (kind < 97) {                            // negotiate
            uint16_t vs[4];
            uint32_t n = 1 + (xs32(&rng) % 4);
            for (uint32_t k = 0; k < n; k++) vs[k] = (uint16_t)(1 + (xs32(&rng) % 4));
            uint16_t chosen = weft_negotiate((uint16_t)(1 + (xs32(&rng) % 4)), vs, n);
            // ORACLE: 0 (incompatible) or one of the offered versions
            if (chosen != 0) {
                bool offered = false;
                for (uint32_t k = 0; k < n; k++) if (vs[k] == chosen) offered = true;
                if (!offered) led.violations++;
            }
            led.n_neg++;
        } else {                                           // lifecycle churn
            weft_destroy(&w);
            if (weft_init(&w, 1024) != 0) return 2;
            prev = tele_of(&w);                            // telemetry resets
            led.n_lifecycle++;
        }

        // ORACLE (b)+(c): ownership + telemetry monotonicity, every 64 ops
        if ((op & 63) == 0) {
            weft_debug_view_t dv;
            weft_debug_view(&w, &dv);
            if (dv.latest == dv.w_work || dv.latest == dv.r_work || dv.w_work == dv.r_work) {
                led.violations++;   // I1 broken
            }
            tele_t now = tele_of(&w);
            if (now.p < prev.p || now.c < prev.c || now.d < prev.d
                || now.ws < prev.ws || now.rs < prev.rs) {
                led.violations++;   // telemetry regressed
            }
            prev = now;
        }
        led.ops++;
    }

    weft_destroy(&w);
    bool pass = led.violations == 0;
    fprintf(stderr, "ffi-fuzz: ops=%llu publish=%llu claim=%llu slice=%llu "
                    "env_enc=%llu env_dec=%llu corrupt=%llu negotiate=%llu "
                    "lifecycle=%llu violations=%llu\n",
            (unsigned long long)led.ops, (unsigned long long)led.n_publish,
            (unsigned long long)led.n_claim, (unsigned long long)led.n_slice,
            (unsigned long long)led.n_env_enc, (unsigned long long)led.n_env_dec,
            (unsigned long long)led.n_corrupt, (unsigned long long)led.n_neg,
            (unsigned long long)led.n_lifecycle, (unsigned long long)led.violations);
    printf("{\"test\":\"ffi-fuzz\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"ops\":%llu,\"publish\":%llu,\"claim\":%llu,"
           "\"env_decode\":%llu,\"corrupted_bytes\":%llu,\"negotiate\":%llu,"
           "\"lifecycle_churns\":%llu,\"violations\":%llu}}\n",
           pass ? "true" : "false",
           (unsigned long long)led.ops, (unsigned long long)led.n_publish,
           (unsigned long long)led.n_claim, (unsigned long long)led.n_env_dec,
           (unsigned long long)led.n_corrupt, (unsigned long long)led.n_neg,
           (unsigned long long)led.n_lifecycle, (unsigned long long)led.violations);
    return pass ? 0 : 1;
}
