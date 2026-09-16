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

// --- Series 6: pre-keyed verifier + batch stream verification ---------------
// weft_vw_verify() re-derives the HMAC key schedule on every call — two extra
// compressions per frame, the classic one-shot API shape. Consumers that
// verify a stream of records from ONE key (flight-recorder ingest, WebSocket
// bridge, cross-origin SAB readers — RFC 0005's own motivation) can pre-key
// once and amortize the schedule across the whole stream.

/// Reusable per-stream verifier state (pre-keyed; the verify mirror of
/// weft_vw_signer_t). No allocation; reseeded by hmac final, same as signer.
typedef struct weft_vw_verifier {
    hmac_sha256_key_t key;
} weft_vw_verifier_t;

void weft_vw_verifier_init(weft_vw_verifier_t* v, const uint8_t auth_key[WEFT_VW_KEY_LEN]);

/// Verify one frame with the pre-keyed state. Identical accept/reject
/// semantics to weft_vw_verify (same codes, same constant-time compare);
/// only the key schedule is amortized. Hot path for stream consumers.
weft_vw_result_t weft_vw_verifier_verify(weft_vw_verifier_t* v,
                                         const uint8_t* envelope,
                                         const uint8_t* payload, size_t payload_len,
                                         const uint8_t tag[WEFT_VW_TAG_LEN]);

/// Zero-copy view of one verified record inside a batch buffer.
typedef struct weft_vw_record_view {
    const uint8_t* envelope;  // 16 bytes, points INTO the batch buffer
    const uint8_t* payload;   // payload_len bytes, points INTO the buffer
    size_t payload_len;
    uint32_t seq;             // envelope seq field, decoded little-endian
} weft_vw_record_view_t;

/// Walk a buffer of concatenated auth records, verifying each in order.
/// Semantics (Law 4 — drop and count, never consume):
///   - returns WEFT_VW_OK when ALL records verify; *n_verified = record count
///   - stops at the FIRST bad record, returning its code; *n_verified then
///     counts only the good prefix — the caller drops/logs from there
///   - *bytes_consumed = end offset of the last VERIFIED record, so a stream
///     consumer can resume a re-synced stream after a bad record
///   - views (optional, may be NULL) are filled for verified records only,
///     zero-copy into src; views_cap bounds the fill, extra records still
///     verify and count
/// Trailing bytes short of a full record are ignored (n_bytes_consumed < src_len
/// is legal); the caller decides whether that is truncation or a partial read.
weft_vw_result_t weft_vw_batch_decode_verify(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                             const uint8_t* src, size_t src_len,
                                             weft_vw_record_view_t* views,
                                             size_t views_cap,
                                             size_t* n_verified,
                                             size_t* bytes_consumed);

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
