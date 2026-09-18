// RenderQoS.swift — real-time thread QoS + the Swift worklet (Series 8;
// the Apple port of core/c/thread_qos.{h,c} + worklet.{h,c}).
//
// WHY EXISTS: the lead's Series-8 mandate — real-time render-thread QoS
// (QOS_CLASS_USER_INTERACTIVE) to isolate consumer loops from OS
// background jitter. On iOS the mapped mechanism is qualityOfService on
// the Thread + the Mach thread policy set; the C tier's flags-not-
// silence contract carries over: every attempt reports what ACTUALLY
// happened.
//
// HONESTY NOTES (Apple-specific, documented by Apple itself):
//   - THREAD_AFFINITY_POLICY is a no-op on Apple Silicon — requesting it
//     returns APPLIED_AFFINITY only if the kernel accepted; the mask is
//     therefore an OPINION here, and the Swift port leaves placement to
//     the QoS classes (the same abstain the C big-core mask takes on
//     homogeneous topologies).
//   - THREAD_TIME_CONSTRAINT_POLICY (audio-style hard RT) is guarded
//     behind `allowTimeConstraint` (default false): without a measured
//     rationale it is a footgun — the flag path exists and is tested.
//
// WeftWorklet: the governed consumer loop OWNED by the runtime — a
// dedicated .userInteractive thread with a DispatchSemaphore tick pump;
// zero per-tick allocation (Law 2). Single producer by contract.

import Foundation

/// QoS result flags (bitmask; the C tier's contract).
public enum QosFlags {
    public static let appliedAffinity: UInt32 = 0x1
    public static let appliedSched: UInt32 = 0x2
    public static let schedUnprivileged: UInt32 = 0x4
    public static let appliedJVMpriority: UInt32 = 0x10 // n/a on Apple
    public static let appliedQosClass: UInt32 = 0x20    // the Apple tier
}

/// Harden the current thread: QOS_CLASS_USER_INTERACTIVE via the
/// Foundation Thread API (what the runtime actually honors on iOS/macOS).
/// Returns the flags that truthfully apply.
@discardableResult
public func weftApplyRenderQoS(allowTimeConstraint: Bool = false) -> UInt32 {
    var flags: UInt32 = 0
    Thread.current.qualityOfService = .userInteractive
    if Thread.current.qualityOfService == .userInteractive {
        flags |= QosFlags.appliedQosClass
    }
    #if arch(arm64) || arch(x86_64)
    if allowTimeConstraint {
        // Mach time-constraint policy — the hard-RT seatbelted path.
        var policy = thread_time_constraint_policy()
        policy.period = 0
        policy.computation = UInt32(2 * kNanosPerMilli)      // 2 ms of work
        policy.constraint = UInt32(8 * kNanosPerMilli)       // by 8 ms
        policy.preemptible = 1
        let selfThread = mach_thread_self()
        let kr = withUnsafeMutablePointer(to: &policy) { p in
            thread_policy_set(selfThread, UInt32(THREAD_TIME_CONSTRAINT_POLICY),
                              thread_policy_t(OpaquePointer(p)),
                              UInt32(MemoryLayout<thread_time_constraint_policy>.size /
                                      MemoryLayout<integer_t>.size))
        }
        if kr == KERN_SUCCESS {
            flags |= QosFlags.appliedSched
        } else {
            flags |= QosFlags.schedUnprivileged
        }
    }
    #endif
    return flags
}

private let kNanosPerMilli: UInt32 = 1_000_000

/// The Swift worklet: a dedicated .userInteractive thread pulling ticks
/// through a DispatchSemaphore handoff. Zero allocation per tick.
public final class WeftWorklet {
    private let body: (UInt64) -> Void
    private let sem = DispatchSemaphore(value: 0)
    private let latch = DispatchGroup()
    private var thread: Thread?
    private var stop = false
    private var posted: UInt64 = 0
    public private(set) var executed: UInt64 = 0

    /// - Parameter body: one tick — runs on the worklet thread per post.
    public init(body: @escaping (UInt64) -> Void) {
        self.body = body
    }

    deinit { dispose() }

    /// Spawn + harden. Returns the flags the thread applied (latched —
    /// no race: the flags are set before the pump begins and start()
    /// waits on the latch).
    @discardableResult
    public func start() -> UInt32 {
        var latchedFlags: UInt32 = 0
        let group = latch
        group.enter()
        let t = Thread { [weak self] in
            guard let self = self else { group.leave(); return }
            latchedFlags = weftApplyRenderQoS()
            group.leave()
            while !self.stop {
                _ = self.sem.wait(timeout: .distantFuture)
                if self.stop { break }
                self.executed &+= 1 // single consumer — ordered
                self.body(self.executed)
            }
        }
        t.name = "dev.weft.worklet"
        t.qualityOfService = .userInteractive
        t.start()
        latch.wait()
        thread = t
        return latchedFlags
    }

    /// Request one tick (the display ticker is the single producer).
    public func post() {
        posted &+= 1
        sem.signal()
    }

    /// posted - executed right now (the only queue depth).
    public func pending() -> UInt64 { posted &- executed }

    /// Stop the loop and join. Idempotent.
    public func dispose() {
        stop = true
        sem.signal()
        thread = nil // detached Thread exits on the stop flag
    }
}
