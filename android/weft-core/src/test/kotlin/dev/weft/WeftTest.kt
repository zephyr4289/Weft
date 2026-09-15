package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer

class WeftTest {

    @Test
    fun testPublishAndClaimBasic() {
        val payloadMax = 256
        val weft = Weft(payloadMax)

        assertEquals(0, weft.tPublishCount())
        assertEquals(0, weft.tClaimCount())

        val wBuf = weft.wBegin()
        wBuf.put(0, 0x42.toByte())
        wBuf.put(1, 0x99.toByte())

        val pubRes = weft.publish(seq = 1, payloadLen = payloadMax)
        assertEquals(PubResult.OK, pubRes)
        assertEquals(1, weft.tPublishCount())

        val heldIdx = weft.claim()
        assertEquals(1, weft.tClaimCount())
        assertEquals(1, weft.rSeq())
    }

    @Test
    fun testRevocationAndReclaim() {
        val weft = Weft(128)
        val e0 = weft.epochVal()

        weft.revoke()
        assertTrue(weft.isRevoked())

        val res = weft.publish(seq = 1, payloadLen = 128)
        assertEquals(PubResult.DROPPED_REVOKED, res)

        val reclaimed = weft.reclaim(preRevokeEpoch = e0, timeoutMs = 100)
        assertTrue(reclaimed)
    }

    @Test
    fun test1000FrameParityAndInvariants() {
        // T19.1 Cross-Package Parity Test: 1,000 frames @ 3,000 floats (W2 scale)
        val payloadFloats = 3000
        val payloadBytes = payloadFloats * 4
        val weft = Weft(payloadBytes)
        val totalFrames = 1000

        var lastClaimedSeq = 0
        var freshClaims = 0
        var staleClaims = 0

        for (seq in 1..totalFrames) {
            // I2: Writer step bound & publish
            val wBuf = weft.wBegin()
            wBuf.putInt(0, seq) // Embed seq in payload header
            wBuf.putFloat(4, (seq * 0.05f)) // Simulated RK4 particle coordinate

            val pubRes = weft.publish(seq = seq, payloadLen = payloadBytes)
            assertEquals("I2 Invariant: Publish must succeed", PubResult.OK, pubRes)

            // I3: Reader step bound & claim
            weft.claim()
            val claimedSeq = weft.rSeq()

            // I4: Monotonicity invariant
            assertTrue("I4 Invariant: Claimed seq must be monotonic", claimedSeq >= lastClaimedSeq)

            // I1: No torn read
            val rBuf = weft.rLiveBuf(0)
            val embeddedSeq = rBuf.getInt(0)
            assertEquals("I1 Invariant: Payload seq matches claimed seq (no torn read)", claimedSeq, embeddedSeq)

            if (claimedSeq > lastClaimedSeq) {
                freshClaims++
                // I5: Bounded staleness
                assertTrue("I5 Invariant: Staleness bound <= 1 frame", seq - claimedSeq <= 1)
                lastClaimedSeq = claimedSeq
            } else {
                staleClaims++
            }
        }

        assertEquals("All 1,000 frames published", totalFrames.toLong(), weft.tPublishCount())
        assertEquals("All 1,000 frames claimed", totalFrames.toLong(), weft.tClaimCount())
        assertEquals("Final claimed sequence reached", totalFrames, lastClaimedSeq)
        assertTrue("Steady-state fresh frames processed", freshClaims > 0)

        // I6: Clean teardown
        val ePre = weft.epochVal()
        weft.revoke()
        assertTrue("I6 Invariant: Revoked flag set", weft.isRevoked())
        val reclaimed = weft.reclaim(preRevokeEpoch = ePre, timeoutMs = 100)
        assertTrue("I6 Invariant: Epoch acknowledged and reclaimed", reclaimed)
    }
}
