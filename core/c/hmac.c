// hmac.c — HMAC-SHA256 (RFC 2104). Pure C99, no dependencies.
// Driver-layer module (RFC 0005 VerifiedWeft) — weft.c/weft.h untouched.

#include "hmac.h"

#include <string.h>

static void xor_pad(uint8_t* dst, const uint8_t* key, size_t key_len, uint8_t pad) {
    memset(dst, pad, SHA256_BLOCK_LEN);
    for (size_t i = 0; i < key_len; i++) {
        dst[i] = (uint8_t)(key[i] ^ pad);
    }
}

void hmac_sha256_init_key(hmac_sha256_key_t* k, const uint8_t* key, size_t key_len) {
    uint8_t key_block[SHA256_BLOCK_LEN];

    // Keys longer than the block size are hashed first (RFC 2104 §2).
    memset(key_block, 0, sizeof(key_block));
    if (key_len > SHA256_BLOCK_LEN) {
        sha256(key, key_len, key_block);  // 32 bytes < 64: rest stays zero
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad_block[SHA256_BLOCK_LEN];
    xor_pad(ipad_block, key_block, SHA256_BLOCK_LEN, 0x36);
    xor_pad(k->opad_block, key_block, SHA256_BLOCK_LEN, 0x5c);

    sha256_init(&k->inner);
    sha256_update(&k->inner, ipad_block, SHA256_BLOCK_LEN);
}

void hmac_sha256_update(hmac_sha256_key_t* k, const uint8_t* data, size_t len) {
    sha256_update(&k->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_key_t* k, uint8_t out[HMAC_SHA256_TAG_LEN]) {
    uint8_t inner_digest[SHA256_DIGEST_LEN];
    sha256_final(&k->inner, inner_digest);

    // outer = H(opad || inner_digest)
    sha256_ctx_t outer;
    sha256_init(&outer);
    sha256_update(&outer, k->opad_block, SHA256_BLOCK_LEN);
    sha256_update(&outer, inner_digest, SHA256_DIGEST_LEN);
    sha256_final(&outer, out);

    // Re-seed the inner pad so the key state is reusable for the next message.
    uint8_t ipad_block[SHA256_BLOCK_LEN];
    xor_pad(ipad_block, k->opad_block, SHA256_BLOCK_LEN, (0x36 ^ 0x5c));  // recover key then re-xor
    // The trick above works because opad = key ^ 0x5c, so key = opad ^ 0x5c,
    // and ipad = key ^ 0x36 = opad ^ 0x5c ^ 0x36.
    sha256_init(&k->inner);
    sha256_update(&k->inner, ipad_block, SHA256_BLOCK_LEN);
}

void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t data_len,
                 uint8_t out[HMAC_SHA256_TAG_LEN]) {
    hmac_sha256_key_t k;
    hmac_sha256_init_key(&k, key, key_len);
    hmac_sha256_update(&k, data, data_len);
    hmac_sha256_final(&k, out);
}
