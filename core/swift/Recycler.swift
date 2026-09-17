// Recycler.swift — RFC-0009 Series 7: 0-GC buffer recycler + memory-pressure
// backstop, Swift driver layer.
//
// WHY EXISTS: the Series-7 drawing loops (the governed consumers, the
// PACED_INTERPOLATE two-frame history, the raster blend) must not allocate
// per frame — Law 2. A PACED consumer retains a PREV-frame snapshot and a
// raster scratch buffer; allocating those per present would hand the GC a
// steady 60-120 Hz garbage stream (exactly the jank source mobile hardening
// exists to kill). This module pools those slots once and reuses them
// forever, with the memory-pressure backstop the work order demanded:
//
//   WeftMemoryPressureCenter — the app-facing hook (the iOS twin of the
//   Kotlin OnLowMemoryListener / Android ComponentCallbacks2 fan-out). The
//   app forwards didReceiveMemoryWarning() (UIApplicationDelegate or
//   UIViewController, or UIApplication.didReceiveMemoryWarningNotification)
//   to WeftMemoryPressureCenter.handleMemoryWarning(), which fans out to
//   every registered recycler. FREE (pooled, idle) slots are released;
//   LIVE slots (checked out, mid-frame) are NEVER touched — the backstop
//   cannot drop a frame because it cannot free the buffer a raster is
//   being blended into. The next acquire() after a trim lazily re-allocates
//   and COUNTS it (reallocs — re-allocation is a decision, visible per
//   AXIOM T; "safely re-allocate without frame drops" is the contract).
//
// HOT PATH (Law 2): acquire()/release() allocate NOTHING on the pooled
//   path — a ContiguousArray of [UInt8] slots (capacity bounded by
//   maxFreeSlots; removeLast/append on a non-empty-capacity array does not
//   reallocate storage). Swift has no portable allocation counter (the
//   per-port honesty wall): the battery pins the identity discipline
//   (a released-then-reacquired slot comes back as the SAME array object
//   via === on the boxed reference) and the deterministic counter
//   equations; instruments-on-device is the deferred road.
//
// TRIM SEMANTICS (deterministic, documented, tested — identical to the
// Kotlin/Dart twins):
//   handleMemoryWarning()      -> drop ALL free slots (keepFree = 0)
//   trim(keepFree)             -> drop free slots down to keepFree
//   LIVE slots are never touched; reallocs counts only allocations caused
//   by an effective trim (a trim that dropped >= 1 slot); steady-state
//   first-fill allocations are not reallocs.
//
// SINGLE CONSUMER THREAD per recycler is the contract (the draw thread);
// the center's listener list is synchronized via a lock (cold path —
// registration and pressure events, never per-frame).
//
// Pure Foundation (no UIKit/MetalKit): compiles standalone for the
// xlang-style batteries. STATUS: SOURCE-ONLY, PENDING REAL-DEVICE
// VERIFICATION (apple-packages CI is the compile+test cover).

import Foundation

/// The app-facing memory-pressure hook (the work order's name, iOS twin).
/// Implement and register with WeftMemoryPressureCenter.
public protocol WeftMemoryPressureListening: AnyObject {
    /// Memory pressure event. `level` is the caller's severity class
    /// (Android ComponentCallbacks2-compatible values when cross-fed;
    /// 80 = COMPLETE on this platform — there is one iOS pressure class).
    func onLowMemory(level: Int)
}

/// Level classes (ComponentCallbacks2-compatible values — the Kotlin twin's
/// TrimLevel; values are API-stable).
public enum TrimLevel {
    public static let runningModerate: Int = 5
    public static let runningLow: Int = 10
    public static let runningCritical: Int = 15
    public static let uiHidden: Int = 20
    public static let background: Int = 40
    public static let moderate: Int = 60
    public static let complete: Int = 80
}

/// A 0-GC pool of same-sized byte slots (the Kotlin WeftBufferRecycler
/// twin; see that file's header for the full contract).
public final class WeftBufferRecycler: WeftMemoryPressureListening {
    /// Slot capacity in bytes (payload-sized for frame snapshots).
    public let slotBytes: Int
    /// Pool ceiling: at most this many released slots are kept free.
    public let maxFreeSlots: Int
    private var free: ContiguousArray<[UInt8]>

