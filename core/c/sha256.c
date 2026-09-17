// sha256.c — SHA-256 (FIPS 180-4). Pure C99, no dependencies, no allocation.
// Driver-layer module (RFC 0005 VerifiedWeft) — weft.c/weft.h untouched.
//
// Series 6: the compression below is the portable normative reference; when
// the CPU offers an accelerator, sha256_update/final dispatch through the
// resolved pointer (see sha256.h). Digests are bit-identical across regimes
// (V8 gate) — acceleration is a runtime decision, never a semantic one.

#include "sha256.h"

#include <stdatomic.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

// ---------------------------------------------------------------------------
// HW dispatch (Series 6) — see sha256.h. Resolved once, relaxed-atomically;
// every racing resolver stores the SAME value, so there is no lock and no
// allocation on the hot path. force_* (test/bench only) re-pins the table.
// ---------------------------------------------------------------------------

weft_sha256_transform_fn weft_sha256_hw_resolve(weft_sha256_impl_t impl);

static void sha256_transform_scalar(uint32_t state[8], const uint8_t* data,
                                    size_t blocks) {
    for (size_t b = 0; b < blocks; b++) {
        const uint8_t* block = data + b * SHA256_BLOCK_LEN;
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0], b2 = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b2) ^ (a & c) ^ (b2 & c);
            uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b2; b2 = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b2; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
}

/// Resolved transform (NULL until first use; scalar fallback always valid).
static _Atomic(weft_sha256_transform_fn) g_transform;
/// Non-zero when force_scalar pinned the table (test/bench A/B).
static _Atomic int g_forced_scalar;

static weft_sha256_transform_fn transform_resolve(void) {
    weft_sha256_transform_fn fn =
        atomic_load_explicit(&g_transform, memory_order_relaxed);
    if (__builtin_expect(fn != NULL, 1)) {
        return fn;
    }
    fn = &sha256_transform_scalar;
    if (!atomic_load_explicit(&g_forced_scalar, memory_order_relaxed)) {
        const weft_sha256_impl_t hw = weft_sha256_hw_probe();
        const weft_sha256_transform_fn accelerated = weft_sha256_hw_resolve(hw);
        if (accelerated != NULL) {
            fn = accelerated;
        }
    }
    // Benign duplicate store: every resolver computes the same value here.
    atomic_store_explicit(&g_transform, fn, memory_order_relaxed);
    return fn;
}

weft_sha256_impl_t weft_sha256_active_impl(void) {
    weft_sha256_transform_fn fn = transform_resolve();
    if (fn == &sha256_transform_scalar) {
        return WEFT_SHA256_SCALAR;
    }
    // The only non-scalar transforms come from the probe; ask it which.
    return weft_sha256_hw_probe();
}

void weft_sha256_force_scalar(void) {
    atomic_store_explicit(&g_forced_scalar, 1, memory_order_relaxed);
    atomic_store_explicit(&g_transform, &sha256_transform_scalar,
                          memory_order_relaxed);
}

void weft_sha256_force_auto(void) {
    atomic_store_explicit(&g_forced_scalar, 0, memory_order_relaxed);
    atomic_store_explicit(&g_transform, NULL, memory_order_relaxed);
}

/// Compress exactly one buffered block (block_fill boundary / padding).
static void sha256_compress(sha256_ctx_t* ctx, const uint8_t block[SHA256_BLOCK_LEN]) {
    transform_resolve()(ctx->h, block, 1);
}

static const uint32_t H0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

void sha256_init(sha256_ctx_t* ctx) {
    memcpy(ctx->h, H0, sizeof(H0));
    ctx->total_len = 0;
    ctx->block_fill = 0;
}

void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    ctx->total_len += len;
    if (ctx->block_fill > 0) {
        size_t take = SHA256_BLOCK_LEN - ctx->block_fill;
        if (take > len) take = len;
        memcpy(ctx->block + ctx->block_fill, data, take);
        ctx->block_fill += take;
        data += take;
        len -= take;
        if (ctx->block_fill == SHA256_BLOCK_LEN) {
            sha256_compress(ctx, ctx->block);
            ctx->block_fill = 0;
        }
    }
    if (len >= SHA256_BLOCK_LEN) {
        // Contiguous run: one dispatch for the whole span (multi-block HW
        // transforms amortize the state shuffle dance across the run).
        const size_t blocks = len / SHA256_BLOCK_LEN;
        transform_resolve()(ctx->h, data, blocks);
        data += blocks * SHA256_BLOCK_LEN;
        len -= blocks * SHA256_BLOCK_LEN;
    }
    if (len > 0) {
        memcpy(ctx->block, data, len);
        ctx->block_fill = len;
    }
}

void sha256_final(sha256_ctx_t* ctx, uint8_t out[SHA256_DIGEST_LEN]) {
    uint64_t bit_len = ctx->total_len * 8;
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    uint8_t zero = 0x00;
    while (ctx->block_fill != 56) {
        sha256_update(ctx, &zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    // Feed length directly without counting it (total_len is already captured).
    memcpy(ctx->block + 56, len_be, 8);
    sha256_compress(ctx, ctx->block);
    ctx->block_fill = 0;

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
    sha256_init(ctx);  // reset for reuse
}

void sha256(const uint8_t* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
