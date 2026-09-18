// RenderQos.kt — real-time thread QoS + the Kotlin worklet (Series 8;
// the JVM/Android port of core/c/thread_qos.{h,c} + worklet.{h,c}).
//
// WHY EXISTS: the lead's Series-8 mandate — real-time render-thread QoS
// to isolate consumer loops from OS background jitter, and zero-overhead
// worklets. On Android the cadence loop fights garbage collection,
// radio work, and background services for a core; hardening the DRAW
// thread is the cheap half of frame predictability (the kernel is
// already wait-free).
//
// FLAGS-NOT-SILENCE (the C tier's contract, ported): every attempt
// returns what actually happened. On a plain JVM (unit tests, desktop)
// android.os.Process is absent — the reflective lookup fails and the
// JVM-native priority path (Thread.MAX_PRIORITY) is used instead, and
// the flags say exactly that. No path silently no-ops.
//
// WeftWorklet: the governed consumer loop OWNED by the runtime — a
// dedicated, QOS-hardened thread with a semaphore-acked SPSC tick pump;
// zero allocation per tick (Law 2 — the JVM battery's ThreadMXBean
// audit in CI covers the worklet body). Single producer (the display
// ticker) by contract.

package dev.weft

/** QoS result flags (bitmask; the C tier's contract). */
public object QosFlags {
    public const val APPLIED_AFFINITY: Int = 0x1
    public const val APPLIED_SCHED: Int = 0x2
    public const val SCHED_UNPRIVILEGED: Int = 0x4
    public const val AFFINITY_PARTIAL: Int = 0x8
    public const val APPLIED_JVM_PRIORITY: Int = 0x10
}

/**
 * Thread QoS for the calling (draw) thread. `cpuMask` bit i = allow core
 * i (0 = no affinity request — the scheduler's placement is trusted).
 */
public object RenderQos {
    /** Android ComponentCallbacks2-era priority: THREAD_PRIORITY_URGENT_DISPLAY. */
    private const val THREAD_PRIORITY_URGENT_DISPLAY = -8

    /** Big-core preference from cpufreq (0 = abstain — homogeneous or
     *  unreadable topology). Cold path; pure Kotlin, JVM-testable. */
    public fun bigCoreMask(): Long {
        var maxKhz = 0L
        val khz = LongArray(64)
        var n = 0
        for (cpu in 0 until 64) {
            val v = runCatching {
                val p = java.io.File(
                    "/sys/devices/system/cpu/cpu$cpu/cpufreq/cpuinfo_max_freq"
                )
                if (p.exists()) p.readText().trim().toLong() else 0L
            }.getOrDefault(0L)
            if (v > 0) {
                khz[n++] = v
                if (v > maxKhz) maxKhz = v
            }
        }
        if (n == 0 || maxKhz == 0L) return 0L
        val floor = maxKhz - maxKhz / 20
        var mask = 0L
        var kept = 0
        for (i in 0 until n) {
            if (khz[i] >= floor) {
                mask = mask or (1L shl i)
                kept++
            }
        }
        return if (kept == n) 0L else mask
    }

    /**
     * Harden the current thread: JVM/Android priority, then (Android
     * only, API 26+) per-thread affinity through android.system.Os.
     * Returns the [QosFlags] bitmask — what ACTUALLY happened.
     */
    public fun applyRenderQos(cpuMask: Long = 0L, urgentDisplay: Boolean = true): Int {
        var flags = 0
        // Priority: android.os.Process when present (the Android runtime),
        // Thread.MAX_PRIORITY on the plain JVM — both counted, never silent.
        val appliedAndroid = runCatching {
            val process = Class.forName("android.os.Process")
            val method = process.getMethod(
                "setThreadPriority", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType
            )
            val tid = Class.forName("android.os.Process")
                .getMethod("myTid").invoke(null) as Int
            method.invoke(
                null, tid,
                if (urgentDisplay) THREAD_PRIORITY_URGENT_DISPLAY else 0
            )
            true
        }.getOrDefault(false)
        if (appliedAndroid) {
            flags = flags or QosFlags.APPLIED_SCHED
        } else {
            val t = Thread.currentThread()
            val old = t.priority
            t.priority = Thread.MAX_PRIORITY
            if (t.priority != old || old == Thread.MAX_PRIORITY) {
                flags = flags or QosFlags.APPLIED_JVM_PRIORITY
            }
        }
        // Affinity: android.system.Os.sched_setaffinity (API 26+). On the
        // plain JVM the class is absent — the mask request is DECLINED
        // with no flag set (the caller sees no APPLIED_AFFINITY: honest).
        if (cpuMask != 0L) {
            val appliedAffinity = runCatching {
                val os = Class.forName("android.system.Os")
                val cpus = ArrayList<Int>()
                for (i in 0 until 64) if (cpuMask and (1L shl i) != 0L) cpus.add(i)
                os.getMethod(
                    "sched_setaffinity",
                    Int::class.javaPrimitiveType,
                    IntArray::class.java
                ).invoke(null, 0, cpus.toIntArray())
                true
            }.getOrDefault(false)
            if (appliedAffinity) flags = flags or QosFlags.APPLIED_AFFINITY
        }
        return flags
    }
}

/**
 * The Kotlin worklet: a dedicated, QOS-hardened thread pulling ticks
 * through a semaphore-acked SPSC handoff. Zero allocation per tick.
 *
 * @param body one tick — runs on the worklet thread every [post].
 */
public class WeftWorklet(private val body: (tick: Long) -> Unit) {
    private val sem = java.util.concurrent.Semaphore(0, false)
    private val appliedFlags = java.util.concurrent.atomic.AtomicInteger(0)
    private val startedLatch = java.util.concurrent.CountDownLatch(1)
    @Volatile private var stop = false
    private var posted: Long = 0
    private var thread: Thread? = null

    /** Spawn + harden. Returns the [QosFlags] the thread actually
     *  applied (latched before start returns — no race, no guess). */
    public fun start(cpuMask: Long = 0L): Int {
        val t = Thread {
            appliedFlags.set(RenderQos.applyRenderQos(cpuMask))
            startedLatch.countDown()
            while (!stop) {
                sem.acquireUninterruptibly()
                if (stop) break
                executed++ // the tick ordinal (single consumer — ordered)
                body(executed)
            }
        }
        t.name = "weft-worklet"
        t.isDaemon = false
        t.start()
        startedLatch.await()
        thread = t
        return appliedFlags.get()
    }

    /** Request one tick (the display ticker is the single producer). */
    public fun post() {
        posted++
        sem.release()
    }

    /** Ticks executed so far (advisory). */
    @Volatile
    public var executed: Long = 0
        private set

    /** posted - executed right now (the only queue depth). */
    public fun pending(): Long = posted - executed

    /** Stop the loop and join. Idempotent. */
    public fun dispose() {
        stop = true
        sem.release()
        thread?.join()
        thread = null
    }
}
