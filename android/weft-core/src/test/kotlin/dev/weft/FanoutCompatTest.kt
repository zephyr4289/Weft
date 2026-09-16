package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * FanoutCompatTest.kt — the API<33 regime of the fan-out matrix.
 *
 * FanoutTest.kt pins the VarHandle ring (API 33+); THIS class pins the
 * compat ring (AtomicLongArray SC stamps + bracket-discipline payload,
 * FanoutCompat.kt) through the SAME F-series gates — plus the factory
 * routing that decides which regime a device takes. Both classes run in
 * the same gradle invocation, so the MATRIX (both regimes, every gate)
 * is exercised on every host, regardless of the host's own API level.
 *
 * Environment tag: JUnit/JVM (sandbox + CI). Real API<33 devices are the
 * emulator matrix leg (android-emulator.yml, API 26).
 */
class FanoutCompatTest {

    // --- shared mixer (weft_mix32, 04-LITMUS §0.1 — identical to FanoutTest) ---

    private fun mix32(xIn: Int): Int {
        var x = xIn
        x = x xor (x ushr 16)
        x *= 0x7FEB352D.toInt()
        x = x xor (x ushr 15)
        x *= 0x846CA68B.toInt()
        x = x xor (x ushr 16)
        return x
    }

    private fun tword(seq: Int, w: Int): Int = mix32(seq * 2654435761.toInt() + w)

    private fun fillFrame(b: WeftFanoutBroadcasterCompat, seq: Int, words: Int) {
        val src = IntArray(words) { w -> tword(seq, w) }
        b.begin()
        assertEquals(words, b.fill(src, words))
    }

    private fun expectFrame(view: IntArray, seq: Int, words: Int): Boolean {
        for (w in 0 until words) {
            if (view[w] != tword(seq, w)) return false
        }
        return true
    }

    // ----- F1c: geometry validation (constructor contracts) -----

    @Test
    fun f1cGeometryValidation() {
        // Bad payload sizes throw.
        for (bad in intArrayOf(0, -4, 6, 3)) {
            try {
                WeftFanoutBroadcasterCompat(bad, 4)
                throw AssertionError("payloadBytes=$bad must throw")
            } catch (e: IllegalArgumentException) { /* expected */ }
        }
        // Bad slot counts throw.
        for (bad in intArrayOf(0, 1, 65, -2)) {
            try {
                WeftFanoutBroadcasterCompat(64, bad)
                throw AssertionError("slotCount=$bad must throw")
            } catch (e: IllegalArgumentException) { /* expected */ }
        }
        // Reader-side geometry mismatch fails fast instead of tearing.
        val b = WeftFanoutBroadcasterCompat(64, 4)
        try {
            WeftFanoutReaderCompat(b.ctrl, IntArray(10), 64, 4)
            throw AssertionError("payload word-count mismatch must throw")
        } catch (e: IllegalArgumentException) { /* expected */ }
        assertEquals(4 * 16, weftFanoutCompatPayloadWords(64, 4))
        assertEquals(0, weftFanoutCompatPayloadWords(6, 4))
    }

    // ----- F2c: roundtrip (no frame -> first publish -> fresh claim) -----

    @Test
    fun f2cRoundtrip() {
        val b = WeftFanoutBroadcasterCompat(64, 4)
        val r = b.createReader()
        // Fresh ring: no frame yet.
        val c0 = r.claim()
        assertFalse(c0.fresh)
        assertEquals(0L, c0.seq)
        // First publish claims fresh with intact payload.
        fillFrame(b, 1, 16)
        assertEquals(1L, b.publish())
        val c1 = r.claim()
        assertTrue(c1.fresh)
        assertEquals(1L, c1.seq)
        assertEquals(0L, c1.dropped)
        assertTrue(expectFrame(r.view(), 1, 16))
        // Telemetry counted.
        assertEquals(1L, b.debugStats().publishes)
    }

    // ----- F3c: drop accounting -----

