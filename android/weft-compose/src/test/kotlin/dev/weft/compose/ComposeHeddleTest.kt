package dev.weft.compose

import dev.weft.Steward
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ComposeHeddleTest {

    @Test
    fun testHeddleCreationAndDisposal() {
        val steward = Steward()
        val weft = steward.weft<ByteArray>(256)
        val heddle = WeftHeddle(steward, weft, ReattachPolicy.PRESERVE_HELD)

        assertFalse(heddle.isDisposed)
        assertEquals(ReattachPolicy.PRESERVE_HELD, heddle.policy)

        heddle.dispose()
        assertTrue(heddle.isDisposed)
    }

    @Test
    fun testRecompositionSurvivalSimulation() {
        val steward = Steward()

        // Simulate 100 recompositions with preserved heddle
        val heddles = (1..100).map {
            val weft = steward.weft<ByteArray>(128)
            WeftHeddle(steward, weft)
        }

        assertEquals(100, heddles.size)
        heddles.forEach { assertFalse(it.isDisposed) }

        // Cleanup on scope clear
        heddles.forEach { it.dispose() }
        heddles.forEach { assertTrue(it.isDisposed) }
    }
}
