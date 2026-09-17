// sha256.h — SHA-256 (FIPS 180-4), pure C, zero dependencies.
//
// Driver-layer module (RFC 0005 VerifiedWeft). NOT part of the frozen kernel:
// weft.c / weft.h are untouched. This is a standalone hash primitive used by
// hmac.c; it exists because the kernel's dependency bar is "no external
// libraries" (core/c builds with plain gcc, no third-party code).
//
// Verified against RFC 4231 HMAC-SHA-256 test vectors (see verified_test.c).

#ifndef WEFT_SHA256_H
#define WEFT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

/// Streaming SHA-256 context. Reusable; no allocation.
typedef struct sha256_ctx {
    uint32_t h[8];
    uint64_t total_len;               // bytes fed so far
    uint8_t  block[SHA256_BLOCK_LEN]; // partial block buffer
    size_t   block_fill;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len);
/// Finalizes and writes SHA256_DIGEST_LEN bytes to out. ctx is reset for reuse.
void sha256_final(sha256_ctx_t* ctx, uint8_t out[SHA256_DIGEST_LEN]);

/// One-shot convenience.
void sha256(const uint8_t* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

// ---------------------------------------------------------------------------
// Hardware acceleration (Series 6 — RFC 0005's hardware deferral, realized)
// ---------------------------------------------------------------------------
// The scalar FIPS 180-4 implementation above remains the normative reference;
// the accelerated paths produce bit-identical digests, by construction and by
// test (V8 sweeps both regimes over the shared fixture vectors plus
// randomized buffers). Acceleration is a runtime decision: the CPU is probed
// once and per-compression dispatch goes through a resolved-once pointer
// (relaxed atomic; benign duplicate stores of the same value — no lock, no
// allocation, frozen kernel untouched).

/// Which compression implementation the CPU offers / the table is using.
typedef enum {
    WEFT_SHA256_SCALAR     = 0,  // portable C99 — always available
    WEFT_SHA256_X86_SHA_NI = 1,  // Intel SHA extensions (rnds2/msg1/msg2)
    WEFT_SHA256_ARM_CE     = 2,  // ARMv8 Crypto Extensions (FEAT_SHA256)
} weft_sha256_impl_t;

/// Multi-block transform: compress `blocks` x 64-byte big-endian blocks into
/// state[8]. Shared signature for scalar and accelerated implementations;
/// sha256_update/final call the resolved pointer. Function pointer type is
/// part of the dispatch contract (see sha256.c).
typedef void (*weft_sha256_transform_fn)(uint32_t state[8],
                                         const uint8_t* data, size_t blocks);

/// What this CPU supports (probe only; SCALAR when no accelerator matches).
weft_sha256_impl_t weft_sha256_hw_probe(void);

/// What the dispatch table currently uses (lazy resolve; reflects force_*).
/// Tests and the bench runner report this so evidence logs name the regime.
weft_sha256_impl_t weft_sha256_active_impl(void);

/// Pin the portable scalar path (A/B benchmarking, conformance sweeps).
/// Test/bench-only API — single-threaded use assumed by its callers.
void weft_sha256_force_scalar(void);

/// Restore runtime hardware dispatch (the default state).
void weft_sha256_force_auto(void);

#endif // WEFT_SHA256_H
