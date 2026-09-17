// GovernorTest.kt — RFC-0009 FreshnessGovernor + §cadence policies
// conformance suite, Kotlin/JVM (Series 7).
//
// The G-series/PC-series counterpart of packages/core/test/governor.test.ts
// + cadence.test.ts, core/c/governor_test.c, core/rust/tests/governor_test.rs,
// Tests/WeftTests/GovernorTests.swift, and governor_test.dart: the ladder,
// the cooldown, the Law-4 counters, the three presentation policies, the
// exact-count steady-cadence regimes, and the zero-allocation contract.
//
// CROSS-LANGUAGE PARITY, LOCALLY (the Series-7 upgrade over "run the xlang
// fixture in CI"): the canonical xorshift32 traces (04-LITMUS §0.2) are
// pinned by FNV-1a 64 hashes computed from the TS reference
// (scripts/gen_trace_refs.mjs — ladder 0x3c33156204c7cfdf over 10,000
// bytes, cadence 0x6f654c298cbcc9f4 over 60,000 bytes). Any arithmetic
// drift in THIS port fails HERE, without needing another toolchain; the
// fixtures (xlang-governor/vm, xlang-cadence) then byte-compare all ports
// in CI as the standing proof.
//
// Environment tag: JVM 21, kotlinc 2.0.21 (sandbox evidence log:
// litmus/evidence/governor-vm/kotlin-jvm.log; android-packages gradle CI
// is the cover).

