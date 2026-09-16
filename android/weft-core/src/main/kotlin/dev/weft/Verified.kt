// Verified.kt — RFC 0005: VerifiedWeft authenticated frames, Kotlin/JVM driver layer
//
// WHY EXISTS: RFC 0005 shipped VerifiedWeft to the three canonical kernels
// (core/c/verified.{h,c}, core/rust/src/verified.rs, core/ts/verified.ts,
// PR #6) — but the three VM ports had no equivalent: Android apps and JVM
// engines consuming authenticated records (a .weftrec bridge, a WebSocket
// feed, cross-process IPC) had to detour through JNI to the C verifier.
// This module completes the six-port story with the SAME wire format, the
// SAME key schedule, and the SAME result codes, so a record produced by any
// port verifies in Kotlin bit-exactly (docs/PORTS.md §7; the shared fixture
// is fixtures/xlang-verifiedweft/hmac-vectors.json).
//
// WIRE FORMAT (RFC 0005 "extended record" — identical bytes in all ports):
//   [0..16)                     frame envelope v1 (magic "WEFT", version 1,
//                               header_size 16, seq u32 LE, payload_len u32 LE)
//   [16..16+payload_len)        payload
//   [16+payload_len..+32)       HMAC-SHA256 tag
// KEY SCHEDULE (domain separation):
//   auth_key = HMAC-SHA256(secret, "Weft-VerifiedWeft-v1:key")
//   tag      = HMAC-SHA256(auth_key, envelope[0..16] || payload)
// RESULT CODES: 0 OK / 1 short / 2 bad-magic / 3 tag — numerically identical
// to weft_vw_result_t (C) and VW_* (TS).
//
// THE PLATFORM IS THE ACCELERATOR (Series 6 HW story, per-port honesty):
// javax.crypto.Mac("HmacSHA256") delegates to the JVM's native crypto —
// OpenJDK's intrinsics/OpenSSL-backed provider on server JVMs (SHA-NI),
// BoringSSL/Conscrypt on Android (ARMv8 CE). Kotlin cannot reach those
// instructions directly (no intrinsics surface); the platform library is
// the honest road — the same story as the TS port's WebCrypto path. The
// JVM provider applies the HMAC key schedule ONCE per Mac instance (init),
// so VwSigner/VwVerifier are pre-keyed by construction: one init, N
// doFinal calls. Measured numbers are environment-tagged in the evidence
// log, not asserted here.
//
// GUARANTEE BOUNDARY (Law 4): integrity + authenticity of frame contents.
// A record that fails verification is DROPPED and counted, never consumed.
// No DoS protection (RFC 0005 boundary); latest-wins untouched.

package dev.weft

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import java.security.MessageDigest

/** Result codes — numeric parity with weft_vw_result_t (C) / VW_* (TS). */
object VwResult {
    const val OK: Int = 0
    const val ERR_SHORT: Int = 1
    const val ERR_BAD_MAGIC: Int = 2
    const val ERR_TAG: Int = 3
}

/** Derived auth-key length (bytes). */
const val VW_KEY_LEN = 32

/** HMAC-SHA256 tag length (bytes). */
const val VW_TAG_LEN = 32

/** Signed envelope prefix length (bytes). */
const val VW_ENVELOPE_LEN = 16

private val VW_DOMAIN = "Weft-VerifiedWeft-v1:key".toByteArray(Charsets.US_ASCII)

/**
 * One-time auth-key derivation with domain separation.
 * auth_key = HMAC-SHA256(secret, "Weft-VerifiedWeft-v1:key").
 */
fun vwDeriveKey(secret: ByteArray): ByteArray = hmacSha256(secret, VW_DOMAIN)

/// One-shot HMAC-SHA256 (setup + conformance use; the hot paths use the
/// pre-keyed signer/verifier classes below).
fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec(key, "HmacSHA256"))
    return mac.doFinal(data)
}

