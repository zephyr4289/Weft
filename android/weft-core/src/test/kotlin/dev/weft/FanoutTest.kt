package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

// FanoutTest.kt — F-series battery for the pure-JVM fan-out ring (Fanout.kt)
// + threaded torture, mirroring packages/core/test/fanout.test.ts and
// core/c/fanout_test.c so all three ports are pinned by the same contract:
//
//   F1  geometry validation (payloadFloats >= 1, slotCount in 2..64)
//   F2  begin/publish/claim roundtrip — fresh, seq, dropped=0, floats intact
//   F3  drop accounting — publish 3 unseen frames, one claim, dropped=2
//   F4  telescoping identity: sum(dropped) == lastSeq - freshClaims, exact
//   F5  graceful skip — mid-overwrite of the LATEST slot, no newer frame
//   F6  publish-without-begin is a detectable no-op (returns 0)
//   F7  multi-reader independence — per-reader telescoping, exact
//   F8  debugStats advisory snapshot
//   FT  threaded torture — 1 writer x 3 readers, per-frame validation,
//       exact telescoping, final convergence
//
// Runs on the host JVM (gradlew test); no Robolectric, no native library —
// the NATIVE ring path is pinned by fixtures/jni-fanout/FanoutJniHarness.

class FanoutTest {

    private fun fill(slot: FloatArray, seq: Int) {
        // seq*128 + i stays below 2^24 (exact-integer f32 territory) for the
        // whole torture range, so any mismatch IS a tear, never rounding.
        for (i in slot.indices) slot[i] = (seq * 128f) + i
    }

    private fun frameSeqOf(view: FloatArray): Int {
        // Every element encodes the same frame: (seq*128 + i). A torn copy
        // mixes frames and fails this invariant.
        val base = view[0]
        for (i in view.indices) assertEquals("frame invariant at i=$i", base, view[i] - i, 0f)
        return (base / 128f).toInt()
    }

    @Test
    fun f1_geometryValidation() {
        try { WeftFanoutBroadcaster(0); assertTrue(false); } catch (e: IllegalArgumentException) {}
        try { WeftFanoutBroadcaster(64, 1); assertTrue(false); } catch (e: IllegalArgumentException) {}
        try { WeftFanoutBroadcaster(64, 65); assertTrue(false); } catch (e: IllegalArgumentException) {}
        WeftFanoutBroadcaster(1, 2) // minimum geometry is legal
        WeftFanoutBroadcaster(64, FANOUT_MAX_SLOTS) // maximum depth is legal
    }

    @Test
    fun f2_roundtrip() {
        val b = WeftFanoutBroadcaster(64, 4)
        val r = b.createReader()
        val slot = b.begin()
        fill(slot, 1)
        assertEquals(1L, b.publish())
        val rec = r.claim()
        assertTrue(rec.fresh)
        assertEquals(1L, rec.seq)
        assertEquals(0L, rec.dropped)
        assertEquals(1, frameSeqOf(r.view()))
        // Claim again with a quiet writer: not fresh, keeps the frame.
        val rec2 = r.claim()
        assertFalse(rec2.fresh)
        assertEquals(1L, rec2.seq)
        assertEquals(1, frameSeqOf(r.view()))
    }

    @Test
    fun f3_dropAccounting() {
        val b = WeftFanoutBroadcaster(32, 4)
        val r = b.createReader()
        for (seq in 1..3) {
            fill(b.begin(), seq)
            b.publish()
        }
        val rec = r.claim()
        assertTrue(rec.fresh)
        assertEquals(3L, rec.seq)
        assertEquals(2L, rec.dropped) // frames 1 and 2 completed unseen
        assertEquals(3, frameSeqOf(r.view()))
    }

    @Test
    fun f4_telescopingIdentity() {
        val b = WeftFanoutBroadcaster(32, 4)
        val r = b.createReader()
        var fresh = 0L
        var sumDropped = 0L
        var lastSeq = 0L
        for (seq in 1..50) {
            fill(b.begin(), seq)
            b.publish()
            if (seq % 5 == 0) {
                val rec = r.claim()
                if (rec.fresh) {
                    fresh++
                    sumDropped += rec.dropped
                    lastSeq = rec.seq
                }
            }
        }
        assertEquals(10L, fresh)
        assertEquals(50L, lastSeq)
        assertEquals("sum(dropped) == lastSeq - freshClaims", lastSeq - fresh, sumDropped)
    }

    @Test
    fun f5_gracefulSkip() {
        val b = WeftFanoutBroadcaster(32, 4)
        val r = b.createReader()
        fill(b.begin(), 1)
        b.publish()
        r.claim() // reader holds frame 1

        // Publish 2..5 (latest = 5, slot (5-1)%4 = 0), no claims.
        for (seq in 2..5) {
            fill(b.begin(), seq)
            b.publish()
        }
        // M begins WITHOUT publishing: the M-th re-opens slot 0 (frame 5's
        // own slot), invalidating its stamp — the mid-overwrite window.
        repeat(4) { b.begin() }
        val rec = r.claim()
        assertFalse("mid-overwrite claim skips the tick", rec.fresh)
        assertEquals("keeps frame 1", 1L, rec.seq)
        assertEquals(1, frameSeqOf(r.view()))
        assertEquals("skip counted, never silent", 1L, r.stats().skippedMidOverwrite)

        // Publish the begun frame (seq 9); the next claim converges.
        assertEquals(9L, b.publish())
        val rec2 = r.claim()
        assertTrue(rec2.fresh)
        assertEquals(9L, rec2.seq)
        assertEquals(7L, rec2.dropped) // frames 2..8 completed unseen
    }

