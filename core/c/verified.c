// verified.c — VerifiedWeft: authenticated frames (RFC 0005).
// Driver-layer module — weft.c/weft.h untouched.

#include "verified.h"

#include <string.h>

static const char WEFT_VW_DOMAIN[] = "Weft-VerifiedWeft-v1:key";

void weft_vw_derive_key(const uint8_t* secret, size_t secret_len,
                        uint8_t out_key[WEFT_VW_KEY_LEN]) {
    hmac_sha256(secret, secret_len,
                (const uint8_t*)WEFT_VW_DOMAIN, sizeof(WEFT_VW_DOMAIN) - 1,
                out_key);
}

void weft_vw_signer_init(weft_vw_signer_t* s, const uint8_t auth_key[WEFT_VW_KEY_LEN]) {
    hmac_sha256_init_key(&s->key, auth_key, WEFT_VW_KEY_LEN);
}

void weft_vw_sign(weft_vw_signer_t* s,
                  const uint8_t* envelope, const uint8_t* payload, size_t payload_len,
                  uint8_t out_tag[WEFT_VW_TAG_LEN]) {
    // Only the first 16 envelope bytes are authenticated — the v1 envelope
    // layout (03-ENVELOPE) puts every integrity-relevant field there, and
    // larger header_size variants are a future RFC.
    hmac_sha256_update(&s->key, envelope, WEFT_VW_ENVELOPE_LEN);
    hmac_sha256_update(&s->key, payload, payload_len);
    hmac_sha256_final(&s->key, out_tag);
}

int weft_vw_ct_eq(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    // diff is 0 iff all bytes equal; map to 1/0 without a data-dependent branch
    // on content (the loop count is already length-only).
    return (int)((((uint32_t)diff | (~(uint32_t)diff + 1)) >> 8) & 1u) ^ 1;
}

weft_vw_result_t weft_vw_verify(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                const uint8_t* envelope, const uint8_t* payload,
                                size_t payload_len,
                                const uint8_t tag[WEFT_VW_TAG_LEN]) {
    hmac_sha256_key_t k;
    hmac_sha256_init_key(&k, auth_key, WEFT_VW_KEY_LEN);
    hmac_sha256_update(&k, envelope, WEFT_VW_ENVELOPE_LEN);
    hmac_sha256_update(&k, payload, payload_len);
    uint8_t expect[WEFT_VW_TAG_LEN];
    hmac_sha256_final(&k, expect);

    if (weft_vw_ct_eq(expect, tag, WEFT_VW_TAG_LEN) != 1) {
        return WEFT_VW_ERR_TAG;
    }
    return WEFT_VW_OK;
}

size_t weft_vw_record_encode(const uint8_t* envelope, const uint8_t* payload,
                             size_t payload_len, const uint8_t tag[WEFT_VW_TAG_LEN],
                             uint8_t* dst, size_t dst_len) {
    const size_t total = WEFT_VW_ENVELOPE_LEN + payload_len + WEFT_VW_TAG_LEN;
    if (dst_len < total) {
        return 0;
    }
    memcpy(dst, envelope, WEFT_VW_ENVELOPE_LEN);
    if (payload_len > 0) {
        memcpy(dst + WEFT_VW_ENVELOPE_LEN, payload, payload_len);
    }
    memcpy(dst + WEFT_VW_ENVELOPE_LEN + payload_len, tag, WEFT_VW_TAG_LEN);
    return total;
}

weft_vw_result_t weft_vw_record_decode_verify(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                              const uint8_t* src, size_t src_len,
                                              const uint8_t** envelope, const uint8_t** payload,
                                              size_t* payload_len) {
    // Minimum record: 16-byte envelope + 0-byte payload + 32-byte tag.
    if (src_len < WEFT_VW_ENVELOPE_LEN + WEFT_VW_TAG_LEN) {
        return WEFT_VW_ERR_SHORT;
    }
    if (!(src[0] == 'W' && src[1] == 'E' && src[2] == 'F' && src[3] == 'T')) {
        return WEFT_VW_ERR_BAD_MAGIC;
    }

    // Envelope v1 layout (03-ENVELOPE §1, all little-endian):
    //   magic [0..4), version [4..6), header_size [6..8), seq [8..12),
    //   payload_len [12..16). Payload begins at header_size (NEVER 16).
    const uint16_t header_size =
        (uint16_t)(src[6] | ((uint16_t)src[7] << 8));
    const uint32_t plen =
        (uint32_t)src[12] | ((uint32_t)src[13] << 8) |
        ((uint32_t)src[14] << 16) | ((uint32_t)src[15] << 24);

    if (header_size < WEFT_VW_ENVELOPE_LEN) {
        return WEFT_VW_ERR_BAD_MAGIC;  // malformed header geometry
    }
    // payload_len must fit between header end and the trailing tag.
    const size_t body = (size_t)header_size + plen;
    if (body > src_len - WEFT_VW_TAG_LEN) {
        return WEFT_VW_ERR_SHORT;
    }

    const weft_vw_result_t vr = weft_vw_verify(
        auth_key, src, src + header_size, plen, src + body);
    if (vr != WEFT_VW_OK) {
        return vr;
    }

    if (envelope) *envelope = src;
    if (payload) *payload = src + header_size;
    if (payload_len) *payload_len = plen;
    return WEFT_VW_OK;
}
