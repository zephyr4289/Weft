// GovernedFanoutConsumer.swift — RFC-0009 Series 7: the composed display
// consumer (fan-out reader + FreshnessGovernor + CadencePolicy + two-frame
// history over the Series-7 buffer recyclers), Swift driver layer.
//
// WHY EXISTS: RFC-0009's two open questions both resolved "composed" — the
// governor never touches a Triad or a ring; the reader's per-consumer
// staleness is the input. This class IS that composition, once per display
// tick, so the app does not hand-roll it per screen:
//
//   ticker ──tick──> CONSUMER ──┬─ claim()          (fan-out reader, zero alloc)
//                               ├─ governor.step()  (staleness CLASS: FastPath /
//                               │                    Skip / Snapshot / Reseed —
//                               │                    advisory; the app decides
//                               │                    what a class MEANS)
//                               ├─ policy.step()    (PRESENTATION: present? interp?
//                               │                    alphaQ12 — the raster decision)
//                               └─ raster           (blend(prev, new, alphaQ12)
//                                                    into a pooled slot — LATEST/
//                                                    BURST copy newest directly)
//
// ZERO-GC PER TICK (Law 2): claim mutates the reader's record;
// governor/policy mutate identity-stable records; the two-frame history is
// two preallocated [UInt32]; the raster is a pooled slot from
// WeftBufferRecycler (LIVE for the consumer's lifetime — the
// memory-pressure backstop can never take it mid-blend; dispose() returns
// it to the pool). Swift's proof is the identity discipline (===) — the
// per-port honesty wall; the JVM byte audit is the Kotlin leg's proof.
//
// PACED CONTINUITY (the RFC's construction): on a fresh claim the window
// advances — prevWords := newWords, newWords := reader.view() — and the
// blend at alpha=0 equals prev (the completed previous blend): the raster
// is continuous by construction, never extrapolated past the newest frame
// (saturated alpha holds).
//
// THE GOVERNOR STAYS ADVISORY: tick() returns the policy decision (what
// the display loop needs every frame); the ladder's action is exposed via
// [action] + [actionChanged] for the app's CLASS response. Nothing in
// this class branches on a telemetry counter (AXIOM T).
//
// Pure Foundation (no UIKit/MetalKit — the SwiftUI/MTKView adapters live
// in WeftSwiftUI). STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION
// (apple-packages CI is the compile+test cover).

import Foundation

/// One governed display consumer. Single-threaded by contract (the draw
/// thread — the reader's discipline); the reader is borrowed, never owned.
public final class GovernedFanoutConsumer {
    /// Borrowed fan-out reader; claim() is called once per tick().
    public let reader: WeftFanoutReader
    /// A CadencePolicyKind PROTOCOL value.
    public let policyKind: Int32

    private let governor: FreshnessGovernor
    private let policy: CadencePolicy

    /// Words per frame (payloadBytes / 4).
    public let words: Int

    /// The recycler the raster slot came from (nil = privately owned).
    private let myPool: WeftBufferRecycler?

    /// The raster slot (pooled; LIVE for this consumer's lifetime). Packed
    /// u32 words, little-endian.
    public private(set) var raster: [UInt8]

    // --- two-frame history (preallocated; zero alloc per tick) ---
    private var prevWords: [UInt32]
    private var newWords: [UInt32]

    // --- advisory state (AXIOM T) ---
    /// The ladder's latest action (identity-stable record).
    public var action: GovernorAction { governor.act }
    /// True when this tick's ladder action differs from the last (class
    /// change edge — the app's hook for class responses).
    public private(set) var actionChanged = false
    private var lastActionKind: Int32 = GovernorActionKind.fastPath

    /// The policy's counters (presents, coalescedByDecision, ...).
    public var cadence: CadencePolicy { policy }
    /// The ladder's counters (decidedDrops, reseeds, ...).
    public var staleness: FreshnessGovernor { governor }

    /// The ladder's clock (milliseconds). Injectable for deterministic
    /// tests; wall-clock by default.
    public var clock: () -> Int64 = { Int64(Date().timeIntervalSince1970 * 1000) }

    private var disposed = false

