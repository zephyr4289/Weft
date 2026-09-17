// RecyclerTest.kt — RFC-0009 Series 7: 0-GC buffer recycler + memory-pressure
// backstop conformance suite, Kotlin/JVM.
//
// The R-series (Recycler): pool discipline, bounded steady state, the trim
// backstop's LIVE-slot safety, realloc accounting, the level mapping, the
// center fan-out — and the flagship zero-allocation audit over the WHOLE
// Series-7 per-frame drawing path (claim -> rLiveWords into a stable word
// view -> cadence step -> integer blend into a pooled raster slot ->
// pool round-trip): HotSpot's per-thread allocation counter must show a
// ZERO-byte delta across 100k ticks. That is the "eliminate hidden wrapper
// allocations in the drawing loop" deliverable, proven, not asserted.
//
// Runs two ways (the GovernorTest pattern): gradle in android-packages CI,
// and standalone kotlinc + junit (evidence: litmus/evidence/governor-vm/).

package dev.weft

import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RecyclerTest {

    // --- R1: pool reuse + counter discipline ---

    @Test
    fun r1PoolReuseIdentityAndCounters() {
        val pool = WeftBufferRecycler(slotBytes = 256, maxFreeSlots = 2)
        assertEquals(0, pool.pooledNow)
        assertEquals(0, pool.liveNow)

        val a = pool.acquire()
        assertEquals(256, a.size)
        assertEquals(1, pool.liveNow)
        assertEquals(0, pool.pooledNow)
        assertEquals(0, pool.reallocs) // first fill is not a realloc

        assertTrue(pool.release(a))
        assertEquals(0, pool.liveNow)
        assertEquals(1, pool.pooledNow)

        // Reuse: the SAME slot identity comes back (zero allocation).
        val b = pool.acquire()
        assertTrue("pooled slot identity reused", a === b)
        assertEquals(2, pool.acquires.toInt())
        assertEquals(1, pool.releases.toInt())
        assertEquals(0, pool.reallocs)
    }

    // --- R2: bounded steady state (excess releases drop to the GC) ---

    @Test
    fun r2MaxFreeSlotsBoundsSteadyState() {
        val pool = WeftBufferRecycler(slotBytes = 64, maxFreeSlots = 2)
        val x = pool.acquire(); val y = pool.acquire(); val z = pool.acquire()
        assertTrue(pool.release(x))
        assertTrue(pool.release(y))
        assertFalse("pool full — z drops to the GC", pool.release(z))
        assertEquals(2, pool.pooledNow)
        assertEquals(0, pool.liveNow)
    }

    // --- R3: the backstop touches FREE slots only, never LIVE ---

    @Test
    fun r3TrimDropsOnlyFreeSlotsLiveSurvivesAndNextAcquireReallocates() {
        val pool = WeftBufferRecycler(slotBytes = 128, maxFreeSlots = 2)
        val live = pool.acquire()          // LIVE: mid-frame raster buffer
        val free1 = pool.acquire()          // two DISTINCT slots pool up
        val free2 = pool.acquire()
        pool.release(free1)
        pool.release(free2)
        assertEquals(2, pool.pooledNow)

        pool.trim(keepFree = 0)
        assertEquals(0, pool.pooledNow)
        assertEquals(2, pool.trimmedSlots.toInt())
        assertEquals(1, pool.trims.toInt())
        assertEquals("live slot untouched by the backstop", 1, pool.liveNow)

        // The LIVE slot is still perfectly usable (no frame dropped).
        live[0] = 0xAB.toByte()
        assertEquals(0xAB.toByte(), live[0])

        // Next acquire: fresh allocation, COUNTED (pressure's visible cost).
        val fresh = pool.acquire()
        assertNotEquals(free1, fresh)
        assertEquals(1, pool.reallocs.toInt())
    }

    // --- R4: realloc accounting (first fill != realloc; keep-all trims
    //         do not mark the pool as trimmed) ---

    @Test
    fun r4ReallocAccounting() {
        val pool = WeftBufferRecycler(slotBytes = 32, maxFreeSlots = 4)
        // Steady state: 4 acquires (first fill — no trim ever happened).
        repeat(4) { pool.acquire() }
        assertEquals(0, pool.reallocs)
        assertEquals(4, pool.liveNow)

        // UI_HIDDEN-class with an empty free list: NOT an effective trim.
        pool.onLowMemory(TrimLevel.UI_HIDDEN)
        assertEquals(1, pool.trims.toInt())
        assertEquals(0, pool.trimmedSlots.toInt())

        // Release all, then a keep-all trim again — still not effective.
        repeat(4) { pool.release(ByteArray(32)) }
        assertEquals(4, pool.pooledNow)
        pool.onLowMemory(TrimLevel.UI_HIDDEN)
        assertEquals(4, pool.pooledNow) // everything kept
        assertEquals(0, pool.trimmedSlots.toInt())
        assertEquals(0, pool.reallocs)

        // COMPLETE: drops all four; the next four acquires are reallocs.
        pool.onLowMemory(TrimLevel.COMPLETE)
        assertEquals(0, pool.pooledNow)
        assertEquals(4, pool.trimmedSlots.toInt())
        repeat(4) { pool.acquire() }
        assertEquals(4, pool.reallocs.toInt())
    }

    // --- R5: the documented level mapping ---

    @Test
    fun r5LevelMapping() {
        fun keepAfter(level: Int): Int {
            val p = WeftBufferRecycler(slotBytes = 16, maxFreeSlots = 4)
            val slots = Array(4) { p.acquire() } // FOUR distinct live slots
            slots.forEach { p.release(it) }      // then pooled
            assertEquals(4, p.pooledNow)
            p.onLowMemory(level)
            return p.pooledNow
        }
        // COMPLETE / RUNNING_CRITICAL -> keep 0
        assertEquals(0, keepAfter(TrimLevel.COMPLETE))
        assertEquals(0, keepAfter(TrimLevel.RUNNING_CRITICAL))
        // MODERATE / BACKGROUND / RUNNING_LOW -> keep half (4 -> 2)
        assertEquals(2, keepAfter(TrimLevel.MODERATE))
        assertEquals(2, keepAfter(TrimLevel.BACKGROUND))
        assertEquals(2, keepAfter(TrimLevel.RUNNING_LOW))
        // UI_HIDDEN / RUNNING_MODERATE / unknown(0) -> keep all
        assertEquals(4, keepAfter(TrimLevel.UI_HIDDEN))
        assertEquals(4, keepAfter(TrimLevel.RUNNING_MODERATE))
        assertEquals(4, keepAfter(0))
    }

    // --- R6: the center fan-out ---

    @Test
    fun r6CenterFanOutAndUnregister() {
        val a = WeftBufferRecycler(slotBytes = 8, maxFreeSlots = 2)
        val b = WeftBufferRecycler(slotBytes = 8, maxFreeSlots = 2)
        a.release(a.acquire()); b.release(b.acquire())
        val n0 = WeftRecyclerCenter.registered
        WeftRecyclerCenter.register(a)
        WeftRecyclerCenter.register(b)
        assertEquals(n0 + 2, WeftRecyclerCenter.registered)

        WeftRecyclerCenter.onLowMemory() // hard event == COMPLETE class
        assertEquals(0, a.pooledNow)
        assertEquals(0, b.pooledNow)
        assertEquals(1, a.trims.toInt())
        assertEquals(1, b.trims.toInt())

        WeftRecyclerCenter.unregister(a)
        WeftRecyclerCenter.unregister(b)
        assertEquals(n0, WeftRecyclerCenter.registered)
    }

    // --- R7: rLiveWords — the zero-allocation payload read (Law 2) ---

    @Test
    fun r7RLiveWordsBulkReadMatchesSlice() {
        val weft = Weft(payloadMax = 256)
        // Publish a deterministic payload: word i = (i * golden) as u32.
        val wb = weft.wBegin()
        for (i in 0 until 64) {
            wb.putInt(4 * i, (i * 2654435761L).toInt())
        }
        weft.publish(seq = 7, payloadLen = 256)
        weft.claim()

        val dst = IntArray(64)
        val n = weft.rLiveWords(dst)
        assertEquals(64, n)
        for (i in 0 until 64) {
            assertEquals("word $i", (i * 2654435761L).toInt(), dst[i])
        }
        // Offset + clamp semantics: only maxWords - offsetWords remain.
        val part = IntArray(10)
        assertEquals(4, weft.rLiveWords(part, offsetWords = 60)) // 64 - 60
        assertEquals((60 * 2654435761L).toInt(), part[0])
        assertEquals((63 * 2654435761L).toInt(), part[3])
        assertEquals(10, weft.rLiveWords(part, offsetWords = 50)) // full dst
        assertEquals((59 * 2654435761L).toInt(), part[9])
    }

    // --- R8: the flagship — the WHOLE Series-7 per-frame drawing path is
    //         allocation-free on the JVM (100k ticks, zero bytes) ---

    @Test
    fun r8DrawingLoopZeroAllocationJvmAudit() {
        if (!HotspotAllocAudit.isAvailable) {
            println("R8 alloc audit SKIPPED (no com.sun.management.ThreadMXBean allocation counter)")
            return
        }
        // The governed-consumer per-tick path, end to end:
        //   publish -> claim -> rLiveWords into the stable word view ->
        //   cadence step -> integer blend into the pooled raster slot ->
        //   pool round-trip (release + reacquire on the pooled path).
        val words = 256
        val weft = Weft(payloadMax = words * 4)
        val pool = WeftBufferRecycler(slotBytes = words * 4, maxFreeSlots = 2)
        val policy = CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE)
        var rasterSlot = pool.acquire() // blend output slot (churns the pool)
        val prevWords = IntArray(words)
        val newWords = IntArray(words)

        fun publish(seq: Int) {
            val wb = weft.wBegin()
            for (i in 0 until words) {
                wb.putInt(4 * i, (seq * 31 + i * 2654435761L).toInt())
            }
            weft.publish(seq = seq, payloadLen = words * 4)
        }

        // The consumer tick — the DRAWING LOOP, and ONLY the drawing loop:
        // claim -> rSeq high-water clamp -> (on fresher) rLiveWords into
        // the word view -> cadence step -> blend -> pool round-trip.
        // (The producer's wBegin slice wrappers are writer-thread costs,
        // declared out of the audit's scope — the draw loop never calls
        // the writer path. The high-water clamp is the honest consumer
        // pattern: the triad claim is an EXCHANGE, so a claim with no
        // intervening publish re-holds the previously-parked buffer —
        // rSeq() is the truth, word values alone are not.)
        var lastObs = 0L
        fun consume(): Boolean {
            weft.claim()
            val s = weft.rSeq().toLong() and 0xffffffffL
            var arrived = false
            if (s > lastObs) { // fresher frame held — read it (zero alloc)
                lastObs = s
                weft.rLiveWords(newWords)
                arrived = true
            }
            val d = policy.step(lastObs)
            var presented = false
            if (d.present && d.interp) {
                // Integer blend prev -> new at alphaQ12, into the raster
                // slot's byte storage (packed u32, little-endian).
                val alpha = d.alphaQ12
                val inv = 4096 - alpha
                val r = rasterSlot
                for (i in 0 until words) {
                    val packed = blendQ12(prevWords[i], newWords[i], alpha, inv)
                    r[4 * i] = packed.toByte()
                    r[4 * i + 1] = (packed ushr 8).toByte()
                    r[4 * i + 2] = (packed ushr 16).toByte()
                    r[4 * i + 3] = (packed ushr 24).toByte()
                }
                presented = true
            }
            if (arrived) {
                // The previous NEW becomes PREV (preallocated memcpy).
                System.arraycopy(newWords, 0, prevWords, 0, words)
            }
            // The raster slot round-trips the pool's ZERO-ALLOC path.
            pool.release(rasterSlot)
            rasterSlot = pool.acquire()
            return presented
        }

        // THE AUDIT IS PER-THREAD: the consumer (drawing loop) runs on its
        // own thread; the producer's wBegin slice wrappers are writer-thread
        // costs and cannot pollute the counter. Handoff is spin-based
        // (AtomicInteger flags — zero allocation both sides).
        //
        // ZERO-WINDOW RE-MEASUREMENT (documented, bounded): HotSpot's
        // tiered compilation can land a tiny asynchronous allocation
        // (observed: 136 bytes once in ~20 runs) inside ANY window — JIT
        // noise, not a code allocation. The contract is STEADY-STATE zero:
        // the audit re-measures up to 3 windows and requires at least ONE
        // exactly-zero window. A real per-tick leak (the bridge bug this
        // audit caught: 4.6 KB/claim = 458 MB per window) can NEVER
        // produce a zero window — the gate stays exact, not tolerant.
        val seqFlag = AtomicInteger(0)
        val doneFlag = AtomicInteger(0)
        val warmup = 20_000
        val measured = 100_000
        val windows = 3
        val presentsOut = AtomicInteger(0)
        val deltasOut = java.util.concurrent.atomic.AtomicLongArray(windows)
        val consumer = Thread {
            val tid = Thread.currentThread().getId()
            var p = 0
            var w = 0
            var before = 0L
            var i = 0
            val total = warmup + measured * windows
            while (i < total) {
                i++
                while (seqFlag.get() < i) { /* spin */ }
                if (i == 1) {
                    // Warm the per-thread allocation counter's first call
                    // ON THIS THREAD (its own first-use allocation must
                    // land outside any window).
                    HotspotAllocAudit.getThreadAllocatedBytes(tid)
                }
                if (i == warmup + measured * w + 1) {
                    before = HotspotAllocAudit.getThreadAllocatedBytes(tid) // window opens
                }
                if (consume()) p++
                if (i == warmup + measured * (w + 1)) {
                    deltasOut.set(w, HotspotAllocAudit.getThreadAllocatedBytes(tid) - before)
                    w++
                }
                doneFlag.set(i)
            }
            presentsOut.set(p)
        }
        consumer.start()
        var i = 0
        val total = warmup + measured * windows
        while (i < total) {
            i++
            // 30 Hz feed on a 120 Hz ticker (every 4th tick, starting at
            // tick 1 so the consumer NEVER sees the null frame's pattern
            // word0 — a regressed first observation would poison the PACED
            // window): the alpha ladder advances between arrivals.
            if (i % 4 == 1) publish(i)
            seqFlag.set(i)
            while (doneFlag.get() < i) { /* spin */ }
        }
        consumer.join()
        val presents = presentsOut.get()
        var zeroWindow = false
        for (w in 0 until windows) {
            if (deltasOut.get(w) == 0L) zeroWindow = true
        }
        assertTrue(
            "R8: drawing loop allocated in every window (deltas=" +
                (0 until windows).joinToString(",") { deltasOut.get(it).toString() } + ")",
            zeroWindow
        )
        assertTrue("R8: PACED exercised (presents=$presents)", presents > 10_000)
        assertTrue("R8: synthesis exercised (interp=${policy.interpFrames})", policy.interpFrames > 0)
        assertEquals("R8: pool never reallocated on the pooled path", 0L, pool.reallocs)
    }

    /** Per-channel u32 blend in Q12 — the drawing-loop raster op. A plain
     *  static method (no captures, no lambdas): nothing to allocate. */
    private fun blendQ12(a: Int, b: Int, alpha: Int, inv: Int): Int {
        val r = ((a and 0xff) * inv + (b and 0xff) * alpha) ushr 12
        val g = ((a ushr 8 and 0xff) * inv + (b ushr 8 and 0xff) * alpha) ushr 12
        val bl = ((a ushr 16 and 0xff) * inv + (b ushr 16 and 0xff) * alpha) ushr 12
        val al = ((a ushr 24 and 0xff) * inv + (b ushr 24 and 0xff) * alpha) ushr 12
        return r or (g shl 8) or (bl shl 16) or (al shl 24)
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
