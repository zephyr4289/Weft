package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * FanoutTest.kt — RFC-0004 fan-out driver-layer conformance suite, Kotlin/JVM.
 *
 * The F-series counterpart of packages/core/test/fanout.test.ts and
 * core/c/fanout_test.c: geometry, the stamp-then-fill writer protocol,
 * per-reader fresh/drop accounting, the graceful-skip tear discipline,
 * zero-allocation identity (Law 2), the canonical four-consumer scenario,
 * and a REAL multi-thread torture (writer thread + 3 reader threads — the
 * JVM can express true parallelism, unlike the single-threaded reference
 * schedule; this battery is the analog of the C torture runner).
 *
 * Payload mixer: weft_mix32-based u32 words (04-LITMUS §0.1 pattern family —
 * the same generator as the C F-series and the xlang fixtures), so payload
 * validation is bit-exact against the shared family.
 *
 * Environment tag for any timing-sensitive numbers: JUnit/JVM (sandbox).
 */
class FanoutTest {

    // --- shared mixer (weft_mix32, 04-LITMUS §0.1 — identical to core/c) ---

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

    private fun fillFrame(b: WeftFanoutBroadcaster, seq: Int, words: Int) {
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

    // ----- F1: geometry validation (constructor contracts + layout parity) -----

    @Test
    fun f1GeometryValidation() {
        assertThrows<IllegalArgumentException> { WeftFanoutBroadcaster(0, 4) }
        assertThrows<IllegalArgumentException> { WeftFanoutBroadcaster(6, 4) } // %4 != 0
        assertThrows<IllegalArgumentException> { WeftFanoutBroadcaster(256, 1) } // < 2 slots
        assertThrows<IllegalArgumentException> { WeftFanoutBroadcaster(256, 0) }
        assertThrows<IllegalArgumentException> { WeftFanoutBroadcaster(256, WEFT_FANOUT_MAX_SLOTS + 1) }
        // ring_bytes formula: 16 + 8M + M*payload_bytes — TS/C parity.
        assertEquals(16 + 8 * 4 + 4 * 256, weftFanoutRingBytes(256, 4))
        assertEquals(0, weftFanoutRingBytes(6, 4)) // bad geometry -> 0
        // Reader attach rejects a geometry mismatch instead of tearing.
        val b = WeftFanoutBroadcaster(256, 4)
        assertThrows<IllegalArgumentException> { WeftFanoutReader(b.ring, 256, 8) }
        assertThrows<IllegalArgumentException> { WeftFanoutReader(b.ring, 512, 4) }
    }

    // ----- F2: roundtrip (null frame -> first publish -> fresh claim) -----

    @Test
    fun f2Roundtrip() {
        val b = WeftFanoutBroadcaster(256, 4)
        val r = b.createReader()
        val c0 = r.claim()
        assertFalse(c0.fresh)
        assertEquals(0L, c0.seq)
        assertEquals(0L, c0.dropped)
        fillFrame(b, 1, 64)
        assertEquals(1L, b.publish())
        val c1 = r.claim()
        assertTrue(c1.fresh)
        assertEquals(1L, c1.seq)
        assertEquals(0L, c1.dropped)
        assertTrue(expectFrame(r.view(), 1, 64))
        // publish() before any begin() is a detectable no-op (TS/C parity).
        val b2 = WeftFanoutBroadcaster(256, 4)
        assertEquals(0L, b2.publish())
    }

    // ----- F3: drop accounting -----

    @Test
    fun f3DropAccounting() {
        val b = WeftFanoutBroadcaster(256, 4)
        fillFrame(b, 1, 64); b.publish()
        val r = b.createReader()
        r.claim() // lastSeq = 1 (the TS/C F3 structure: claim the baseline first)
        for (f in 2..6) {
            fillFrame(b, f, 64); b.publish()
        }
        val c = r.claim() // jumps to frame 6; frames 2..5 dropped
        assertTrue(c.fresh)
        assertEquals(6L, c.seq)
        assertEquals(4L, c.dropped) // frames 2..5 unseen
        assertTrue(expectFrame(r.view(), 6, 64))
    }

    // ----- F4: telescoping identity across an interleaved sequence -----

    @Test
    fun f4TelescopingIdentity() {
        val b = WeftFanoutBroadcaster(256, 4)
        val r = b.createReader()
        var sumDropped = 0L
        var freshClaims = 0L
        for (seq in 1..60) {
            fillFrame(b, seq, 64)
            b.publish()
            if (seq % 3 == 0) {
                val c = r.claim()
                if (c.fresh) { sumDropped += c.dropped; freshClaims++ }
            }
        }
        val c = r.claim()
        if (c.fresh) { sumDropped += c.dropped; freshClaims++ }
        // The exact telescoping identity (RFC 0004): sum(dropped) == lastSeq - freshClaims
        assertEquals(60L - freshClaims, sumDropped)
        assertEquals(60L, c.seq) // lastSeq is 60 (the final claim is fresh)
    }

    // ----- F5: ring overwrite -> claim yields the LATEST frame -----

    @Test
    fun f5RingOverwriteYieldsLatest() {
        val b = WeftFanoutBroadcaster(256, 4)
        val r = b.createReader()
        for (f in 1..7) { // M+3 publishes: slots overwritten twice over
            fillFrame(b, f, 64)
            b.publish()
        }
        val c = r.claim()
        assertTrue(c.fresh)
        assertEquals(7L, c.seq)
        assertTrue(expectFrame(r.view(), 7, 64))
    }

    // ----- F6: graceful-skip tear discipline (deterministic protocol exercise) -----

    @Test
    fun f6GracefulSkip() {
        val b = WeftFanoutBroadcaster(64, 4)
        fillFrame(b, 1, 16); b.publish()
        val r = b.createReader()
        assertEquals(1L, r.claim().seq) // lastSeq = 1
        for (f in 2..4) { fillFrame(b, f, 16); b.publish() }
        // latest = 4 lives in slot 3. M abandoned begins advance wSeq to 8;
        // the 4th invalidates slot 3 — the very slot latest points at.
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

    // ----- F6b: abandoned begins are skip-observable, not silent -----

    @Test
    fun f6bAbandonedBeginsObservable() {
        val b = WeftFanoutBroadcaster(64, 4)
        for (f in 1..4) { fillFrame(b, f, 16); b.publish() }
        for (i in 0 until 4) b.begin() // none ever published
        val r = b.createReader()
        val c = r.claim()
        assertFalse(c.fresh)
        assertEquals(0L, c.seq)
        assertEquals(1L, r.stats().skippedMidOverwrite)
        fillFrame(b, 9, 16)
        b.publish()
        val c2 = r.claim()
        assertTrue(c2.fresh)
        assertEquals(9L, c2.seq)
        // Seqs 5..8 never completed, yet read as 8 drops — abandoned seqs
        // are indistinguishable from missed publishes (declared boundary).
        assertEquals(8L, c2.dropped)
    }

    // ----- F7: canonical four-consumer scenario, deterministic closed form -----

    @Test
    fun f7FourConsumerScenario() {
        val n = 10000
        val b = WeftFanoutBroadcaster(256, 4)
        data class Consumer(val name: String, val divisor: Int, val reader: WeftFanoutReader)
        val consumers = listOf(
            Consumer("flight-recorder", 1, b.createReader()),
            Consumer("primary-canvas", 2, b.createReader()),
            Consumer("minimap", 4, b.createReader()),
            Consumer("network-viz", 8, b.createReader())
        )
        var integrityFailures = 0
        for (t in 1..n) {
            fillFrame(b, t, 64)
            b.publish()
            for (c in consumers) {
                if (t % c.divisor == 0) {
                    val claim = c.reader.claim()
                    if (!claim.fresh) integrityFailures++
                    else {
                        if (claim.dropped != (c.divisor - 1).toLong()) integrityFailures++
                        if (!expectFrame(c.reader.view(), claim.seq.toInt(), 64)) integrityFailures++
                    }
                }
            }
        }
        assertEquals(0, integrityFailures)
        for (c in consumers) {
            val st = c.reader.stats()
            val expectedFresh = (n / c.divisor).toLong()
            assertEquals(expectedFresh, st.reads)
            assertEquals(expectedFresh, st.fresh)
            assertEquals(n.toLong() - expectedFresh, st.drops)
            assertEquals(0L, st.skippedMidOverwrite) // single-threaded schedule
            assertEquals(0L, st.tornExhausted)
            assertEquals(n.toLong(), c.reader.claim().seq)
        }
    }

    // ----- F8: zero-allocation contract (Law 2) — identity-stability -----

    @Test
    fun f8ZeroAllocationIdentity() {
        val b = WeftFanoutBroadcaster(256, 4)
        val r = b.createReader()
        val firstClaim = r.claim()
        val firstView = r.view()
        for (f in 1..1000) {
            fillFrame(b, f, 64)
            b.publish()
            val c = r.claim()
            if (c !== firstClaim) { throw AssertionError("claim record identity changed") }
            if (r.view() !== firstView) { throw AssertionError("view buffer identity changed") }
        }
        // begin() hands out slice views of the SAME ring bytes — no per-call
        // allocation of payload storage (the ring itself is the only heap cost
        // beyond the slice object; the TS port's stance: the cached-view
        // discipline applies to reader copies and claim records).
        val v1 = b.begin()
        val v2 = b.begin()
        assertTrue(v1 !== v2) // fresh slice objects (JVM slices are cheap views)
        assertEquals(256, v1.capacity())
    }

    // ----- F9: layout parity — raw ctrl bytes are the wire contract -----

    @Test
    fun f9LayoutParityRawCtrl() {
        val b = WeftFanoutBroadcaster(64, 4)
        fillFrame(b, 1, 16); b.publish()
        // Read the ctrl region with PLAIN LE views (no VarHandle) — the
        // layout is the cross-port contract (TS BigInt64Array, C _Atomic u64).
        val ctrl = b.ring.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(1L, ctrl.getLong(0))          // latestSeq
        assertEquals(1L, ctrl.getLong(8))          // publishes
        assertEquals(1L, ctrl.getLong(16))         // slotSeq[0]
        assertEquals(0L, ctrl.getLong(16 + 8))     // slotSeq[1]: untouched -> 0 (invalidated)
        // payload word 0 of slot 0 at 16 + 8*4 = byte 48
        assertEquals(tword(1, 0), ctrl.getInt(48))
        // Attach a SECOND reader through the raw-bytes road (JNI/interop
        // story): any ByteBuffer of matching capacity is a valid ring.
        val foreign = ByteBuffer.allocateDirect(weftFanoutRingBytes(64, 4))
            .order(ByteOrder.LITTLE_ENDIAN)
        val raw = ByteArray(b.ring.capacity())
        b.ring.duplicate().position(0).get(raw, 0, raw.size)
        foreign.duplicate().position(0).put(raw, 0, raw.size)
        val r2 = WeftFanoutReader(foreign, 64, 4)
        val c = r2.claim()
        assertTrue(c.fresh)
        assertEquals(1L, c.seq)
        assertTrue(expectFrame(r2.view(), 1, 16))
    }

    // ----- F10: multi-thread torture — writer thread + 3 reader threads -----
    // The JVM's real parallelism makes this the analog of the C torture
    // runner: every fresh claim is word-validated (mixer), zero torn frames
    // may be ACCEPTED (the protocol's job), and the telescoping identity
    // must hold exactly per reader.

    @Test
    fun f10MultiThreadTorture() {
        val frames = 100_000
        val words = 64
        val payloadBytes = words * 4
        val b = WeftFanoutBroadcaster(payloadBytes, 4)
        val readers = listOf(b.createReader(), b.createReader(), b.createReader())
        val integrityFailures = AtomicInteger(0)
        val startGate = CountDownLatch(1)
        val done: MutableList<Future<*>> = ArrayList()
        val pool: ExecutorService = Executors.newFixedThreadPool(4)
        try {
            // Writer: mixer frames 1..frames.
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
            // Readers: tight claim loops; validate every fresh claim's every
            // word; track per-reader telescoping sums.
            val freshCounts = arrayOf(AtomicLong(0), AtomicLong(0), AtomicLong(0))
            val dropSums = arrayOf(AtomicLong(0), AtomicLong(0), AtomicLong(0))
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
                            // A skipped/exhausted tick is legal (counted,
                            // never silent) — but a torn ACCEPTED frame is not.
                        }
                    }
                    freshCounts[i].set(localFresh)
                    dropSums[i].set(localDrops)
                })
            }
            startGate.countDown()
            for (f in done) f.get(120, TimeUnit.SECONDS)

            assertEquals(0, integrityFailures.get())
            // Telescoping identity per reader: sum(dropped) == lastSeq - fresh.
            for (i in readers.indices) {
                val st = readers[i].stats()
                assertEquals(st.fresh, freshCounts[i].get())
                assertEquals(st.drops, dropSums[i].get())
                assertEquals(readers[i].claim().seq, frames.toLong())
                assertEquals(st.drops, frames.toLong() - st.fresh)
                // Convergence: every reader ends on the final frame.
            }
        } finally {
            pool.shutdownNow()
        }
    }

    // ----- helpers -----

    private inline fun <reified T : Throwable> assertThrows(block: () -> Unit) {
        val thrown = runCatching(block).exceptionOrNull()
        if (thrown == null) {
            throw AssertionError("Expected ${T::class.java.simpleName} was not thrown")
        }
        if (thrown !is T) {
            throw AssertionError(
                "Expected ${T::class.java.simpleName} but got " +
                    "${thrown::class.java.simpleName}: $thrown"
            )
        }
    }
}