/// Constant-time equality — MessageDigest.isEqual is the platform's
/// content-independent comparison (documented constant-time for equal
/// lengths); the same platform-delegation story as Mac itself.
fun vwCtEq(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

/**
 * Reusable pre-keyed signer: one Mac init (the platform applies the HMAC
 * key schedule there), N sign calls. The Kotlin mirror of
 * weft_vw_signer_t (C) / VerifiedWeftSigner (TS).
 */
class VwSigner(authKey: ByteArray) {
    private val mac: Mac = Mac.getInstance("HmacSHA256").apply {
        init(SecretKeySpec(authKey, "HmacSHA256"))
    }

    /** tag = HMAC(auth_key, envelope[0..16] || payload). */
    fun sign(envelope: ByteArray, payload: ByteArray): ByteArray {
        mac.update(envelope, 0, VW_ENVELOPE_LEN)
        return mac.doFinal(payload)
    }
}

/**
 * Reusable pre-keyed verifier — accept/reject identical to the one-shot
 * [vwVerify]; the key schedule is amortized in the platform Mac, the
 * semantics are not. Stream-consumer hot path (Series 6).
 *
 * Range-based feed/finish let the batch walk verify records IN a buffer
 * without per-record payload copies (the JVM exposes Mac.doFinal(byte[],
 * int, int) for exactly this shape).
 */
class VwVerifier(authKey: ByteArray) {
    private val mac: Mac = Mac.getInstance("HmacSHA256").apply {
        init(SecretKeySpec(authKey, "HmacSHA256"))
    }

    /** Feed a buffer range (the envelope prefix, then payload). */
    fun feed(buf: ByteArray, off: Int, len: Int) {
        mac.update(buf, off, len)
    }

    /** Finalize over a payload range; returns the tag. Mac resets for the
     *  next message (pre-keyed by init, reseeded by the platform). Uses
     *  update(range) + doFinal() — zero-copy; this JDK's Mac has no
     *  doFinal(input, off, len) overload. */
    fun finish(buf: ByteArray, off: Int, len: Int): ByteArray {
        mac.update(buf, off, len)
        return mac.doFinal()
    }

    /** Returns VwResult.OK only on a byte-exact constant-time tag match. */
    fun verify(envelope: ByteArray, payload: ByteArray, tag: ByteArray): Int {
        mac.update(envelope, 0, VW_ENVELOPE_LEN)
        return if (vwCtEq(mac.doFinal(payload), tag)) VwResult.OK else VwResult.ERR_TAG
    }
}

/// Verify one frame with a one-shot key schedule (API compat mirror of
/// weft_vw_verify / verifiedWeftVerify).
fun vwVerify(authKey: ByteArray, envelope: ByteArray, payload: ByteArray, tag: ByteArray): Int =
    VwVerifier(authKey).verify(envelope, payload, tag)

/// Encode envelope v1 (magic "WEFT", version 1, header_size 16, seq,
/// payload_len — all little-endian), mirroring weftEnvelopeEncodeV1 (TS).
fun vwEnvelopeEncodeV1(dst: ByteArray, seq: Int, payloadLen: Int) {
    dst[0] = 0x57; dst[1] = 0x45; dst[2] = 0x46; dst[3] = 0x54 // "WEFT"
    dst[4] = 1; dst[5] = 0            // version u16 LE
    dst[6] = 16; dst[7] = 0           // header_size u16 LE
    dst[8] = (seq and 0xff).toByte()
    dst[9] = ((seq ushr 8) and 0xff).toByte()
    dst[10] = ((seq ushr 16) and 0xff).toByte()
    dst[11] = ((seq ushr 24) and 0xff).toByte()
    dst[12] = (payloadLen and 0xff).toByte()
    dst[13] = ((payloadLen ushr 8) and 0xff).toByte()
    dst[14] = ((payloadLen ushr 16) and 0xff).toByte()
    dst[15] = ((payloadLen ushr 24) and 0xff).toByte()
}

/// Encode a full auth record into dst; returns bytes written (0 if too small).
fun vwRecordEncode(envelope: ByteArray, payload: ByteArray, tag: ByteArray, dst: ByteArray): Int {
    val total = VW_ENVELOPE_LEN + payload.size + VW_TAG_LEN
    if (dst.size < total) return 0
    System.arraycopy(envelope, 0, dst, 0, VW_ENVELOPE_LEN)
    System.arraycopy(payload, 0, dst, VW_ENVELOPE_LEN, payload.size)
    System.arraycopy(tag, 0, dst, VW_ENVELOPE_LEN + payload.size, VW_TAG_LEN)
    return total
}

/** Zero-copy view of one record inside a buffer (offsets, not copies). */
class VwRecordView(
    /** Record start offset in the source buffer. */
    val offset: Int,
    /** Envelope prefix length (16 for v1). */
    val envelopeLen: Int,
    /** Payload start offset in the source buffer. */
    val payloadOffset: Int,
    /** Payload length in bytes. */
    val payloadLen: Int,
    /** Envelope seq field, decoded little-endian. */
    val seq: Int,
)

/** Batch decode-verify result — mirrors the C/TS/Rust batch APIs. */
class VwBatchResult(
    /** VwResult.OK, or the code of the FIRST bad record. */
    val code: Int,
    /** Count of verified records (the good prefix). */
    val verified: Int,
    /** End offset of the last VERIFIED record (resync point). */
    val bytesConsumed: Int,
    /** Zero-copy views of verified records (bounded by maxViews). */
    val records: List<VwRecordView>,
)

/**
 * Decode + verify ONE record from wire bytes (zero-copy: offsets into src).
 * Mirrors weft_vw_record_decode_verify / verifiedWeftRecordDecodeVerify.
 */
fun vwRecordDecodeVerify(authKey: ByteArray, src: ByteArray): VwBatchResult {
    if (src.size < VW_ENVELOPE_LEN + VW_TAG_LEN) {
        return VwBatchResult(VwResult.ERR_SHORT, 0, 0, emptyList())
    }
    if (!(src[0] == 0x57.toByte() && src[1] == 0x45.toByte() &&
                src[2] == 0x46.toByte() && src[3] == 0x54.toByte())) {
        return VwBatchResult(VwResult.ERR_BAD_MAGIC, 0, 0, emptyList())
    }
    val headerSize = (src[6].toInt() and 0xff) or ((src[7].toInt() and 0xff) shl 8)
    // Unsigned decode (C: size_t / TS: >>> 0 / Rust: usize) — a hostile
    // high-bit payload_len must land in ERR_SHORT, never wrap negative.
    val plen = ((src[12].toLong() and 0xff) or ((src[13].toLong() and 0xff) shl 8) or
            ((src[14].toLong() and 0xff) shl 16) or ((src[15].toLong() and 0xff) shl 24))
    if (headerSize < VW_ENVELOPE_LEN) {
        return VwBatchResult(VwResult.ERR_BAD_MAGIC, 0, 0, emptyList())
    }
    val body = headerSize.toLong() + plen
    if (body > src.size - VW_TAG_LEN) {
        return VwBatchResult(VwResult.ERR_SHORT, 0, 0, emptyList())
    }
    val tag = src.copyOfRange(body.toInt(), (body + VW_TAG_LEN).toInt())
    val verifier = VwVerifier(authKey)
    val code = verifier.verify(src, src.copyOfRange(headerSize, body.toInt()), tag)
    if (code != VwResult.OK) {
        return VwBatchResult(code, 0, 0, emptyList())
    }
    val seq = (src[8].toInt() and 0xff) or ((src[9].toInt() and 0xff) shl 8) or
            ((src[10].toInt() and 0xff) shl 16) or ((src[11].toInt() and 0xff) shl 24)
    return VwBatchResult(
        VwResult.OK, 1, (body + VW_TAG_LEN).toInt(),
        listOf(VwRecordView(0, VW_ENVELOPE_LEN, headerSize, plen.toInt(), seq)),
    )
}

/**
 * Walk a buffer of concatenated auth records, verifying each in order
 * (Series 6 stream-consumer path; Law 4 drop-and-count preserved).
 *
 * Stops at the first bad record (code + good-prefix count + its offset);
 * trailing bytes shorter than a minimal record are ignored — a record that
 * STARTS but does not FIT is ERR_SHORT, identical to the C/TS/Rust ports.
 * maxViews bounds the returned views (verification covers the whole prefix).
 */
fun vwBatchDecodeVerify(
    authKey: ByteArray,
    src: ByteArray,
    maxViews: Int = Int.MAX_VALUE,
): VwBatchResult {
    val verifier = VwVerifier(authKey)
    val records = ArrayList<VwRecordView>()
    var verified = 0
    var off = 0

    while (off + VW_ENVELOPE_LEN + VW_TAG_LEN <= src.size) {
        if (!(src[off] == 0x57.toByte() && src[off + 1] == 0x45.toByte() &&
                    src[off + 2] == 0x46.toByte() && src[off + 3] == 0x54.toByte())) {
            return VwBatchResult(VwResult.ERR_BAD_MAGIC, verified, off, records)
        }
        val headerSize = (src[off + 6].toInt() and 0xff) or ((src[off + 7].toInt() and 0xff) shl 8)
        // Unsigned decode — hostile geometry lands in ERR_SHORT (see above).
        val plen = ((src[off + 12].toLong() and 0xff) or ((src[off + 13].toLong() and 0xff) shl 8) or
                ((src[off + 14].toLong() and 0xff) shl 16) or ((src[off + 15].toLong() and 0xff) shl 24))
        if (headerSize < VW_ENVELOPE_LEN) {
            return VwBatchResult(VwResult.ERR_BAD_MAGIC, verified, off, records)
        }
        val body = headerSize.toLong() + plen
        if (body > src.size - off - VW_TAG_LEN) {
            return VwBatchResult(VwResult.ERR_SHORT, verified, off, records)
        }

        verifier.feed(src, off, VW_ENVELOPE_LEN)
        val expect = verifier.finish(src, off + headerSize, plen.toInt())
        val tagOff = (off + body).toInt()
        var diff: Int = 0
        for (i in 0 until VW_TAG_LEN) {
            diff = diff or ((expect[i].toInt() xor src[tagOff + i].toInt()) and 0xff)
        }
        if (diff != 0) {
            return VwBatchResult(VwResult.ERR_TAG, verified, off, records)
        }

        if (verified < maxViews) {
            val seq = (src[off + 8].toInt() and 0xff) or ((src[off + 9].toInt() and 0xff) shl 8) or
                    ((src[off + 10].toInt() and 0xff) shl 16) or ((src[off + 11].toInt() and 0xff) shl 24)
            records.add(VwRecordView(off, VW_ENVELOPE_LEN, off + headerSize, plen.toInt(), seq))
        }
        verified++
        off += (body + VW_TAG_LEN).toInt()
    }
    return VwBatchResult(VwResult.OK, verified, off, records)
}
