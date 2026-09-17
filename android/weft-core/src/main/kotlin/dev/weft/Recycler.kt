// Recycler.kt — RFC-0009 Series 7: 0-GC buffer recycler + memory-pressure
// backstop, Kotlin driver layer.
//
// WHY EXISTS: the Series-7 drawing loops (the governed consumers, the
// PACED_INTERPOLATE two-frame history, the raster blend) must not allocate
// per frame — Law 2. A PACED consumer retains a PREV-frame snapshot and a
// raster scratch buffer; allocating those per present would hand the GC a
// steady 60-120 Hz garbage stream (exactly the jank source mobile hardening
// exists to kill). This module pools those slots once and reuses them
// forever, with the memory-pressure backstop the work order demanded:
//
//   OnLowMemoryListener — the lead's name for the app-facing hook. The app
//   registers ONE ComponentCallbacks2 (Android) and forwards
//   onTrimMemory(level)/onLowMemory() to WeftRecyclerCenter, which fans out
//   to every registered recycler. FREE (pooled, idle) slots are released;
//   LIVE slots (checked out, mid-frame) are NEVER touched — the backstop
//   cannot drop a frame because it cannot free the buffer a raster is
//   being blended into. The next acquire() after a trim lazily re-allocates
//   and COUNTS it (reallocs — re-allocation is a decision, visible per
//   AXIOM T; "safely re-allocate without frame drops" is the contract).
//
// HOT PATH (Law 2): acquire()/release() allocate NOTHING on the pooled
// path — a fixed-capacity ArrayDeque of ByteArray references, no boxing
// (ByteArray is a reference type), no wrapper objects. The JVM battery
// audits allocated bytes across a 100k acquire/release churn and asserts a
// ZERO delta (HotSpot ThreadMXBean, guarded — the identity/stability checks
// pin the contract everywhere else).
//
// TRIM SEMANTICS (deterministic, documented, tested):
//   onLowMemory()               -> drop ALL free slots (keepFree = 0)
//   onTrimMemory(level)         -> keepFree by level class:
//     COMPLETE(80) / RUNNING_CRITICAL(15)                    -> 0
//     MODERATE(60) / BACKGROUND(40) / RUNNING_LOW(10)        -> half
//     UI_HIDDEN(20) / RUNNING_MODERATE(5) / unknown          -> all
//   (UI_HIDDEN keeps everything: the UI is hidden, nothing is drawing, the
//   pool costs nothing until the surface returns — and then it must be
//   warm. "Half" = floor(pooled / 2), newest-first dropped.)
//
// REALLOC ACCOUNTING: `reallocs` counts allocations that happen because a
// trim actually REMOVED slots (a trim that kept everything — UI_HIDDEN —
// never marks the pool trimmed; the first-fill allocations of steady state
// are not reallocs; the cost of pressure is what the counter is for).
//
// SINGLE CONSUMER THREAD per recycler is the contract (the draw thread);
// the center's listener list is synchronized (cold path — registration and
// pressure events, never per-frame).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (JVM battery green;
// android-packages gradle CI is the cover).

package dev.weft

/** The app-facing memory-pressure hook (the work order's name).
 *  Implement and register with [WeftRecyclerCenter]; forward Android's
 *  ComponentCallbacks2.onTrimMemory/onLowMemory (or call from any
 *  memory-pressure signal source). */
fun interface OnLowMemoryListener {
    /** Memory pressure event. [level] is the caller's severity class
     *  (Android ComponentCallbacks2 constants when forwarded; 0 when
     *  unknown — treated as UI_HIDDEN-class, the conservative keep). */
    fun onLowMemory(level: Int)
}

/** Level classes (Android ComponentCallbacks2 values — mirrored here so
 *  the pure-Kotlin module needs no android import; values are API-stable). */
public object TrimLevel {
    public const val RUNNING_MODERATE: Int = 5
    public const val RUNNING_LOW: Int = 10
    public const val RUNNING_CRITICAL: Int = 15
    public const val UI_HIDDEN: Int = 20
    public const val BACKGROUND: Int = 40
    public const val MODERATE: Int = 60
    public const val COMPLETE: Int = 80
}

/**
 * A 0-GC pool of same-sized byte slots. acquire() returns a pooled slot or
 * (lazily, post-trim) a fresh allocation; release() returns it to the pool
 * bounded by [maxFreeSlots] — excess releases are dropped to the GC (a
 * burst consumer never grows the pool beyond its steady-state working set).
 */