    // --- counters (advisory, AXIOM T; the battery pins the exact ones) ---
    public private(set) var acquires: Int = 0
    public private(set) var releases: Int = 0
    /// Allocations that happened because a trim removed slots.
    public private(set) var reallocs: Int = 0
    public private(set) var trims: Int = 0
    public private(set) var trimmedSlots: Int = 0

    /// Slots acquired and not yet released right now.
    public var liveNow: Int { _live }
    /// Slots currently pooled free.
    public var pooledNow: Int { free.count }

    private var _live = 0
    private var poolWasTrimmed = false

    public init(slotBytes: Int, maxFreeSlots: Int = 2) {
        precondition(slotBytes > 0, "slotBytes must be > 0: \(slotBytes)")
        precondition(maxFreeSlots >= 0, "maxFreeSlots must be >= 0: \(maxFreeSlots)")
        self.slotBytes = slotBytes
        self.maxFreeSlots = maxFreeSlots
        self.free = ContiguousArray<[UInt8]>()
        self.free.reserveCapacity(maxFreeSlots)
    }

    /// Take a slot: pooled (zero allocation) or freshly allocated (counted
    /// as a realloc iff a trim previously emptied the pool).
    public func acquire() -> [UInt8] {
        acquires += 1
        _live += 1
        if !free.isEmpty {
            return free.removeLast()
        }
        if poolWasTrimmed { reallocs += 1 }
        return [UInt8](repeating: 0, count: slotBytes)
    }

    /// Return a slot. True if pooled; false if the pool was full (the slot
    /// is dropped — bounded steady-state memory, Law 2's cold side).
    @discardableResult
    public func release(_ slot: [UInt8]) -> Bool {
        releases += 1
        _live -= 1
        if free.count < maxFreeSlots {
            free.append(slot)
            return true
        }
        return false
    }

    /// The memory-pressure backstop: drop FREE slots down to `keepFree`
    /// (default 0 — drop all). LIVE slots are never touched.
    public func trim(keepFree: Int = 0) {
        trims += 1
        var dropped = 0
        while free.count > keepFree {
            free.removeLast()
            dropped += 1
        }
        trimmedSlots += dropped
        if dropped > 0 { poolWasTrimmed = true }
    }

    /// WeftMemoryPressureListening forwarding with the documented level
    /// mapping (identical to the Kotlin twin).
    public func onLowMemory(level: Int) {
        switch level {
        case TrimLevel.complete, TrimLevel.runningCritical:
            trim(0)
        case TrimLevel.moderate, TrimLevel.background, TrimLevel.runningLow:
            trim(free.count / 2)
        default:
            trim(free.count) // uiHidden / runningModerate / unknown: keep all
        }
    }
}

/// The process-wide fan-out point: the app forwards
/// didReceiveMemoryWarning() (UIApplicationDelegate / UIViewController /
/// UIApplication.didReceiveMemoryWarningNotification) to
/// handleMemoryWarning(); N listeners (recyclers, governed consumers) get
/// the COMPLETE-class trim. Cold path only — never called per frame.
public final class WeftMemoryPressureCenter {
    private var listeners: [WeftMemoryPressureListening] = []
    private let lock = NSLock()

    public static let shared = WeftMemoryPressureCenter()

    public func register(_ listener: WeftMemoryPressureListening) {
        lock.lock(); defer { lock.unlock() }
        listeners.append(listener)
    }

    public func unregister(_ listener: WeftMemoryPressureListening) {
        lock.lock(); defer { lock.unlock() }
        listeners.removeAll { $0 === listener }
    }

    public var registered: Int {
        lock.lock(); defer { lock.unlock() }
        return listeners.count
    }

    /// Forward a hard memory warning (didReceiveMemoryWarning).
    public func handleMemoryWarning() {
        onLowMemory(level: TrimLevel.complete)
    }

    /// Forward a classed pressure event (ComponentCallbacks2-compatible).
    public func onLowMemory(level: Int) {
        lock.lock()
        let snapshot = listeners
        lock.unlock()
        for l in snapshot { l.onLowMemory(level: level) }
    }
}
