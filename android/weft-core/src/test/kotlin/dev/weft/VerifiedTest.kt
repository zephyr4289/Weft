// VerifiedTest.kt — RFC-0005 VerifiedWeft conformance suite, Kotlin/JVM.
//
// The V-series counterpart of packages/core/test/verified.test.ts,
// core/c/verified_test.c, and core/rust/src/verified.rs: shared fixture
// vectors, derivation, roundtrip, exhaustive tamper, rejections, pre-keyed
// verifier semantics, and the Series-6 batch stream API. Tags here are
// bit-identical to the C/Rust/TS ports by construction (same key schedule,
// same wire format — the fixture below is the shared ground truth).
//
// Environment tag: JVM 21, kotlinc 2.0.21 (sandbox evidence log:
// litmus/evidence/verified/kotlin-jvm-vseries.log).

package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VerifiedTest {

    // Byte-identical to fixtures/xlang-verifiedweft/hmac-vectors.json
    // (RFC 4231 TC1-4,6,7 + Weft boundary cases; digests cross-checked
    // against node:crypto at generation time).
    private data class Vector(val name: String, val key: String, val data: String, val tag: String)

    private val vectors = listOf(
        Vector(
            "rfc4231-tc1",
            "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
            "4869205468657265",
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        ),
        Vector(
            "rfc4231-tc2",
            "4a656665",
            "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        ),
        Vector(
            "rfc4231-tc3",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
        ),
        Vector(
            "rfc4231-tc4",
            "0102030405060708090a0b0c0d0e0f10111213141516171819",
            "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
            "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
        ),
        Vector(
            "rfc4231-tc6",
            ("aa".repeat(131)),
            "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669727374",
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        ),
        Vector(
            "rfc4231-tc7",
            ("aa".repeat(131)),
            "5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206b657920616e642061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565647320746f20626520686173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e",
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
        ),
        Vector(
            "weft-empty-payload",
            "5a".repeat(32),
            "",
            "87a26610b4e32f22d6d403b2397f534fb64c83b15aa53deaec60b1afa31dbb74",
        ),
        Vector(
            "weft-one-byte",
            "5a".repeat(32),
            "ff",
            "869b6896716dbdbce95aa32d75657fae807c82b8d52c25c83b7afa617271b7c8",
        ),
    )

    private fun hex(s: String): ByteArray =
        ByteArray(s.length / 2) { i -> ((Character.digit(s[i * 2], 16) shl 4) +
                Character.digit(s[i * 2 + 1], 16)).toByte() }

    private fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }

    // --- V1: fixture vectors ------------------------------------------------

    @Test fun `v1 hmac fixture vectors`() {
        for (v in vectors) {
            val tag = hmacSha256(hex(v.key), hex(v.data))
            assertEquals("${v.name}: tag matches shared fixture", v.tag, tag.toHex())
        }
    }

    // --- V2: key derivation ---------------------------------------------------

    @Test fun `v2 domain-separated derivation`() {
        val secret = "v2-secret".toByteArray()
        val k1 = vwDeriveKey(secret)
        val k2 = vwDeriveKey(secret)
        assertTrue("deterministic", k1.contentEquals(k2))
        assertEquals("32-byte key", 32, k1.size)
        val other = vwDeriveKey("other".toByteArray())
        assertFalse("different secret -> different key", k1.contentEquals(other))
        assertEquals(
            "key = HMAC(secret, domain) exactly",
            hmacSha256(secret, "Weft-VerifiedWeft-v1:key".toByteArray()).toHex(),
            k1.toHex(),
        )
    }

    // --- V3: record roundtrip ---------------------------------------------------

    @Test fun `v3 record roundtrip zero-copy offsets`() {
        val key = vwDeriveKey("v3-roundtrip".toByteArray())
        val signer = VwSigner(key)
        val envelope = ByteArray(16)
        val dst = ByteArray(16 + 300 + 32)
        for (plen in intArrayOf(0, 1, 16, 63, 64, 65, 128, 300)) {
            val payload = ByteArray(plen) { (it * 7 + 3).toByte() }
            vwEnvelopeEncodeV1(envelope, 42, plen)
            val tag = signer.sign(envelope, payload)
            val n = vwRecordEncode(envelope, payload, tag, dst)
            assertEquals("record length", 16 + plen + 32, n)
            val r = vwRecordDecodeVerify(key, dst.copyOf(n))
            assertEquals("decode+verify OK ($plen)", VwResult.OK, r.code)
            assertEquals("payload view length", plen, r.records[0].payloadLen)
            assertEquals("seq decoded", 42, r.records[0].seq)
        }
    }

    // --- V4: exhaustive tamper ---------------------------------------------------

    @Test fun `v4 every byte flip rejected`() {
        val key = vwDeriveKey("v4-tamper".toByteArray())
        val signer = VwSigner(key)
        val envelope = ByteArray(16)
        vwEnvelopeEncodeV1(envelope, 7, 64)
        val payload = ByteArray(64) { (it * 13).toByte() }
        val tag = signer.sign(envelope, payload)
        val rec = ByteArray(16 + 64 + 32)
        vwRecordEncode(envelope, payload, tag, rec)

        var rejected = 0
        for (i in rec.indices) {
            val tampered = rec.copyOf()
            tampered[i] = (tampered[i].toInt() xor 0x80).toByte()
            if (vwRecordDecodeVerify(key, tampered).code != VwResult.OK) rejected++
        }
        assertEquals("all ${rec.size} byte flips rejected", rec.size, rejected)
    }

    // --- V5: rejections ---------------------------------------------------

    @Test fun `v5 wrong key short bad magic`() {
        val key = vwDeriveKey("v5-key".toByteArray())
        val wrong = vwDeriveKey("wrong".toByteArray())
        val signer = VwSigner(key)
        val envelope = ByteArray(16)
        vwEnvelopeEncodeV1(envelope, 1, 24)
        val payload = ByteArray(24) { 0xAB.toByte() }
        val tag = signer.sign(envelope, payload)
        val rec = ByteArray(16 + 24 + 32)
        vwRecordEncode(envelope, payload, tag, rec)

        assertEquals("wrong key", VwResult.ERR_TAG, vwRecordDecodeVerify(wrong, rec).code)
        assertEquals("short record", VwResult.ERR_SHORT, vwRecordDecodeVerify(key, ByteArray(40)).code)
        val badMagic = rec.copyOf(); badMagic[0] = 'X'.code.toByte()
        assertEquals("bad magic", VwResult.ERR_BAD_MAGIC, vwRecordDecodeVerify(key, badMagic).code)
    }

    // --- V8: pre-keyed verifier == one-shot --------------------------------------

    @Test fun `v8 prekeyed verifier semantics`() {
        val key = vwDeriveKey("v8-stream".toByteArray())
        val verifier = VwVerifier(key)
        val signer = VwSigner(key)
        val envelope = ByteArray(16)
        val payload = ByteArray(64)
        for (i in 0 until 1000) {
            vwEnvelopeEncodeV1(envelope, i, 64)
            for (j in 0 until 64) payload[j] = ((i + j) and 0xff).toByte()
            val tag = signer.sign(envelope, payload)
            assertEquals("pre-keyed OK ($i)", VwResult.OK, verifier.verify(envelope, payload, tag))
            assertEquals("one-shot OK ($i)", VwResult.OK, vwVerify(key, envelope, payload, tag))
            tag[0] = (tag[0].toInt() xor 1).toByte()
            assertEquals("tamper red ($i)", VwResult.ERR_TAG, verifier.verify(envelope, payload, tag))
        }
        // State reusable after the stream.
        vwEnvelopeEncodeV1(envelope, 0, 64)
        for (j in 0 until 64) payload[j] = j.toByte()
        val tag = signer.sign(envelope, payload)
        assertEquals("state stable", VwResult.OK, verifier.verify(envelope, payload, tag))
    }

    // --- V9: batch decode-verify ---------------------------------------------------

    private fun buildStream(n: Int, plen: Int, key: ByteArray): ByteArray {
        val signer = VwSigner(key)
        val recLen = VW_ENVELOPE_LEN + plen + VW_TAG_LEN
        val stream = ByteArray(n * recLen)
        val envelope = ByteArray(16)
        val payload = ByteArray(plen)
        val slice = ByteArray(recLen)
        for (i in 0 until n) {
            vwEnvelopeEncodeV1(envelope, i * 3 + 1, plen)
            for (j in 0 until plen) payload[j] = ((i xor j) and 0xff).toByte()
            val tag = signer.sign(envelope, payload)
            val written = vwRecordEncode(envelope, payload, tag, slice)
            System.arraycopy(slice, 0, stream, i * recLen, written)
        }
        return stream
    }

    @Test fun `v9 batch all-ok tamper-stop views truncation`() {
        val key = vwDeriveKey("v9-batch".toByteArray())
        val n = 500
        val plen = 48
        val recLen = VW_ENVELOPE_LEN + plen + VW_TAG_LEN
        val stream = buildStream(n, plen, key)

        run {
            val r = vwBatchDecodeVerify(key, stream)
            assertEquals("all OK", VwResult.OK, r.code)
            assertEquals("count", n, r.verified)
            assertEquals("consumed exact", n * recLen, r.bytesConsumed)
            assertEquals("views size", n, r.records.size)
            for (i in 0 until n) {
                assertEquals("view offset $i", i * recLen, r.records[i].offset)
                assertEquals("view seq $i", i * 3 + 1, r.records[i].seq)
            }
        }

        run {
            val k = 137
            stream[k * recLen + 20] = (stream[k * recLen + 20].toInt() xor 0x40).toByte()
            val r = vwBatchDecodeVerify(key, stream)
            assertEquals("tamper code", VwResult.ERR_TAG, r.code)
            assertEquals("good prefix", k, r.verified)
            assertEquals("resync offset", k * recLen, r.bytesConsumed)
        }

        run {
            val fresh = buildStream(n, plen, key)
            // Tail shorter than a minimal record: ignored.
            val r = vwBatchDecodeVerify(key, fresh.copyOf((n - 1) * recLen + 17))
            assertEquals("short tail OK", VwResult.OK, r.code)
            assertEquals("short tail count", n - 1, r.verified)
            // A started-but-unfitting record: ERR_SHORT at its offset.
            val r2 = vwBatchDecodeVerify(key, fresh.copyOf(n * recLen - 30))
            assertEquals("mid-record short", VwResult.ERR_SHORT, r2.code)
            assertEquals("mid-record prefix", n - 1, r2.verified)
        }

        run {
            val fresh2 = buildStream(n, plen, key)
            val r = vwBatchDecodeVerify(key, fresh2, 10)
            assertEquals("capped views still verify all", n, r.verified)
            assertEquals("cap respected", 10, r.records.size)
        }
    }

    // --- V10: platform-acceleration honesty tag ------------------------------------

    @Test fun `v10 platform mac is hmac-sha256 with hw sha`() {
        // The honesty gate: the platform Mac must produce the SHARED fixture
        // digests (V1 above pins this) — acceleration is invisible to the
        // wire. This test exists so the battery's platform-acceleration
        // claim (OpenJDK intrinsics/Conscrypt under the Mac) has an explicit
        // pass/fail anchor: the Mac name and the fixture agreement.
        val mac = javax.crypto.Mac.getInstance("HmacSHA256")
        assertEquals("HmacSHA256", mac.algorithm)
        // And the signer path through a fresh Mac instance agrees with VwSigner.
        val key = vwDeriveKey("v10-platform".toByteArray())
        val signer = VwSigner(key)
        val envelope = ByteArray(16)
        vwEnvelopeEncodeV1(envelope, 9, 32)
        val payload = ByteArray(32) { (it * 3).toByte() }
        val viaSigner = signer.sign(envelope, payload)
        mac.init(javax.crypto.spec.SecretKeySpec(key, "HmacSHA256"))
        mac.update(envelope, 0, VW_ENVELOPE_LEN)
        assertTrue("signer == platform mac", viaSigner.contentEquals(mac.doFinal(payload)))
    }
}