package dev.weft

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GovernorTest {

    /// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG
    /// (identical step in TS/C/Rust/Kotlin/Swift/Dart).
    private fun xorshift32(x0: Long): Long {
        var x = x0 and 0xffffffffL
        x = x xor ((x shl 13) and 0xffffffffL)
        x = x xor (x ushr 17)
        x = x xor ((x shl 5) and 0xffffffffL)
        return x and 0xffffffffL
    }

    /// FNV-1a 64 over the packed trace bytes (Long arithmetic wraps mod
    /// 2^64 — the same hash every port computes).
    private fun fnv1a64(bytes: ByteArray): Long {
        // 0xcbf29ce484222325 sets the sign bit — a ULong literal carries
        // the exact bit pattern into the signed Long hash state.
        var h = 0xcbf29ce484222325uL.toLong()
        for (b in bytes) {
            h = h xor (b.toLong() and 0xffL)
            h *= 0x100000001b3L
        }
        return h
    }

    // ------------------------------------------------------------------
    // G-series — the ladder
    // ------------------------------------------------------------------

    @Test
    fun g1LadderEveryBehindMapsToTheDocumentedAction() {
        val gov = FreshnessGovernor()
        // now advances past the cooldown every step so Reseed is never
        // suppressed (G1 tests the LADDER, not the rate limit).
        for (behind in 0..64L) {
            val a = gov.step(behind, behind * 1000)
            when {
                behind <= GOVERNOR_DEFAULTS.fastPathBehind -> {
                    assertEquals(GovernorActionKind.FAST_PATH, a.kind)
                    assertEquals(0, a.skipN)
                }
                behind <= GOVERNOR_DEFAULTS.skipBehind -> {
                    assertEquals(GovernorActionKind.SKIP, a.kind)
                    assertEquals((behind - GOVERNOR_DEFAULTS.fastPathBehind).toInt(), a.skipN)
                }
                behind <= GOVERNOR_DEFAULTS.snapshotBehind -> {
                    assertEquals(GovernorActionKind.SNAPSHOT, a.kind)
                    assertEquals(0, a.skipN)
                }
                else -> {
                    assertEquals(GovernorActionKind.RESEED, a.kind)
                    assertEquals(0, a.skipN)
                }
            }
        }
    }

    @Test
    fun g2MonotoneLargerBehindNeverYieldsAFresherClassAction() {
        val gov = FreshnessGovernor()
        var prevClass = -1
        for (behind in 0..64L) {
            val a = gov.step(behind, behind * 1000)
            assertTrue("G2 monotone at behind=$behind", a.kind >= prevClass)
            prevClass = a.kind
        }
    }

    @Test
    fun g3ReseedFlap10kSpikeTraceBoundsAndSpacing() {
        val gov = FreshnessGovernor()
        var state = 0x00c0ffeeL
        var reseeds = 0L
        var lastReseedAt = -1L
        for (i in 0 until 10000) {
            state = xorshift32(state)
            val behind = state % 128
            val a = gov.step(behind, i.toLong())
            if (a.kind == GovernorActionKind.RESEED) {
                reseeds++
                if (lastReseedAt >= 0) {
                    assertTrue(
                        "G3 cooldown spacing at i=$i",
                        i - lastReseedAt >= GOVERNOR_DEFAULTS.reseedCooldownMs
                    )
                }
                lastReseedAt = i.toLong()
            }
        }
        val bound = (10000 + GOVERNOR_DEFAULTS.reseedCooldownMs - 1) / GOVERNOR_DEFAULTS.reseedCooldownMs
        assertTrue("G3 flap bound ($reseeds <= $bound)", reseeds <= bound)
        assertTrue("G3 exercised ($reseeds > 0)", reseeds > 0)
        assertEquals(reseeds, gov.reseeds)
    }

    @Test
    fun g3bSuppressedReseedDegradesToSnapshot() {
        val gov = FreshnessGovernor()
        assertEquals(GovernorActionKind.RESEED, gov.step(64, 1000).kind)
        assertEquals(GovernorActionKind.SNAPSHOT, gov.step(64, 1050).kind)
        assertEquals(GovernorActionKind.RESEED, gov.step(64, 1300).kind)
        assertEquals(2L, gov.reseeds)
    }

    @Test
    fun g4IdentityStableRecordAndCounters() {
        val gov = FreshnessGovernor()
        val a1 = gov.step(0, 0)
        for (i in 0 until 1_000_000) {
            gov.step(i.toLong() % 128, i.toLong())
        }
        val a2 = gov.step(0, 2_000_000)
        assertTrue("G4 identity-stable record", a1 === a2)
        assertEquals(1_000_002L, gov.steps)
    }

    @Test
    fun g4ZeroAllocationJvmAudit() {
        // The JVM proof of G4/PC4: HotSpot's per-thread allocation counter
        // must show a ZERO-byte delta across 100k mixed ladder+policy steps
        // (after warmup). Guarded — the identity checks above pin the
        // contract on every JVM; this audit pins it on HotSpot.
        if (!HotspotAllocAudit.isAvailable) {
            println("G4 alloc audit SKIPPED (no com.sun.management.ThreadMXBean allocation counter)")
            return
        }
        val gov = FreshnessGovernor()
        val polLatest = CadencePolicy(CadencePolicyKind.LATEST_WINS)
        val polPaced = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        val polBurst = CadencePolicy(CadencePolicyKind.BURST_COALESCE)
        var state = 0x00c0ffeeL
        var latest = 0L
        // Warmup: JIT, lazily-linked call sites, and any first-touch costs
        // leave the measurement window.
        for (i in 0 until 20_000) {
            state = xorshift32(state)
            latest += state % 5
            gov.step(state % 128, i.toLong())
            polLatest.step(latest)
            polPaced.step(latest)
            polBurst.step(latest)
        }
        val tid = Thread.currentThread().id
        val before = HotspotAllocAudit.getThreadAllocatedBytes(tid)
        for (i in 0 until 100_000) {
            state = xorshift32(state)
            latest += state % 5
            gov.step(state % 128, i.toLong())
            polLatest.step(latest)
            polPaced.step(latest)
            polBurst.step(latest)
        }
        val after = HotspotAllocAudit.getThreadAllocatedBytes(tid)
        assertEquals("G4/PC4 zero allocation (bytes allocated over 100k steps)", 0L, after - before)
    }

    @Test
    fun law4DecidedDropsDistinctFromRingCounters() {
        val gov = FreshnessGovernor()
        // behind 2 -> Skip(1); behind 4 -> Skip(3): 1+3 = 4 decided drops.
        gov.step(2, 0)
        gov.step(4, 1)
        assertEquals(4L, gov.decidedDrops)
        // A Snapshot and a FastPath add nothing.
        gov.step(8, 2)
        gov.step(0, 3)
        assertEquals(4L, gov.decidedDrops)
    }

    @Test
    fun customLadderAndReset() {
        val cfg = GovernorConfig(
            fastPathBehind = 0, skipBehind = 2, snapshotBehind = 8, reseedCooldownMs = 100
        )
        val gov = FreshnessGovernor(cfg)
        assertEquals(GovernorActionKind.FAST_PATH, gov.step(0, 0).kind)
        assertEquals(GovernorActionKind.SKIP, gov.step(1, 1).kind)
        assertEquals(1, gov.step(1, 1).skipN)
        assertEquals(GovernorActionKind.SNAPSHOT, gov.step(3, 2).kind)
        assertEquals(GovernorActionKind.RESEED, gov.step(9, 3).kind)
        assertEquals(GovernorActionKind.SNAPSHOT, gov.step(9, 50).kind) // suppressed
        gov.reset()
        assertEquals(0L, gov.steps)
        assertEquals(0L, gov.reseeds)
        assertEquals(GovernorActionKind.FAST_PATH, gov.step(0, 0).kind)
    }

    @Test
    fun g5LocalLadderTraceHashPin() {
        // The canonical (behind, nowMs) trace, packed identically to
        // fixtures/xlang-governor: byte = (kind << 6) | min(skipN, 63).
        // Hash pinned from the TS reference — cross-language parity fails
        // HERE on any arithmetic drift (Series-7 local-parity upgrade).
        val gov = FreshnessGovernor()
        val bytes = ByteArray(10_000)
        var state = 0x00c0ffeeL
        for (i in 0 until 10_000) {
            state = xorshift32(state)
            val behind = state % 128
            val a = gov.step(behind, i.toLong())
            bytes[i] = ((a.kind shl 6) or minOf(a.skipN, 63)).toByte()
        }
        assertEquals("G5 local trace hash (TS reference pin)", 0x3c33156204c7cfdfL, fnv1a64(bytes))
    }

    // ------------------------------------------------------------------
    // PC-series — the cadence policies
    // ------------------------------------------------------------------

    @Test
    fun pc1LatestWinsPresentsIffSeqAdvanced() {
        val p = CadencePolicy(CadencePolicyKind.LATEST_WINS)
        val seqs = longArrayOf(0, 1, 1, 4, 4, 4, 5)
        val expectPresent = booleanArrayOf(false, true, false, true, false, false, true)
        val expectCoalesced = longArrayOf(0, 0, 0, 2, 0, 0, 0)
        val presented = mutableListOf<Long>()
        for (i in seqs.indices) {
            val a = p.step(seqs[i])
            assertEquals("present@$i", expectPresent[i], a.present)
            assertEquals("coalesced@$i", expectCoalesced[i], a.coalesced)
            assertFalse(a.interp)
            if (a.present) presented.add(a.presentSeq)
        }
        assertEquals(listOf(1L, 4L, 5L), presented)
        val held = p.step(5)
        assertFalse(held.present)
        assertEquals(5L, held.presentSeq)
    }

    @Test
    fun pc5Paced30On120PresentsEveryTickAfterWarmup() {
        val p = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        var latest = 0L
        var presents = 0L
        for (t in 1..400L) {
            if (t % 4 == 1L) latest += 1 // 30 Hz on a 120 Hz ticker
            val a = p.step(latest)
            assertTrue(a.interp)
            if (a.present) presents++
        }
        // First window period 1 saturates at tick 2 (ticks 3-4 elide);
        // from tick 5 the ladder (0,1024,2048,3072 -> arrival 0) presents
        // EVERY tick: 2 + 396 = 398 of 400.
        assertEquals(398L, presents)
        assertEquals(presents, p.presents)
    }

    @Test
    fun pc5Paced240On120PresentsEveryTickOnePeriodBehind() {
        val p = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        var latest = 0L
        for (t in 1..400L) {
            latest += 2
            val a = p.step(latest)
            assertTrue(a.present)
            assertEquals(0, a.alphaQ12)
            assertEquals(latest, a.presentSeq)
        }
        assertEquals(400L, p.presents)
    }

    @Test
    fun pc5PacedStallSaturatesAndElidesNeverExtrapolates() {
        val p = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        p.step(1)
        p.step(2)
        val a3 = p.step(2)
        assertTrue(a3.present)
        assertEquals(CADENCE_ALPHA_ONE_Q12, a3.alphaQ12)
        val a4 = p.step(2)
        assertFalse(a4.present)
        assertEquals(CADENCE_ALPHA_ONE_Q12, a4.alphaQ12)
        for (i in 0 until 200) p.step(2)
        assertEquals(0L, p.interpFrames) // endpoints only — zero true blends
    }

    @Test
    fun pc6PacedAlphaLadderIsThePeriodLadder() {
        val p = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        val alphas = mutableListOf<Int>()
        for (t in 1..8L) {
            val latest = when (t) {
                1L -> 1L
                5L -> 2L
                else -> if (t <= 4) 1L else 2L
            }
            val a = p.step(latest)
            if (t >= 5) alphas.add(a.alphaQ12)
        }
        assertEquals(listOf(0, 1024, 2048, 3072), alphas)
    }

    @Test
    fun pc5Burst30On120LocksKOnTheContentBeat() {
        val p = CadencePolicy(CadencePolicyKind.BURST_COALESCE)
        var latest = 0L
        val settled = mutableListOf<Int>()
        for (t in 1..400L) {
            if (t % 4 == 1L) latest += 1
            val a = p.step(latest)
            if (t > 200) settled.add(a.k)
        }
        assertTrue("K locked at 4 (last 200 ticks)", settled.all { it == 4 })
    }

    @Test
    fun pc5Burst240On120DegradesToNewestWins() {
        val p = CadencePolicy(CadencePolicyKind.BURST_COALESCE)
        var latest = 0L
        val settled = mutableListOf<Int>()
        var presents = 0L
        for (t in 1..400L) {
            latest += 2
            val a = p.step(latest)
            if (a.present) presents++
            if (t > 200) settled.add(a.k)
        }
        assertTrue(settled.all { it == 1 })
        assertEquals(400L, presents)
        assertEquals(400L, p.coalescedByDecision)
    }

    @Test
    fun pc5BurstFeedPacesOnePresentPerBurst() {
        val p = CadencePolicy(CadencePolicyKind.BURST_COALESCE)
        var latest = 0L
        var presents = 0L
        var coalescedSeen = 0L
        for (t in 1..400L) {
            if (t % 10 == 1L) latest += 24 // 24-frame burst every 10 ticks
            val a = p.step(latest)
            if (a.present) {
                presents++
                coalescedSeen += a.coalesced
            }
        }
        assertTrue("presents per burst in 38..41 (got $presents)", presents in 38L..41L)
        assertEquals(960L - presents, coalescedSeen)
        assertEquals(400L, p.missedPresentTicks + p.elided + presents)
    }

    @Test
    fun pc5BurstStallFreezesCadenceRecoveryImmediate() {
        val p = CadencePolicy(CadencePolicyKind.BURST_COALESCE)
        var latest = 0L
        for (t in 1..200L) {
            latest += 2
            p.step(latest)
        }
        val before = p.presents
        for (t in 1..100L) p.step(latest)
        assertEquals(before, p.presents) // nothing newer -> no presents
        assertEquals(100L, p.missedPresentTicks)
        latest += 5
        val a = p.step(latest)
        assertTrue(a.present)
        assertEquals(4L, a.coalesced)
    }

    @Test
    fun pc2TelescopingIdentitiesOverVolatileTraces() {
        // LATEST_WINS / BURST: sum(coalesced) == lastPresentedSeq - presents
        for (kind in intArrayOf(CadencePolicyKind.LATEST_WINS, CadencePolicyKind.BURST_COALESCE)) {
            val p = CadencePolicy(kind)
            var state = 0x1234abcdL
            var latest = 0L
            for (i in 0 until 10_000) {
                state = xorshift32(state)
                latest += state % 5
                p.step(latest)
            }
            assertEquals("PC2 $kind", p.lastPresentedSeqForTest() - p.presents, p.coalescedByDecision)
        }
        // PACED: sum(coalesced) == newestSeq - arrivalTicks
        run {
            val p = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
            var state = 0xfeedfaceL
            var latest = 0L
            for (i in 0 until 10_000) {
                state = xorshift32(state)
                latest += state % 5
                p.step(latest)
            }
            assertEquals("PC2 PACED", p.newestSeqForTest() - p.arrivalTicks, p.coalescedByDecision)
        }
    }

    @Test
    fun pc4CadenceIdentityStableRecord() {
        val p = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        val first = p.step(1)
        var state = 0xabcdef01L
        var latest = 1L
        for (i in 0 until 10_000) {
            state = xorshift32(state)
            latest += state % 5
            val a = p.step(latest)
            assertTrue(a === first) // SAME object, mutated in place
        }
    }

    @Test
    fun pc6PolicySwitchMidTraceMonotonePresentSeq() {
        val p = CadencePolicy(CadencePolicyKind.LATEST_WINS)
        var latest = 0L
        var lastPresented = 0L
        val kinds = intArrayOf(
            CadencePolicyKind.LATEST_WINS,
            CadencePolicyKind.PACED_INTERPOLATE,
            CadencePolicyKind.BURST_COALESCE,
            CadencePolicyKind.LATEST_WINS
        )
        for (i in 0 until 4000) {
            if (i % 1000 == 0) p.reset(kinds[i / 1000])
            latest += (if (i % 3 == 0) 1L else 0L) + (if (i % 7 == 0) 2L else 0L)
            val a = p.step(latest)
            if (a.present && !a.interp) {
                assertTrue("PC6 monotone at i=$i", a.presentSeq >= lastPresented)
                lastPresented = a.presentSeq
            }
        }
        assertTrue(p.presents > 0)
    }

    @Test
    fun pc3LocalCadenceTraceHashPin() {
        // The canonical arrival trace, packed identically to
        // fixtures/xlang-cadence: per tick, 3 policies in kind order, each
        // 2 bytes: b1 = present<<7 | interp<<6 | alphaQ12>>7,
        // b2 = min(coalesced, 255). Hash pinned from the TS reference.
        val pols = arrayOf(
            CadencePolicy(CadencePolicyKind.LATEST_WINS),
            CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE),
            CadencePolicy(CadencePolicyKind.BURST_COALESCE)
        )
        val bytes = ByteArray(60_000)
        var state = 0x00c0ffeeL
        var latest = 0L
        var out = 0
        for (i in 0 until 10_000) {
            state = xorshift32(state)
            latest += state % 5
            for (pol in pols) {
                val a = pol.step(latest)
                val b1 = ((if (a.present) 1 else 0) shl 7) or
                    ((if (a.interp) 1 else 0) shl 6) or
                    (a.alphaQ12 shr 7)
                bytes[out++] = b1.toByte()
                bytes[out++] = minOf(a.coalesced, 255L).toByte()
            }
        }
        assertEquals("PC3 local trace hash (TS reference pin)", 0x6f654c298cbcc9f4L, fnv1a64(bytes))
    }
}

private object HotspotAllocAudit {
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
