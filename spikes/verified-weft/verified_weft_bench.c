// verified_weft_bench.c — Benchmark for RFC 0005 (VerifiedWeft HMAC-SHA256 frames)
// Measures authenticated frame encoding/decoding overhead at 64B payload.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Standalone SHA-256 implementation
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(sha256_ctx_t* ctx, const uint8_t data[64]) {
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    uint32_t m[64];

    for (int i = 0; i < 16; ++i) {
        m[i] = (data[i * 4] << 24) | (data[i * 4 + 1] << 16) | (data[i * 4 + 2] << 8) | (data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85; ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c; ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    size_t i = 0;
    size_t idx = (ctx->count >> 3) & 63;
    ctx->count += (uint64_t)len << 3;

    if (idx) {
        size_t part = 64 - idx;
        if (len >= part) {
            memcpy(&ctx->buffer[idx], data, part);
            sha256_transform(ctx, ctx->buffer);
            i = part;
        } else {
            memcpy(&ctx->buffer[idx], data, len);
            return;
        }
    }
    for (; i + 63 < len; i += 64) {
        sha256_transform(ctx, &data[i]);
    }
    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

static void sha256_final(sha256_ctx_t* ctx, uint8_t hash[32]) {
    uint8_t final_count[8];
    for (int i = 0; i < 8; ++i) {
        final_count[i] = (uint8_t)((ctx->count >> ((7 - i) * 8)) & 0xFF);
    }
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    while (((ctx->count >> 3) & 63) != 56) {
        uint8_t zero = 0;
        sha256_update(ctx, &zero, 1);
    }
    sha256_update(ctx, final_count, 8);
    for (int i = 0; i < 8; ++i) {
        hash[i * 4] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        hash[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        hash[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

static void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t mac[32]) {
    uint8_t k_pad[64];
    uint8_t tk[32];
    if (key_len > 64) {
        sha256_ctx_t tctx;
        sha256_init(&tctx);
        sha256_update(&tctx, key, key_len);
        sha256_final(&tctx, tk);
        key = tk;
        key_len = 32;
    }
    memset(k_pad, 0, 64);
    memcpy(k_pad, key, key_len);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5c;
    }

    uint8_t inner_hash[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner_hash, 32);
    sha256_final(&ctx, mac);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    const size_t PAYLOAD_SIZE = 64;
    const uint64_t ITERATIONS = 200000;
    const uint8_t key[32] = "weft-secret-hmac-key-auth-triad";

    uint8_t frame_data[16 + 64]; // 16B envelope + 64B payload
    memset(frame_data, 0x42, sizeof(frame_data));
    uint8_t tag[32];

    printf("=== RFC 0005: VerifiedWeft HMAC-SHA256 Benchmark ===\n");
    printf("Environment Tag: x86_64-sandbox / linux-arm64-sandbox\n");
    printf("Payload size: %zu bytes (total frame: %zu bytes)\n", PAYLOAD_SIZE, sizeof(frame_data));
    printf("Iterations: %lu\n", (unsigned long)ITERATIONS);

    // 1. Warm-up
    for (int i = 0; i < 5000; i++) {
        hmac_sha256(key, 32, frame_data, sizeof(frame_data), tag);
    }

    // 2. Measure Encode (Signing)
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < ITERATIONS; i++) {
        frame_data[4] = (uint8_t)(i & 0xFF); // simulate seq update
        hmac_sha256(key, 32, frame_data, sizeof(frame_data), tag);
    }
    uint64_t t1 = now_ns();

    double encode_total_ns = (double)(t1 - t0);
    double encode_ns_per_frame = encode_total_ns / (double)ITERATIONS;

    // 3. Measure Decode (Verification)
    uint8_t verify_tag[32];
    uint64_t t2 = now_ns();
    int verified_ok = 0;
    for (uint64_t i = 0; i < ITERATIONS; i++) {
        hmac_sha256(key, 32, frame_data, sizeof(frame_data), verify_tag);
        if (memcmp(tag, verify_tag, 32) == 0) verified_ok++;
    }
    uint64_t t3 = now_ns();

    double decode_total_ns = (double)(t3 - t2);
    double decode_ns_per_frame = decode_total_ns / (double)ITERATIONS;

    printf("\nResults:\n");
    printf("  HMAC Encode: %.2f ns/frame (%.3f us)\n", encode_ns_per_frame, encode_ns_per_frame / 1000.0);
    printf("  HMAC Decode: %.2f ns/frame (%.3f us)\n", decode_ns_per_frame, decode_ns_per_frame / 1000.0);
    printf("  Combined Roundtrip: %.2f ns/frame (%.3f us)\n", encode_ns_per_frame + decode_ns_per_frame, (encode_ns_per_frame + decode_ns_per_frame) / 1000.0);
    printf("  Throughput: %.2f M frames/sec\n", 1000.0 / encode_ns_per_frame);
    printf("  Sub-microsecond Target: %s\n", encode_ns_per_frame < 1000.0 ? "MET (< 1.0 us)" : "MISSED (> 1.0 us)");

    return 0;
}
