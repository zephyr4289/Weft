// hmac.h — HMAC-SHA256 (RFC 2104), pure C, zero dependencies.
// Driver-layer module (RFC 0005 VerifiedWeft) — weft.c/weft.h untouched.

#ifndef WEFT_HMAC_H
#define WEFT_HMAC_H

#include "sha256.h"

#define HMAC_SHA256_TAG_LEN SHA256_DIGEST_LEN  // 32

/// Pre-keyed HMAC state: the inner/outer key pads are derived once, then any
/// number of messages can be authenticated with two compressions per message
/// (plus payload blocks). This is the shape the per-frame hot path uses.
typedef struct hmac_sha256_key {
    sha256_ctx_t inner;              // pre-initialized with ipad block
    uint8_t      opad_block[SHA256_BLOCK_LEN]; // opad block, fed at final
} hmac_sha256_key_t;

/// Prepare a keyed state from an arbitrary-length key (RFC 2104 §2).
void hmac_sha256_init_key(hmac_sha256_key_t* k, const uint8_t* key, size_t key_len);

/// Feed message bytes.
void hmac_sha256_update(hmac_sha256_key_t* k, const uint8_t* data, size_t len);

/// Finalize into HMAC_SHA256_TAG_LEN bytes. State remains valid for the next
/// message (inner is re-seeded from the pre-keyed pad).
void hmac_sha256_final(hmac_sha256_key_t* k, uint8_t out[HMAC_SHA256_TAG_LEN]);

/// One-shot convenience.
void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t data_len,
                 uint8_t out[HMAC_SHA256_TAG_LEN]);

#endif // WEFT_HMAC_H
