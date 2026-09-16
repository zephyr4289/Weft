// governor_test.c — RFC 0009 FreshnessGovernor conformance (G-series), C.
//
// Mirrors packages/core/test/governor.test.ts and
// core/rust/tests/governor_test.rs (G1–G4 local; G5 is the shared-trace
// xlang fixture in fixtures/xlang-governor/, driven by the xlang-dump mode
// below).
//
// Modes:
//   ./governor-test            — run G1–G4, emit one JSON verdict line
//   ./governor-test xlang-dump N SEED — emit the packed G5 action log for a
//                                deterministic (behind, now_ms) trace to
//                                stdout (one byte per step: kind<<6|skip_n,
//                                hex-encoded), N steps, xorshift32(SEED)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "governor.h"

static uint32_t xorshift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static int expect(int cond, const char* what, int behind) {
    if (!cond) {
        fprintf(stderr, "GATE FAIL: %s (behind=%u)\n", what, (unsigned)behind);
        return 1;
    }
    return 0;
}

static int run_g1_g4(void) {
    int fails = 0;
    // ---- G1: ladder — every behind in 0..64 maps to the documented action
    // (now advances past the cooldown every step so Reseed is never
    // suppressed; G1 tests the LADDER, not the rate limit). ----
    {
        weft_governor_t g;
        weft_governor_init_default(&g);
        for (uint32_t behind = 0; behind <= 64; behind++) {
            const weft_gov_action_t* a = weft_governor_step(&g, behind, (int64_t)behind * 1000);
            if (behind <= WEFT_GOV_FAST_PATH_BEHIND_DEFAULT) {
                fails += expect(a->kind == WEFT_GOV_FAST_PATH && a->skip_n == 0,
                                "G1 fast path", behind);
            } else if (behind <= WEFT_GOV_SKIP_BEHIND_DEFAULT) {
                fails += expect(a->kind == WEFT_GOV_SKIP &&
                                a->skip_n == behind - WEFT_GOV_FAST_PATH_BEHIND_DEFAULT,
                                "G1 skip(n)", behind);
            } else if (behind <= WEFT_GOV_SNAPSHOT_BEHIND_DEFAULT) {
                fails += expect(a->kind == WEFT_GOV_SNAPSHOT && a->skip_n == 0,
                                "G1 snapshot", behind);
            } else {
                fails += expect(a->kind == WEFT_GOV_RESEED && a->skip_n == 0,
                                "G1 reseed", behind);
            }
        }
    }

    // ---- G2: monotone — larger behind never yields a fresher-class action
    {
        weft_governor_t g;
        weft_governor_init_default(&g);
        int prev_class = -1;
        for (uint32_t behind = 0; behind <= 64; behind++) {
            const weft_gov_action_t* a = weft_governor_step(&g, behind, (int64_t)behind * 1000);
            fails += expect((int)a->kind >= prev_class, "G2 monotone", behind);
            prev_class = (int)a->kind;
        }
    }

    // ---- G3: reseed flap — 10k random spikes, at most ceil(10k/cooldown)
    //      Reseeds, minimum spacing >= cooldown. ----
    {
        weft_governor_t g;
        weft_governor_init_default(&g);
        const int STEPS = 10000;
        uint32_t state = 0x00C0FFEEu;
        uint32_t reseeds = 0;
        int64_t last_reseed_at = -1;
        for (int i = 0; i < STEPS; i++) {
            state = xorshift32(state);
            uint32_t behind = state % 128; // spikes well past 16
            const weft_gov_action_t* a = weft_governor_step(&g, behind, i); // 1 ms/step
            if (a->kind == WEFT_GOV_RESEED) {
                reseeds++;
                if (last_reseed_at >= 0) {
                    fails += expect(i - last_reseed_at >= 250, "G3 cooldown spacing", behind);
                }
                last_reseed_at = i;
            }
        }
        const uint32_t bound = (STEPS + 249) / 250; // ceil(10000/250) = 40
        fails += expect(reseeds <= bound, "G3 flap bound", 0);
        fails += expect(reseeds > 0, "G3 exercised", 0);
        fails += expect(g.reseeds == reseeds, "G3 counter", 0);
    }

    // ---- G3b: suppressed Reseed degrades to Snapshot ----
    {
        weft_governor_t g;
        weft_governor_init_default(&g);
        fails += expect(weft_governor_step(&g, 64, 1000)->kind == WEFT_GOV_RESEED, "G3b first reseed", 64);
        fails += expect(weft_governor_step(&g, 64, 1050)->kind == WEFT_GOV_SNAPSHOT, "G3b suppressed", 64);
        fails += expect(weft_governor_step(&g, 64, 1300)->kind == WEFT_GOV_RESEED, "G3b post-cooldown", 64);
        fails += expect(g.reseeds == 2, "G3b reseed count", 64);
    }

    // ---- G4: zero allocation — no malloc anywhere in step()'s path. In C
    // this is proven by construction (the source calls no allocator; state
    // is stack/struct-resident). The identity-stable record: every step
    // returns the SAME pointer. ----
    {
        weft_governor_t g;
        weft_governor_init_default(&g);
        const weft_gov_action_t* a1 = weft_governor_step(&g, 0, 0);
        const weft_gov_action_t* a2 = weft_governor_step(&g, 64, 1000);
        fails += expect(a1 == a2, "G4 identity-stable record", 0);
        for (int i = 0; i < 1000000; i++) {
            (void)weft_governor_step(&g, (uint32_t)(i % 32), i);
        }
        fails += expect(g.steps == 1000002, "G4 step counter", 0);
    }

    // ---- Law 4: Skip(n) intermediates counted as DECIDED drops ----
    {
        weft_governor_t g;
        weft_governor_init_default(&g);
        (void)weft_governor_step(&g, 3, 0);  // Skip(2)
        (void)weft_governor_step(&g, 4, 1);  // Skip(3)
        (void)weft_governor_step(&g, 1, 2);  // FastPath
        (void)weft_governor_step(&g, 9, 3);  // Snapshot
        fails += expect(g.decided_drops == 5, "Law 4 decided drops", 0);
        fails += expect(g.steps == 4, "Law 4 steps", 0);
    }

    // ---- custom thresholds and reset ----
    {
        weft_governor_t g;
        weft_governor_init(&g, 0, 8, 32, 10);
        fails += expect(weft_governor_step(&g, 0, 0)->kind == WEFT_GOV_FAST_PATH, "custom fast", 0);
        fails += expect(weft_governor_step(&g, 1, 0)->kind == WEFT_GOV_SKIP, "custom skip", 1);
        fails += expect(weft_governor_step(&g, 33, 0)->kind == WEFT_GOV_RESEED, "custom reseed", 33);
        weft_governor_reset(&g);
        fails += expect(g.steps == 0 && g.decided_drops == 0 && g.reseeds == 0, "reset counters", 0);
        fails += expect(weft_governor_step(&g, 33, 0)->kind == WEFT_GOV_RESEED, "post-reset cooldown fresh", 33);
    }

    const int pass = fails == 0;
    printf("{\"test\":\"governor-G-series\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"gates\":\"G1,G2,G3,G3b,G4,Law4,custom\"},"
           "\"notes\":\"RFC-0009 FreshnessGovernor conformance; G5 via xlang-dump + fixtures/xlang-governor\"}\n",
           pass ? "true" : "false");
    return pass ? 0 : 1;
}

// ---- G5: xlang-dump — the deterministic shared trace ----
// Trace generator (identical in all three languages):
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N: state = xorshift32(state);
//                  behind = state % 128; now_ms = i;
//                  action = governor.step(behind, now_ms)
//                  emit byte (kind << 6) | min(skip_n, 63)
// Emitted hex-encoded (lowercase, no separators, one trailing newline).
static int run_xlang_dump(long steps, uint32_t seed) {
    weft_governor_t g;
    weft_governor_init_default(&g);
    uint32_t state = seed;
    for (long i = 0; i < steps; i++) {
        state = xorshift32(state);
        uint32_t behind = state % 128;
        const weft_gov_action_t* a = weft_governor_step(&g, behind, i);
        uint8_t packed = (uint8_t)(((uint32_t)a->kind << 6) | (a->skip_n > 63 ? 63 : a->skip_n));
        printf("%02x", packed);
    }
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "xlang-dump") == 0) {
        long steps = argc >= 3 ? strtol(argv[2], NULL, 10) : 10000;
        uint32_t seed = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0x00C0FFEEu;
        return run_xlang_dump(steps, seed);
    }
    return run_g1_g4();
}
