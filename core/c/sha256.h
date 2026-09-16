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

#endif // WEFT_SHA256_H