    @Test
    fun f6_publishWithoutBeginIsANoOp() {
        val b = WeftFanoutBroadcaster(32, 4)
        assertEquals(0L, b.publish()) // no begin ever ran — detectable no-op
        fill(b.begin(), 1)
        assertEquals(1L, b.publish())
        assertEquals(1L, b.publish()) // re-stamps the SAME frame (TS parity)
    }

    @Test
    fun f7_multiReaderIndependence() {
        val b = WeftFanoutBroadcaster(32, 4)
        val readers = listOf(b.createReader(), b.createReader(), b.createReader())
        val cadence = intArrayOf(1, 3, 7)
        val fresh = LongArray(3)
        val dropped = LongArray(3)
        for (seq in 1..70) {
            fill(b.begin(), seq)
            b.publish()
            for (i in readers.indices) {
                if (seq % cadence[i] == 0) {
                    val rec = readers[i].claim()
                    if (rec.fresh) {
                        fresh[i]++
                        dropped[i] += rec.dropped
                    }
                }
            }
        }
        for (i in readers.indices) {
            val lastClaimed = (70 / cadence[i]) * cadence[i]
            assertEquals("reader $i fresh claims", (70 / cadence[i]).toLong(), fresh[i])
            assertEquals("reader $i telescoping exact", lastClaimed.toLong() - fresh[i], dropped[i])
            assertEquals("reader $i reads == claim calls", (70 / cadence[i]).toLong(), readers[i].stats().reads)
        }
    }

    @Test
    fun f8_debugStats() {
        val b = WeftFanoutBroadcaster(32, 4)
        for (seq in 1..4) {
            fill(b.begin(), seq)
            b.publish()
        }
        val d = b.debugStats()
        assertEquals(4L, d.latestSeq)
        assertEquals(4L, d.publishes)
        assertEquals(4, d.slotCount)
        assertEquals(32, d.payloadFloats)
        assertTrue("slot stamps populated and in range", d.slotStamps.all { it in 1..4 })
    }

    @Test
    fun ft_threadedTorture() {
        val floats = 64
        val frames = 100_000
        val b = WeftFanoutBroadcaster(floats, 4)
        val readers = listOf(b.createReader(), b.createReader(), b.createReader())
        val violations = java.util.Collections.synchronizedList(mutableListOf<String>())
        val freshCount = Array(3) { AtomicLong(0) }
        val droppedSum = Array(3) { AtomicLong(0) }
        val lastSeen = Array(3) { AtomicLong(0) }
        val done = AtomicInteger(0)

        val threads = readers.mapIndexed { idx, reader ->
            Thread {
                try {
                    val view = reader.view()
                    var fresh = 0L
                    var sumDropped = 0L
                    var lastSeq = 0L
                    var claims = 0L
                    while (lastSeq < frames && claims < 50_000_000L) {
                        val rec = reader.claim()
                        claims++
                        if (rec.fresh) {
                            fresh++
                            sumDropped += rec.dropped
                            val seq = frameSeqOf(view)
                            if (seq.toLong() != rec.seq) {
                                violations.add("reader $idx: torn accept (payload says $seq, stamp says ${rec.seq})")
                            }
                            if (rec.seq <= lastSeq) {
                                violations.add("reader $idx: non-monotonic seq ${rec.seq}")
                            }
                            lastSeq = rec.seq
                        }
                    }
                    if (lastSeq < frames) violations.add("reader $idx: starved before $frames (last=$lastSeq)")
                    if (lastSeq - fresh != sumDropped) {
                        violations.add("reader $idx: telescoping violated (last=$lastSeq fresh=$fresh dropped=$sumDropped)")
                    }
                    freshCount[idx].set(fresh)
                    droppedSum[idx].set(sumDropped)
                    lastSeen[idx].set(lastSeq)
                } finally {
                    done.incrementAndGet()
                }
            }.apply { isDaemon = true; start() }
        }

        for (seq in 1..frames) {
            fill(b.begin(), seq)
            b.publish()
        }
        for (t in threads) t.join(60_000)
        assertEquals("all reader threads terminated", 3, done.get())
        assertTrue("zero protocol violations across $frames frames x 3 readers: $violations", violations.isEmpty())
        for (i in 0 until 3) {
            assertEquals("reader $i converged to the final frame", frames.toLong(), lastSeen[i].get())
            assertTrue("reader $i stats populated", readers[i].stats().reads > 0)
        }
        assertTrue(freshCount.map { it.get() }.sum() > 0)
    }
}