public class WeftBufferRecycler(
    /** Slot capacity in bytes (payload-sized for frame snapshots). */
    public val slotBytes: Int,
    /** Pool ceiling: at most this many released slots are kept free. */
    public val maxFreeSlots: Int = 2,
) : OnLowMemoryListener {
    init {
        require(slotBytes > 0) { "slotBytes must be > 0: $slotBytes" }
        require(maxFreeSlots >= 0) { "maxFreeSlots must be >= 0: $maxFreeSlots" }
    }

    private val free = ArrayDeque<ByteArray>(maxFreeSlots)

    // --- counters (advisory, AXIOM T; the battery pins the exact ones) ---
    /** Total acquires (pooled + allocated). */
    public var acquires: Long = 0
        private set
    /** Total releases (pooled + dropped-to-GC). */
    public var releases: Long = 0
        private set
    /** Allocations that happened because a trim removed slots — the
     *  backstop's visible cost (zero until the first effective trim). */
    public var reallocs: Long = 0
        private set
    /** trim()/onLowMemory() invocations. */
    public var trims: Long = 0
        private set
    /** Slots dropped by trims (free slots only — live slots never). */
    public var trimmedSlots: Long = 0
        private set

    /** Slots acquired and not yet released right now. */
    public val liveNow: Int
        get() = _live
    /** Slots currently pooled free. */
    public val pooledNow: Int
        get() = free.size

    private var _live = 0
    private var poolWasTrimmed = false

    /** Take a slot: pooled (zero allocation) or freshly allocated (counted
     *  as a realloc iff a trim previously emptied the pool — steady-state
     *  first-fill allocations are not reallocs; pressure's cost is). */
    public fun acquire(): ByteArray {
        acquires++
        _live++
        if (free.isNotEmpty()) {
            return free.removeLast()
        }
        if (poolWasTrimmed) reallocs++
        return ByteArray(slotBytes)
    }

    /** Return a slot. True if pooled; false if the pool was full (the slot
     *  is dropped to the GC — bounded steady-state memory, Law 2's cold
     *  side). */
    public fun release(slot: ByteArray): Boolean {
        releases++
        _live--
        if (free.size < maxFreeSlots) {
            free.addLast(slot)
            return true
        }
        return false
    }

    /** The memory-pressure backstop: drop FREE slots down to [keepFree]
     *  (default 0 — drop all). LIVE slots are never touched: the backstop
     *  cannot drop a frame because it cannot free the buffer a raster is
     *  being blended into; the next acquire lazily reallocates (counted). */
    public fun trim(keepFree: Int = 0) {
        trims++
        var dropped = 0
        while (free.size > keepFree) {
            free.removeLast()
            dropped++
        }
        trimmedSlots += dropped
        if (dropped > 0) poolWasTrimmed = true
    }

    /** [OnLowMemoryListener] forwarding with the documented level mapping. */
    override fun onLowMemory(level: Int) {
        when (level) {
            TrimLevel.COMPLETE, TrimLevel.RUNNING_CRITICAL -> trim(0)
            TrimLevel.MODERATE, TrimLevel.BACKGROUND, TrimLevel.RUNNING_LOW ->
                trim(free.size / 2)
            else -> trim(free.size) // UI_HIDDEN / RUNNING_MODERATE / unknown: keep all
        }
    }
}

/**
 * The process-wide fan-out point: one registration per app (forwarded from
 * Android's ComponentCallbacks2, or any pressure signal), N listeners
 * (recyclers, governed consumers). Cold path only — never called per frame.
 */
public object WeftRecyclerCenter {
    private val listeners = java.util.Collections.synchronizedList(
        ArrayList<OnLowMemoryListener>()
    )

    public fun register(listener: OnLowMemoryListener) { listeners.add(listener) }
    public fun unregister(listener: OnLowMemoryListener) { listeners.remove(listener) }
    public val registered: Int
        get() = synchronized(listeners) { listeners.size }

    /** Forward a trim event (ComponentCallbacks2.onTrimMemory(level)). */
    public fun onTrimMemory(level: Int) {
        synchronized(listeners) { listeners.toList() }.forEach { it.onLowMemory(level) }
    }

    /** Forward a hard low-memory event (ComponentCallbacks2.onLowMemory()). */
    public fun onLowMemory() {
        onTrimMemory(TrimLevel.COMPLETE)
    }
}
