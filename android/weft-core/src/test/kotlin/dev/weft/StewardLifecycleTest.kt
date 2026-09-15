package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class StewardLifecycleTest {

    @Test
    fun testStewardAllocationAndRecompositionSurvival() {
        val steward = Steward()

        // Allocate 100 channels simulating recomposition survival
        val wefts = (1..100).map { steward.weft<ByteArray>(64) }
        assertEquals(100, wefts.size)

        val stats = steward.stats()
        assertEquals(100, stats.weftCount)

        // Release all
        steward.releaseAll()
        assertEquals(0, steward.dumpLeaks().size)
    }

    @Test
    fun testStewardOnClearedDisposal() {
        var disposed = false
        val steward = object : Steward() {
            override fun onCleared() {
                super.onCleared()
                disposed = true
            }
        }

        val w1 = steward.weft<FloatArray>(128)
        assertNotNull(w1)

        // Simulate ViewModel disposal
        steward.releaseAll()
        assertEquals(0, steward.dumpLeaks().size)
    }
}
