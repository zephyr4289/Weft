// weft_sim_ep.c — the deterministic software EP (see weft_sim_ep.h).

#include "weft_sim_ep.h"

#include <string.h>

static float g_weights[WEFT_SIM_EP_N][WEFT_SIM_EP_N];
static int g_init = 0;

void weft_sim_ep_init(void) {
    if (g_init) return;
    // xorshift32 — deterministic across platforms (unsigned ops only).
    uint32_t s = 0x12345678u;
    for (uint32_t j = 0; j < WEFT_SIM_EP_N; j++) {
        for (uint32_t i = 0; i < WEFT_SIM_EP_N; i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            // map to [-1, 1) — exact float division by a power of two
            // (no rounding variance across ISAs)
            g_weights[j][i] = ((float)(int32_t)(s >> 8) / 8388608.0f);
        }
    }
    g_init = 1;
}

void weft_sim_ep_project(const float* src, size_t n_src, float* out64) {
    if (!src || !out64 || n_src < (size_t)WEFT_SIM_EP_N * 1024u) return;
    for (uint32_t i = 0; i < WEFT_SIM_EP_N; i++) {
        out64[i] = src[(size_t)i * 1024u];  // strided sample, fixed order
    }
}

void weft_sim_ep_run(const float* x, float* out) {
    if (!g_init) weft_sim_ep_init();
    if (!x || !out) return;
    for (uint32_t j = 0; j < WEFT_SIM_EP_N; j++) {
        // mul + add, strictly ordered j-major/i-minor — the accumulation
        // order is part of the deterministic contract (bit-exact across
        // builds; no reassociation, no vectorized partial sums)
        float acc = 0.0f;
        for (uint32_t i = 0; i < WEFT_SIM_EP_N; i++) {
            acc += g_weights[j][i] * x[i];
        }
        out[j] = acc;
    }
}