    public init(reader: WeftFanoutReader,
                policyKind: Int32,
                rasterPool: WeftBufferRecycler? = nil,
                governorConfig: GovernorConfig = governorDefaults,
                reassessTicks: Int = 8) {
        self.reader = reader
        self.policyKind = policyKind
        self.governor = FreshnessGovernor(config: governorConfig)
        self.policy = CadencePolicy(config: CadenceConfig(policy: policyKind,
                                                          reassessTicks: reassessTicks))
        let bytes = reader.payloadBytes
        self.words = bytes / 4
        self.prevWords = [UInt32](repeating: 0, count: words)
        self.newWords = [UInt32](repeating: 0, count: words)
        if let pool = rasterPool {
            let slot = pool.acquire()
            precondition(slot.count >= bytes,
                         "raster pool slot too small: \(slot.count) < \(bytes)")
            self.raster = slot
            self.myPool = pool
        } else {
            self.raster = [UInt8](repeating: 0, count: bytes)
            self.myPool = nil
        }
    }

    /// One display tick: claim -> ladder -> policy -> raster. Returns the
    /// presentation decision (identity-stable — read it synchronously).
    /// Zero allocation.
    public func tick() -> PresentDecision {
        let rec = reader.claim() // zero alloc; mutates the reader's record
        // The ladder: staleness CLASS from this reader's drop accounting.
        let behind = rec.fresh ? Int64(clamping: rec.dropped) : 0
        let a = governor.step(framesBehind: behind, nowMs: clock())
        actionChanged = a.kind != lastActionKind
        lastActionKind = a.kind
        // The window: on a fresh claim, prev := new, new := view.
        // In-place element copies (NOT array reassignment — that would
        // share storage and COW-allocate on the next mutation; Law 2).
        if rec.fresh {
            for i in 0..<words { prevWords[i] = newWords[i] }
            let v = reader.view()
            for i in 0..<words { newWords[i] = v[i] }
        }
        // The presentation decision.
        let d = policy.step(latestSeq: Int64(clamping: rec.seq))
        // The raster: blend at alpha (PACED) or copy newest (LATEST/BURST).
        if d.present {
            if d.interp {
                let alpha = d.alphaQ12
                let inv = cadenceAlphaOneQ12 - alpha
                for i in 0..<words {
                    let packed = GovernedFanoutConsumer.blendQ12(
                        prevWords[i], newWords[i], alpha, inv)
                    raster[4 * i] = UInt8(truncatingIfNeeded: packed)
                    raster[4 * i + 1] = UInt8(truncatingIfNeeded: packed >> 8)
                    raster[4 * i + 2] = UInt8(truncatingIfNeeded: packed >> 16)
                    raster[4 * i + 3] = UInt8(truncatingIfNeeded: packed >> 24)
                }
            } else {
                for i in 0..<words {
                    let packed = newWords[i]
                    raster[4 * i] = UInt8(truncatingIfNeeded: packed)
                    raster[4 * i + 1] = UInt8(truncatingIfNeeded: packed >> 8)
                    raster[4 * i + 2] = UInt8(truncatingIfNeeded: packed >> 16)
                    raster[4 * i + 3] = UInt8(truncatingIfNeeded: packed >> 24)
                }
            }
        }
        return d
    }

    /// Stop the consumer. The raster slot returns to its pool (the next
    /// trim can reclaim it — the consumer is done drawing). Idempotent.
    public func dispose() {
        guard !disposed else { return }
        disposed = true
        myPool?.release(raster)
    }

    /// Per-channel u32 blend in Q12 — the raster op (zero alloc).
    @inline(__always)
    private static func blendQ12(_ a: UInt32, _ b: UInt32,
                                 _ alpha: Int32, _ inv: Int32) -> UInt32 {
        func ch(_ x: UInt32, _ y: UInt32) -> UInt32 {
            UInt32(((Int(x & 0xff) * Int(inv)) + (Int(y & 0xff) * Int(alpha))) >> 12)
        }
        let r = ch(a, b)
        let g = ch(a >> 8, b >> 8)
        let bl = ch(a >> 16, b >> 16)
        let al = ch(a >> 24, b >> 24)
        return r | (g << 8) | (bl << 16) | (al << 24)
    }
}
