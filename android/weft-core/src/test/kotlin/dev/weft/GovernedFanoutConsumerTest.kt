// GovernedFanoutConsumerTest.kt — RFC-0009 Series 7: the composed display
// consumer conformance suite, Kotlin/JVM.
//
// The C-series (Consumer): the reader + governor + policy + two-frame
// history composition over the Series-7 recyclers — the exact-count
// steady-cadence regimes end to end, PC2 telescoping through the real
// fan-out ring, the ladder's advisory classes on real drop accounting,
// dispose-to-pool, and the per-tick zero-allocation audit (the R8
// discipline applied to the consumer's whole tick()).
//
// Runs two ways (the GovernorTest pattern): gradle in android-packages CI,
// and standalone kotlinc + junit (evidence: litmus/evidence/governor-vm/).

package dev.weft

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GovernedFanoutConsumerTest {

    private fun publish(b: WeftFanoutBroadcaster, seq: Long, words: Int) {
        val w = b.begin()
        for (i in 0 until words) {
            w.putInt(4 * i, (seq * 31 + i * 2654435761L).toInt())
        }
        b.publish()
    }

    // --- C1: PACED 30-on-120 end to end — the raster presents nearly
    //         every tick regardless of the feed's cadence ---

    @Test
    fun c1Paced30On120SteadyRasterEndToEnd() {
        val words = 64
        val b = WeftFanoutBroadcaster(words * 4, 4)
        val reader = WeftFanoutReader(b.ring, words * 4, 4)
        val pool = WeftBufferRecycler(slotBytes = words * 4, maxFreeSlots = 1)
        val consumer = GovernedFanoutConsumer(
            reader, CadencePolicyKind.PACED_INTERPOLATE, rasterPool = pool
        )
        var presents = 0
        var elided = 0
        for (t in 1..400) {
            if (t % 4 == 1) publish(b, (t / 4 + 1).toLong(), words) // 30 Hz feed
            val d = consumer.tick()
            if (d.present) presents++ else elided++
        }
        // The PC5 battery's exact count for this regime: 398 of 400
        // (first window saturates early; then every tick presents).
        assertEquals(398, presents)
        assertEquals(consumer.cadence.presents, presents.toLong())
        assertTrue(consumer.cadence.interpFrames > 0) // synthesis exercised
    }

    // --- C2: PC2 telescoping through the real ring (identity exact) ---

    @Test
    fun c2TelescopingHoldsThroughTheRing() {
        val words = 32
        val b = WeftFanoutBroadcaster(words * 4, 4)
        val reader = WeftFanoutReader(b.ring, words * 4, 4)
        for (kind in intArrayOf(
            CadencePolicyKind.LATEST_WINS, CadencePolicyKind.BURST_COALESCE
        )) {
            val consumer = GovernedFanoutConsumer(reader, kind)
            var state = 0x00c0ffeeL
            for (t in 1..10_000) {
                state = run {
                    var x = state and 0xffffffffL
                    x = x xor ((x shl 13) and 0xffffffffL)
                    x = x xor (x ushr 17)
                    x = x xor ((x shl 5) and 0xffffffffL)
                    x and 0xffffffffL
                }
                val arrivals = (state % 5).toInt()
                if (arrivals > 0 || t % 3 == 0) publish(b, t.toLong(), words)
                consumer.tick()
            }
            val c = consumer.cadence
            assertEquals("PC2 kind=$kind", c.lastPresentedSeqForTest() - c.presents,
                c.coalescedByDecision)
        }
    }

    // --- C3: the ladder sees the ring's REAL drop accounting ---

    @Test
    fun c3LadderClassesFromRealDropAccounting() {
        val words = 16
        val b = WeftFanoutBroadcaster(words * 4, 4)
        val reader = WeftFanoutReader(b.ring, words * 4, 4)
        val consumer = GovernedFanoutConsumer(reader, CadencePolicyKind.LATEST_WINS)
        var clock = 0L
        consumer.clock = { clock }

        // Steady feed: FastPath (behind == 0).
        publish(b, 1, words); consumer.tick()
        publish(b, 2, words)
        val d = consumer.tick()
        assertEquals(GovernorActionKind.FAST_PATH, consumer.action.kind)
        assertTrue(d.present)

        // Burst of 4 between ticks: dropped=3 -> Skip(2) ladder class.
        publish(b, 3, words); publish(b, 4, words)
        publish(b, 5, words); publish(b, 6, words)
        clock += 100
        consumer.tick()
        assertEquals(GovernorActionKind.SKIP, consumer.action.kind)
        assertEquals(2, consumer.action.skipN.toLong())
        assertTrue(consumer.actionChanged)

        // Behind 0 again: back to FastPath (edge).
        clock += 100
        consumer.tick()
        assertEquals(GovernorActionKind.FAST_PATH, consumer.action.kind)
        assertTrue(consumer.actionChanged)

        // Massive burst: dropped=40 -> Reseed (post-cooldown).
        clock += 1000
        for (s in 7..47L) publish(b, s, words)
        consumer.tick()
        assertEquals(GovernorActionKind.RESEED, consumer.action.kind)
        assertEquals(1, consumer.staleness.reseeds)
    }

    // --- C4: dispose returns the raster slot to the pool ---

    @Test
    fun c4DisposeReturnsRasterSlotToPool() {
        val words = 8
        val b = WeftFanoutBroadcaster(words * 4, 4)
        val reader = WeftFanoutReader(b.ring, words * 4, 4)
        val pool = WeftBufferRecycler(slotBytes = words * 4, maxFreeSlots = 2)
        val consumer = GovernedFanoutConsumer(
            reader, CadencePolicyKind.LATEST_WINS, rasterPool = pool
        )
        assertEquals(1, pool.liveNow)
        assertEquals(0, pool.pooledNow)
        val r = consumer.raster
        consumer.dispose()
        consumer.dispose() // idempotent
        assertEquals(0, pool.liveNow)
        assertEquals(1, pool.pooledNow)
        // The slot is reusable (pooled identity).
        assertTrue(pool.acquire() === r)
    }

    // --- C5: the whole tick() is allocation-free (the R8 discipline) ---

    @Test
    fun c5ConsumerTickZeroAllocationJvmAudit() {
        if (!HotspotAllocAudit.isAvailable) {
            println("C5 alloc audit SKIPPED (no com.sun.management.ThreadMXBean allocation counter)")
            return
        }
        val words = 64
        val b = WeftFanoutBroadcaster(words * 4, 4)
        val reader = WeftFanoutReader(b.ring, words * 4, 4)
        val pool = WeftBufferRecycler(slotBytes = words * 4, maxFreeSlots = 1)
        val consumer = GovernedFanoutConsumer(
            reader, CadencePolicyKind.PACED_INTERPOLATE, rasterPool = pool
        )
        consumer.clock = { 0L } // deterministic: cooldown state irrelevant here

        // The R8 pattern: the consumer runs on its OWN thread (the draw
        // thread); the test's inline publisher stays on the main thread —
        // its begin() slice wrappers cannot pollute the per-thread window.
        // ZERO-WINDOW RE-MEASUREMENT (documented, bounded — the R8 note):
        // tiered-compilation noise can land a tiny allocation inside any
        // single window; steady-state zero must hold on at least ONE of
        // three windows. A real leak (4.6 KB/claim before the
        // FanoutVhBridge fix) can never produce a zero window.
        val seqFlag = java.util.concurrent.atomic.AtomicInteger(0)
        val doneFlag = java.util.concurrent.atomic.AtomicInteger(0)
        val warmup = 20_000
        val measured = 100_000
        val windows = 3
        val presentsOut = java.util.concurrent.atomic.AtomicInteger(0)
        val deltasOut = java.util.concurrent.atomic.AtomicLongArray(windows)
        val consumerThread = Thread {
            val tid = Thread.currentThread().getId()
            var p = 0
            var w = 0
            var before = 0L
            var i = 0
            val total = warmup + measured * windows
            while (i < total) {
                i++
                while (seqFlag.get() < i) { /* spin */ }
                if (i == 1) HotspotAllocAudit.getThreadAllocatedBytes(tid) // warm the counter call
                if (i == warmup + measured * w + 1) {
                    before = HotspotAllocAudit.getThreadAllocatedBytes(tid) // window opens
                }
                if (consumer.tick().present) p++
                if (i == warmup + measured * (w + 1)) {
                    deltasOut.set(w, HotspotAllocAudit.getThreadAllocatedBytes(tid) - before)
                    w++
                }
                doneFlag.set(i)
            }
            presentsOut.set(p)
        }
        consumerThread.start()
        var seq = 0L
        var i = 0
        val total = warmup + measured * windows
        while (i < total) {
            i++
            if (i % 4 == 1) { // 30 Hz feed on the 120 Hz tick cadence
                seq += 1
                publish(b, seq, words)
            }
            seqFlag.set(i)
            while (doneFlag.get() < i) { /* spin */ }
        }
        consumerThread.join()

        var zeroWindow = false
        for (w in 0 until windows) {
            if (deltasOut.get(w) == 0L) zeroWindow = true
        }
        assertTrue(
            "C5: consumer tick() allocated in every window (deltas=" +
                (0 until windows).joinToString(",") { deltasOut.get(it).toString() } + ")",
            zeroWindow
        )
        assertTrue("C5: PACED exercised", presentsOut.get() > 10_000)
        assertEquals("C5: pool never reallocated", 0L, pool.reallocs)
        val r1 = consumer.raster
        consumer.tick()
        assertTrue("C5: raster identity stable", consumer.raster === r1)
    }
}

internal object HotspotAllocAudit {
    private val pair by lazy {
        try {
            val factoryClass = Class.forName("java.lang.management.ManagementFactory")
            val getBeanMethod = factoryClass.getMethod("getThreadMXBean")
            val bean = getBeanMethod.invoke(null)
            val isSupportedMethod = bean.javaClass.getMethod("isThreadAllocatedMemorySupported")
            if (isSupportedMethod.invoke(bean) == true) {
                val allocMethod = bean.javaClass.getMethod("getThreadAllocatedBytes", Long::class.javaPrimitiveType)
                Pair(bean, allocMethod)
            } else null
        } catch (_: Throwable) {
            null
        }
    }

    val isAvailable: Boolean get() = pair != null

    fun getThreadAllocatedBytes(threadId: Long): Long {
        val p = pair ?: return -1L
        return p.second.invoke(p.first, threadId) as Long
    }
}