    @Test
    fun f3cDropAccounting() {
        val b = WeftFanoutBroadcasterCompat(64, 4)
        val r = b.createReader()
        fillFrame(b, 1, 16); b.publish()
        r.claim() // lastSeq = 1
        // Frames 2..4 complete unobserved.
        for (s in 2..4) { fillFrame(b, s, 16); b.publish() }
        fillFrame(b, 5, 16); b.publish()
        val c = r.claim()
        assertTrue(c.fresh)
        assertEquals(5L, c.seq)
        assertEquals(3L, c.dropped)
    }

    // ----- F4c: telescoping identity across an interleaved sequence -----

    @Test
    fun f4cTelescopingIdentity() {
        val b = WeftFanoutBroadcasterCompat(64, 4)
        val r = b.createReader()
        var sumDropped = 0L
        var fresh = 0L
        for (s in 1..40) {
            fillFrame(b, s, 16); b.publish()
            if (s % 4 == 0) {
                val c = r.claim()
                if (c.fresh) {
                    fresh++
                    sumDropped += c.dropped
                }
            }
        }
        assertEquals(10L, fresh)
        assertEquals(40L - fresh, r.stats().drops)
        assertEquals(r.stats().drops, sumDropped)
    }

    // ----- F5c: ring overwrite -> claim yields the LATEST frame -----

    @Test
    fun f5cRingOverwriteYieldsLatest() {
        val b = WeftFanoutBroadcasterCompat(64, 2)
        val r = b.createReader()
        for (s in 1..7) { fillFrame(b, s, 16); b.publish() }
        val c = r.claim()
        assertTrue(c.fresh)
        assertEquals(7L, c.seq)
        assertTrue(expectFrame(r.view(), 7, 16))
        // The 7 drops are frames 1..6 completed unseen.
        assertEquals(6L, c.dropped)
    }

    // ----- F6c: graceful skip when the target slot is mid-overwrite -----

    @Test
    fun f6cGracefulSkipMidOverwrite() {
        val b = WeftFanoutBroadcasterCompat(64, 4)
        fillFrame(b, 1, 16); b.publish()
        val r = b.createReader()
        assertEquals(1L, r.claim().seq) // lastSeq = 1
        for (f in 2..4) { fillFrame(b, f, 16); b.publish() }
        // latest = 4 lives in slot 3. Four abandoned begins advance wSeq to
        // 8; the 4th invalidates slot 3 — the very slot latest points at —
        // without any newer publish (the deterministic mid-overwrite window).
        for (i in 0 until 4) b.begin()
        val c = r.claim()
        assertFalse(c.fresh) // graceful skip, not a torn frame
        assertEquals(1L, c.seq) // keeps the last consistent frame
        assertEquals(1L, r.stats().skippedMidOverwrite)
        // The in-flight frame completes -> the next claim resumes cleanly.
        fillFrame(b, 9, 16)
        b.publish()
        val c2 = r.claim()
        assertTrue(c2.fresh)
        assertEquals(9L, c2.seq)
        assertEquals(7L, c2.dropped) // frames 2..8 gap over lastSeq=1
        assertTrue(expectFrame(r.view(), 9, 16))
    }

    // ----- F8c: zero-allocation contract (Law 2) — identity stability -----

    @Test
    fun f8cZeroAllocationIdentity() {
        val b = WeftFanoutBroadcasterCompat(64, 4)
        val r = b.createReader()
        val view1 = r.view()
        val rec1 = r.claim()
        assertTrue(rec1 === r.claim())
        assertTrue(view1 === r.view())
        // The payload region and ctrl are stable identities too.
        val payload1 = b.payload
        val ctrl1 = b.ctrl
        for (s in 1..10) { fillFrame(b, s, 16); b.publish(); r.claim() }
        assertTrue(payload1 === b.payload)
        assertTrue(ctrl1 === b.ctrl)
    }

    // ----- F9c: layout parity — ctrl indices are the wire contract -----

