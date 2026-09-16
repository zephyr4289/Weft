// verified.h — VerifiedWeft: authenticated frames (RFC 0005).
//
// Driver-layer module. The frozen kernel (weft.c/weft.h) is untouched and
// unaware of this layer: authentication wraps the wire-level frame RECORD
// (envelope || payload || tag), not the triad buffers. A verified stream is
// a record stream — e.g. a .weftrec leg, a WebSocket bridge, or any
// untrusted IPC channel — that consumers verify BEFORE handing payloads to
// a Triad publisher.
//
// Wire format (RFC 0005 "extended record"):
//   [0..16)                     frame envelope v1 (weft_envelope_encode_v1)
//   [16..16+payload_len)        payload
//   [16+payload_len..+32)       HMAC-SHA256 tag
//
// Key schedule (domain separation, RFC 0005 §reference):
//   auth_key = HMAC-SHA256(secret, "Weft-VerifiedWeft-v1:key")
//   tag      = HMAC-SHA256(auth_key, envelope || payload)
//
// Guarantees (Law 4 boundary): integrity + authenticity of frame contents.
// Does NOT prevent DoS via packet flooding, and does NOT repudiate the
// latest-wins drop contract: a frame that fails verification MUST be
// dropped and counted, never consumed.

#ifndef WEFT_VERIFIED_H
#define WEFT_VERIFIED_H

#include <stddef.h>
#include <stdint.h>

#include "hmac.h"
#include "sha256.h"

#define WEFT_VW_KEY_LEN     32  // derived auth key
#define WEFT_VW_TAG_LEN     32  // HMAC-SHA256 tag
#define WEFT_VW_ENVELOPE_LEN 16 // frame envelope v1

/// Result codes for the verify path.
typedef enum {
    WEFT_VW_OK            = 0,
    WEFT_VW_ERR_SHORT     = 1,  // record shorter than 16 + 32 bytes minimum
    WEFT_VW_ERR_BAD_MAGIC = 2,  // envelope magic != "WEFT"
    WEFT_VW_ERR_TAG       = 3,  // HMAC mismatch — tampered or wrong key
} weft_vw_result_t;

/// One-time key derivation with domain separation. secret may be any length.
void weft_vw_derive_key(const uint8_t* secret, size_t secret_len,
                        uint8_t out_key[WEFT_VW_KEY_LEN]);

/// Reusable per-stream signer state (pre-keyed; two compressions per frame
/// beyond payload blocks). No allocation.
typedef struct weft_vw_signer {
    hmac_sha256_key_t key;
} weft_vw_signer_t;

void weft_vw_signer_init(weft_vw_signer_t* s, const uint8_t auth_key[WEFT_VW_KEY_LEN]);

/// Sign one frame record's bytes: tag = HMAC(auth_key, envelope || payload).
/// envelope must be >= 16 bytes (only the first 16 are signed).
void weft_vw_sign(weft_vw_signer_t* s,
                  const uint8_t* envelope, const uint8_t* payload, size_t payload_len,
                  uint8_t out_tag[WEFT_VW_TAG_LEN]);

/// Verify one frame record's bytes in constant time.
/// Returns WEFT_VW_OK only when the tag matches byte-for-byte (timing-safe).
weft_vw_result_t weft_vw_verify(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                const uint8_t* envelope, const uint8_t* payload,
                                size_t payload_len,
                                const uint8_t tag[WEFT_VW_TAG_LEN]);

/// Encode a full auth record into dst:
///   dst_len must be >= 16 + payload_len + 32. Returns bytes written, or 0.
size_t weft_vw_record_encode(const uint8_t* envelope, const uint8_t* payload,
                             size_t payload_len, const uint8_t tag[WEFT_VW_TAG_LEN],
                             uint8_t* dst, size_t dst_len);

/// Decode + verify a record from wire bytes. On WEFT_VW_OK the envelope and
/// payload pointers are set to views INTO src (zero-copy); payload_len is set.
weft_vw_result_t weft_vw_record_decode_verify(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                              const uint8_t* src, size_t src_len,
                                              const uint8_t** envelope, const uint8_t** payload,
                                              size_t* payload_len);

/// Constant-time equality — exposed for tests and callers building custom
/// verification paths. Runs in time independent of the mismatch position.
int weft_vw_ct_eq(const uint8_t* a, const uint8_t* b, size_t len);

#endif // WEFT_VERIFIED_H
