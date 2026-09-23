// trend_runner.c — RFC 0020 cross-language verdict-stream emitter (C
// reference). Drives the trend estimator with the deterministic behind
// trace (behind = xorshift32(state) % 64) and prints the packed verdict
// stream (verdict << 6 | min(skip_n, 63), hex, one line) — the exact
// format the TS/Rust/VM emitters produce. fixtures/xlang-trend/run.sh
// byte-compares them all.
//
// Usage: trend_runner [STEPS] [SEED]

#define _GNU_SOURCE
#include "weft_trend.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t xs32(uint32_t x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

int main(int argc, char** argv) {
    long steps = argc > 1 ? atol(argv[1]) : 10000;
    uint32_t seed = 0x00C0FFEE;
    if (argc > 2) {
        seed = (uint32_t)strtoul(argv[2], NULL, 0);
    }
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    for (long i = 0; i < steps; i++) {
        seed = xs32(seed);
        uint32_t behind = seed % 64u;
        weft_trend_observe(&t, behind, &o);
        printf("%02x", weft_trend_pack(&o));
    }
    printf("\n");
    return 0;
}