    @Test
    fun f9cLayoutParityRawCtrl() {
        val b = WeftFanoutBroadcasterCompat(64, 4)
        fillFrame(b, 1, 16)
        assertEquals(1L, b.publish())
        // Same indices as the VarHandle ring / TS BigInt64Array / C ctrl:
        // 0 = latestSeq, 1 = publishes, 2+k = slotSeq[k].
        assertEquals(1L, b.ctrl.get(0))
        assertEquals(1L, b.ctrl.get(1))
        assertEquals(1L, b.ctrl.get(FanoutCompatCtrl.SLOTSEQ + 0))
        assertEquals(0L, b.ctrl.get(FanoutCompatCtrl.SLOTSEQ + 1))
    }

    // ----- F10c: multi-thread torture — writer + 3 readers, 100k frames -----

    @Test
    fun f10cMultiThreadTorture() {
        val frames = 100_000
        val words = 64
        val b = WeftFanoutBroadcasterCompat(words * 4, 4)
        val readers = listOf(b.createReader(), b.createReader(), b.createReader())
        val integrityFailures = AtomicInteger(0)
        val startGate = CountDownLatch(1)
        val done: MutableList<Future<*>> = ArrayList()
        val pool: ExecutorService = Executors.newFixedThreadPool(4)
        try {
            done.add(pool.submit {
                startGate.await()
                val src = IntArray(words)
                for (f in 1..frames) {
                    for (w in 0 until words) src[w] = tword(f, w)
                    b.begin()
                    b.fill(src, words)
                    b.publish()
                }
            })
            for (i in readers.indices) {
                val r = readers[i]
                done.add(pool.submit {
                    startGate.await()
                    var localFresh = 0L
                    var localDrops = 0L
                    while (true) {
                        val c = r.claim()
                        if (c.fresh) {
                            localFresh++
                            localDrops += c.dropped
                            if (!expectFrame(r.view(), c.seq.toInt(), words)) {
                                integrityFailures.incrementAndGet()
                            }
                            if (c.seq >= frames) break
                        } else {
                            if (c.seq >= frames) break
                        }
                    }
                    // Publish local sums for the identity check below.
                    freshCounts[i].set(localFresh)
                    dropSums[i].set(localDrops)
                })
            }
            startGate.countDown()
            for (f in done) f.get(120, TimeUnit.SECONDS)

            assertEquals(0, integrityFailures.get())
            for (i in readers.indices) {
                val st = readers[i].stats()
                assertEquals(freshCounts[i].get(), st.fresh)
                assertEquals(dropSums[i].get(), st.drops)
                assertEquals(frames.toLong(), readers[i].claim().seq)
                assertEquals(frames.toLong() - st.fresh, st.drops)
            }
        } finally {
            pool.shutdownNow()
        }
    }

    private val freshCounts = arrayOf(AtomicLong(0), AtomicLong(0), AtomicLong(0))
    private val dropSums = arrayOf(AtomicLong(0), AtomicLong(0), AtomicLong(0))

    // ----- Factory routing: the API<33 matrix entry point -----

    @Test
    fun factoryRoutesBothRegimes() {
        // The gate is a pure function of the API level (testable on any host).
        assertTrue(WeftFanoutFactory.varHandleAvailable(33))
        assertTrue(WeftFanoutFactory.varHandleAvailable(34))
        assertFalse(WeftFanoutFactory.varHandleAvailable(32))
        assertFalse(WeftFanoutFactory.varHandleAvailable(26))
        // Routing returns the right regime per level.
        assertTrue(WeftFanoutFactory.broadcasterFor(34, 64, 4)
            is WeftFanoutBroadcaster)
        assertTrue(WeftFanoutFactory.broadcasterFor(26, 64, 4)
            is WeftFanoutBroadcasterCompat)
        // The compat regime is what every pre-33 device actually gets.
        assertTrue(WeftFanoutFactory.broadcasterFor(21, 64, 4)
            is WeftFanoutBroadcasterCompat)
    }
}
