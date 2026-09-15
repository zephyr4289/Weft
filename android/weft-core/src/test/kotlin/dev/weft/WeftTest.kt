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
        val e0 = weft.epoch()

        weft.revoke()
        assertTrue(weft.isRevoked())

        val res = weft.publish(seq = 1, payloadLen = 128)
        assertEquals(PubResult.DROPPED_REVOKED, res)

        val reclaimed = weft.reclaim(preRevokeEpoch = e0, timeoutMs = 100)
        assertTrue(reclaimed)
    }
}
