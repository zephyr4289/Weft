// Fanout.swift — RFC 0004: Multi-Consumer Fan-Out ring, Swift driver layer
//
// WHY EXISTS: RFC 0004 (Accepted as driver-layer pattern, round-6 §4). The
// Triad kernel is 1-writer/1-reader by design; applications that need a
// primary canvas, a minimap, a flight recorder, and a network visualizer on
// one stream cannot bind N readers to one Triad. The TS port ships the ring
// over a SharedArrayBuffer (core/ts/fanout.ts), and Series 4 brought
// byte-compatible rings to C and Rust (core/c/fanout.{h,c},
// core/rust/src/fanout.rs) — but the Swift port had no equivalent: iOS/
// macOS apps could only fan out through the FFI detour to the C ring. This
// module brings the ring to Swift with the SAME BYTE-COMPATIBLE LAYOUT
// (docs/PORTS.md §6), allocated as ONE 64-byte-aligned region so a C peer
// (or any port's ring bytes handed over raw) attaches with zero copy.
//
// RING LAYOUT (byte-identical to core/ts/fanout.ts and core/c/fanout.h):
//   byte 0              latestSeq   u64  0 = no frame yet; frames from 1
//   byte 8              publishes   u64  telemetry (one add per publish)
//   byte 16 + 8k        slotSeq[k]  u64  0 = INVALIDATED (fill in progress)
//   byte 16 + 8M        payload     M slots x payload_bytes (slot k at +k*payload_bytes)
// payload_bytes MUST be a multiple of 4 (u32 word granularity).
// ring_bytes = 16 + 8M + M*payload_bytes — identical formula in all ports.
//
// PROTOCOL (RFC 0004 §Reference-level specification — same as every port):
//   Writer (single, by contract — the same contract as the kernel's writer):
//     begin():   wSeq += 1; k = (wSeq-1) mod M;
//                slotSeq[k] <- 0  (invalidate BEFORE the fill — the FI1 bracket)
//                return slot payload pointer (plain writes under the bracket
//                discipline, the TS port's stance — or use fill() for the
//                race-free relaxed-atomic word path, the C port's stance)
//     publish(): slotSeq[k] <- wSeq; latestSeq <- wSeq; publishes += 1
//   Reader (N, independent; each owns its pre-allocated copy buffer — Law 2):
//     claim():   L = latestSeq; if L == 0 or L == lastSeq: not fresh
//                else bounded (<= 4 attempts):
//                  k = (L-1) mod M; sB = slotSeq[k]
//                  if sB != L: re-read latestSeq; unchanged -> SKIP this tick
//                    (Law 1: no spin; counted, never silent); changed -> chase
//                  copy slot k -> reader buffer; sA = slotSeq[k]
//                  if sA == L: consistent frame L; dropped = L - lastSeq - 1;
//                    advance lastSeq (FI2: per-slot stamp monotonicity)
//                  else: torn copy; retry on the newest completed frame
//                attempts exhausted: not fresh, counted, never a spin
//
// MEMORY ORDERING — the honest Swift regime (docs/PORTS.md §6):
//   Swift-atomics exposes acquire/release/relaxed/SC per access but NO
//   standalone fence, so the C port's fenced acq/rel regime (SeqCst store +
//   SeqCst fence pairs for P1/P2) cannot be expressed. The Swift port
//   therefore pays SEQUENTIALLY CONSISTENT stamps — the TS port's regime —
//   for every ctrl access (invalidate, re-stamp, latestSeq, revalidation):
//   a SeqCst store is also a Release, a SeqCst load also an Acquire, and on
//   every implementation we target (x86-TSO: stores retire in order; ARM64:
//   swift-atomics' SC accesses emit full dmb-ish barriers) the SC access
//   carries the bracket duty the C port gets from its fence pair. Payload
//   words are RELAXED-ATOMIC u32 accesses on BOTH sides (the C port's
//   stance): race-free in the strict model at zero x86 cost — no plain
//   concurrent payload access anywhere in this file.
//   Divergence note: the port is STRONGER than the TS port (atomic payload
//   words vs plain Float32 stores) and WEAKER-ORDERED-PROOFED differently
//   than the C port (SC stamps instead of fences) — the ordering each
//   language can express is the ordering each pays for; the correctness
//   argument is gated by the F-series + thread-torture battery in CI
//   (Tests/WeftTests/FanoutTests.swift), the same gate structure as C.
//
// LAW 2: begin/fill/publish/claim allocate nothing (init/attach may).
// LAW 1: every path is bounded; a skip or exhausted retry is counted in
//        reader stats, never silent, never a spin.
// LAW 4: honest boundaries — dropped = L - lastSeq - 1 in UInt64 arithmetic
//        assumes the single-writer monotonic contract; multi-canvas renders
//        are approximately synchronized (latest-wins per consumer); the
//        payload contract is u32 words (byte-granular like the C port —
//        float users go through Float(bitPattern:)).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import Foundation
import Atomics

/// Maximum ring depth. RFC 0004 recommends 4-8; the bound matches the C
/// port's WEFT_FANOUT_MAX_SLOTS so geometry validates identically.
public let WEFT_FANOUT_MAX_SLOTS = 64

/// Bounded claim attempts (same constant and rationale as the TS/C ports).
public let WEFT_FANOUT_MAX_CLAIM_ATTEMPTS = 4

/// Control-block u64 indices (byte offset = 8 * index — the layout contract
/// shared with the TS BigInt64Array ctrl and the C _Atomic u64 ctrl).
private let FAN_IDX_LATEST = 0
private let FAN_IDX_PUBLISHES = 1
private let FAN_IDX_SLOTSEQ = 2

/// Total ring size in bytes for the given geometry (the interop contract —
/// identical to core/ts/fanout.ts and core/c/fanout.c). 0 on bad geometry.
public func weftFanoutRingBytes(payloadBytes: Int, slotCount: Int) -> Int {
    guard payloadBytes > 0, payloadBytes % 4 == 0,
          slotCount >= 2, slotCount <= WEFT_FANOUT_MAX_SLOTS else { return 0 }
    return 16 + 8 * slotCount + slotCount * payloadBytes
}

private func fanGeometryOk(_ payloadBytes: Int, _ slotCount: Int) -> Bool {
    payloadBytes > 0 && payloadBytes % 4 == 0 &&
        slotCount >= 2 && slotCount <= WEFT_FANOUT_MAX_SLOTS
}

private func fanPayloadBase(_ slotCount: Int) -> Int { 16 + 8 * slotCount }

/// Claim result record — reader-owned and identity-stable, mutated in place
/// per claim so the hot path allocates nothing (Law 2). Read the fields
/// synchronously after claim(); do not retain the record across claims
/// expecting a snapshot.
public final class FanoutClaim {
    public var fresh: Bool
    public var seq: UInt64
    public var dropped: UInt64
    public init(fresh: Bool = false, seq: UInt64 = 0, dropped: UInt64 = 0) {
        self.fresh = fresh
        self.seq = seq
        self.dropped = dropped
    }
}

/// Advisory reader statistics (AXIOM T: advisory, never a correctness
/// reference). Field names mirror the TS/C/Kotlin ports.
public struct FanoutReaderStats {
    public let reads: UInt64
    public let fresh: UInt64
    public let drops: UInt64
    public let skippedMidOverwrite: UInt64
    public let tornExhausted: UInt64
}

/// Advisory broadcaster state (cold path; AXIOM T applies).
public struct FanoutDebugStats {
    public let latestSeq: UInt64
    public let publishes: UInt64
    public let slotCount: Int
    public let payloadBytes: Int
    public let slotStamps: [UInt64]
}

// ---------------------------------------------------------------------------
// The fan-out ring: one writer + N readers, M pre-allocated slots, one
// publication point (latestSeq). Field discipline mirrors the kernel:
//   - ring/ctrl/wordAtomics: shared; synchronized ONLY by the stamp protocol.
//   - wSeq/wSlot/begun: WRITER-PRIVATE (single writer by contract).
// ---------------------------------------------------------------------------

public final class WeftFanoutBroadcaster {

    /// Per-slot payload capacity in bytes (immutable after init; multiple of 4).
    public let payloadBytes: Int
    /// Ring depth (immutable after init). RFC 0004 recommends 4-8.
    public let slotCount: Int

    /// The single 64-byte-aligned ring allocation. Hand `ringBytes`-many
    /// bytes to any peer (C FFI, another port) and they attach with zero
    /// copy — the bytes are the interop contract, exactly like posting the
    /// SAB in the TS port.
    public private(set) var ring: UnsafeMutableRawPointer

    // ctrl accessors: UnsafeAtomic views over the ring's ctrl region (SC
    // regime — see the ordering note in the header).
    private let ctrl: UnsafeAtomic<UInt64>  // storage FROZEN at ring + 0
    // Per-slot ctrl views are derived on demand from a single bound pointer.
    private let ctrlPtr: UnsafeMutablePointer<UInt64.AtomicRepresentation>
    // Payload words: relaxed-atomic u32 views over the payload region.
    private let payloadPtr: UnsafeMutablePointer<UInt32.AtomicRepresentation>

    // Writer-private (single writer by contract — the kernel's discipline).
    private var wSeq: UInt64 = 0
    private var wSlot: Int = 0
    private var begun: Bool = false

    /// Allocate a fan-out ring (init may allocate — Law 2 applies to
    /// begin/fill/publish/claim). Ctrl is zero-initialized: latestSeq=0 (no
    /// frame yet), publishes=0, every slotSeq=0 (all invalidated) — the same
    /// invariants a fresh SAB gives the TS port.
    public init(payloadBytes: Int, slotCount: Int = 4) {
        precondition(fanGeometryOk(payloadBytes, slotCount),
                     "fanout geometry: payloadBytes must be a positive multiple of 4 and slotCount in [2, \(WEFT_FANOUT_MAX_SLOTS)]")
        self.payloadBytes = payloadBytes
        self.slotCount = slotCount
        let bytes = weftFanoutRingBytes(payloadBytes: payloadBytes, slotCount: slotCount)
        let region = UnsafeMutableRawPointer.allocate(byteCount: bytes, alignment: 64)
        region.initializeMemory(as: UInt8.self, repeating: 0, count: bytes)
        self.ring = region
        // Bind ctrl (u64 atomics) and payload (u32 atomics) sub-regions.
        self.ctrlPtr = region.bindMemory(to: UInt64.AtomicRepresentation.self,
                                         capacity: 2 + slotCount)
        // Zero-init each ctrl storage to a valid atomic 0 (required before
        // any UnsafeAtomic view may touch it).
        for i in 0..<(2 + slotCount) {
            ctrlPtr[i] = UnsafeAtomic<UInt64>.Storage(0)
        }
        let pbase = region.advanced(by: fanPayloadBase(slotCount))
        let words = slotCount * payloadBytes / 4
        self.payloadPtr = pbase.bindMemory(to: UInt32.AtomicRepresentation.self,
                                           capacity: words)
        for w in 0..<words {
            payloadPtr[w] = UnsafeAtomic<UInt32>.Storage(0)
        }
        // The latestSeq accessor view (SC loads/stores at ring + 0).
        self.ctrl = UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_LATEST)
    }

    deinit {
        ring.deallocate()
    }

    /// Begin the next frame: bumps the frame counter, INVALIDATES the target
    /// slot's stamp (SC store — see the ordering note) and returns the slot's
    /// payload pointer. The pointer stays valid until the next begin().
    /// Plain writes through it are visible to readers only after publish()
    /// (the bracket discipline, TS-port stance); use fill() for the
    /// race-free relaxed-atomic word path (C-port stance). Zero allocation.
    public func begin() -> UnsafeMutableRawPointer {
        wSeq += 1
        wSlot = Int((wSeq - 1) % UInt64(slotCount))
        begun = true
        UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_SLOTSEQ + wSlot)
            .store(0, ordering: .sequentiallyConsistent)
        return ring.advanced(by: fanPayloadBase(slotCount) + wSlot * payloadBytes)
    }

    /// Fill the begun slot from src via relaxed-atomic u32 word stores (the
    /// race-free fill path — strict-model-clean, the analog of the C port's
    /// weft_fanout_fill). words must be <= payloadBytes/4. Returns words
    /// written, or -1 on bad args / no begin(). Zero allocation.
    public func fill(_ src: [UInt32], _ words: Int) -> Int {
        if !begun { return -1 }
        if words < 0 || words * 4 > payloadBytes || words > src.count { return -1 }
        let base = wSlot * payloadBytes / 4
        for w in 0..<words {
            UnsafeAtomic<UInt32>(at: payloadPtr + base + w)
                .store(src[w], ordering: .relaxed)
        }
        return words
    }

    /// Publish the begun frame: stamp the slot (SC — also a Release, which
    /// orders the fill below it), flip latestSeq (the publication point),
    /// bump publishes (relaxed telemetry). Returns the published frame seq,
    /// or 0 if no begin() ever ran (a detectable no-op, not an error — same
    /// as the TS/C/Kotlin ports). Zero allocation.
    @discardableResult
    public func publish() -> UInt64 {
        if wSeq == 0 { return 0 }
        UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_SLOTSEQ + wSlot)
            .store(wSeq, ordering: .sequentiallyConsistent)
        ctrl.store(wSeq, ordering: .sequentiallyConsistent)
        UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_PUBLISHES)
            .loadThenWrappingIncrement(by: 1, ordering: .relaxed)
        return wSeq
    }

    /// Create a reader bound to this ring (same process). Reader contexts
    /// elsewhere construct WeftFanoutReader directly from the ring bytes.
    public func createReader() -> WeftFanoutReader {
        WeftFanoutReader(ring: ring,
                         ringBytes: weftFanoutRingBytes(payloadBytes: payloadBytes, slotCount: slotCount),
                         payloadBytes: payloadBytes, slotCount: slotCount)
    }

    /// Advisory state snapshot (cold path — allocates; never call per frame).
    public func debugStats() -> FanoutDebugStats {
        var stamps: [UInt64] = []
        stamps.reserveCapacity(slotCount)
        for k in 0..<slotCount {
            stamps.append(UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_SLOTSEQ + k)
                .load(ordering: .sequentiallyConsistent))
        }
        return FanoutDebugStats(
            latestSeq: ctrl.load(ordering: .sequentiallyConsistent),
            publishes: UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_PUBLISHES)
                .load(ordering: .sequentiallyConsistent),
            slotCount: slotCount,
            payloadBytes: payloadBytes,
            slotStamps: stamps)
    }
}

// ---------------------------------------------------------------------------
// Reader — the consumer side. N per ring, each fully independent.
// ---------------------------------------------------------------------------

public final class WeftFanoutReader {

    public let payloadBytes: Int
    public let slotCount: Int

    private let ring: UnsafeMutableRawPointer
    private let ctrlPtr: UnsafeMutablePointer<UInt64.AtomicRepresentation>
    private let payloadPtr: UnsafeMutablePointer<UInt32.AtomicRepresentation>

    /// The reader's own pre-allocated copy buffer — u32 words
    /// (payloadBytes/4), stable identity for the consumer's lifetime; holds
    /// frame data only after a fresh claim (Law 2).
    private let target: [UInt32]

    /// Last frame seq this reader has held consistent (0 = none yet).
    private var lastSeq: UInt64 = 0

    /// Preallocated, identity-stable claim record (mutated per claim).
    private let rec = FanoutClaim()

    // Reader-private statistics (advisory; exposed via stats()).
    private var nReads: UInt64 = 0
    private var nFresh: UInt64 = 0
    private var nDrops: UInt64 = 0
    private var nSkip: UInt64 = 0
    private var nExhausted: UInt64 = 0

    /// Attach a reader to a fan-out ring (any thread, any port's memory).
    /// Geometry is validated against ringBytes — a mismatched pair fails
    /// fast instead of tearing. The caller keeps the ring alive.
    public init(ring: UnsafeMutableRawPointer, ringBytes: Int,
                payloadBytes: Int, slotCount: Int = 4) {
        precondition(fanGeometryOk(payloadBytes, slotCount),
                     "fanout geometry: payloadBytes must be a positive multiple of 4 and slotCount in [2, \(WEFT_FANOUT_MAX_SLOTS)]")
        precondition(ringBytes == weftFanoutRingBytes(payloadBytes: payloadBytes, slotCount: slotCount),
                     "fanout ring geometry mismatch: expected \(weftFanoutRingBytes(payloadBytes: payloadBytes, slotCount: slotCount)) bytes for \(slotCount) slots x \(payloadBytes) bytes, got \(ringBytes)")
        self.ring = ring
        self.payloadBytes = payloadBytes
        self.slotCount = slotCount
        // bindMemory (not assumingMemoryBound): correct both for the
        // broadcaster's own allocation (same-type rebind is a no-op) and for
        // FOREIGN raw memory (a C-produced ring is unbound from Swift's
        // perspective until bound here).
        self.ctrlPtr = ring.bindMemory(to: UInt64.AtomicRepresentation.self,
                                       capacity: 2 + slotCount)
        let pbase = ring.advanced(by: fanPayloadBase(slotCount))
        self.payloadPtr = pbase.bindMemory(to: UInt32.AtomicRepresentation.self,
                                           capacity: slotCount * payloadBytes / 4)
        self.target = [UInt32](repeating: 0, count: payloadBytes / 4)
    }

    /// Claim the freshest completed frame into this reader's buffer. Never
    /// blocks, never spins unboundedly, never fails: a tick on which no
    /// consistent newer frame is available returns fresh=false and the reader
    /// keeps its last consistent frame. Returns the reader-owned claim record
    /// (identity-stable, mutated in place — zero allocation per claim).
    ///
    /// `dropped` counts frames that completed without this reader ever
    /// observing them (RFC 0004 per-reader drop accounting). The telescoping
    /// identity sum(dropped) == lastSeq - freshClaims holds exactly.
    public func claim() -> FanoutClaim {
        nReads += 1
        var L = UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_LATEST)
            .load(ordering: .sequentiallyConsistent)
        if L == 0 || L == lastSeq {
            rec.fresh = false
            rec.seq = lastSeq
            rec.dropped = 0
            return rec
        }
        for _ in 0..<WEFT_FANOUT_MAX_CLAIM_ATTEMPTS {
            let k = Int((L - 1) % UInt64(slotCount))
            let sB = UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_SLOTSEQ + k)
                .load(ordering: .sequentiallyConsistent)
            if sB != L {
                // Slot mid-overwrite (stamp 0) or already re-stamped by a
                // newer frame. Re-read latestSeq: unchanged -> graceful skip
                // (Law 1); changed -> a newer frame completed, chase it.
                let L2 = UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_LATEST)
                    .load(ordering: .sequentiallyConsistent)
                if L2 == L {
                    nSkip += 1
                    rec.fresh = false
                    rec.seq = lastSeq
                    rec.dropped = 0
                    return rec
                }
                L = L2
                continue
            }
            // Stamp matches frame L: copy (relaxed-atomic words), then
            // re-validate the stamp (SC — carries the P2 duty here; see the
            // header ordering note).
            let base = k * payloadBytes / 4
            for w in 0..<(payloadBytes / 4) {
                target[w] = UnsafeAtomic<UInt32>(at: payloadPtr + base + w)
                    .load(ordering: .relaxed)
            }
            let sA = UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_SLOTSEQ + k)
                .load(ordering: .sequentiallyConsistent)
            if sA == L {
                // Consistent frame L (FI2: unchanged stamp proves no
                // overwrite began during the copy).
                let dropped = L - lastSeq - 1
                nDrops += dropped
                lastSeq = L
                nFresh += 1
                rec.fresh = true
                rec.seq = lastSeq
                rec.dropped = dropped
                return rec
            }
            // Torn copy detected — retry on the newest completed frame.
            L = UnsafeAtomic<UInt64>(at: ctrlPtr + FAN_IDX_LATEST)
                .load(ordering: .sequentiallyConsistent)
        }
        // Bounded retries exhausted: keep the last consistent frame. Counted,
        // never silent, never a spin (Law 1).
        nExhausted += 1
        rec.fresh = false
        rec.seq = lastSeq
        rec.dropped = 0
        return rec
    }

    /// The reader's pre-allocated copy buffer as u32 words (stable identity).
    /// Meaningful after a fresh claim; read it live before the next claim —
    /// the same discipline as the kernel's rLive (A3). Float users:
    /// Float(bitPattern: view()[i]).
    public func view() -> [UInt32] { target }

    /// Advisory statistics snapshot (cold path; AXIOM T: advisory, never a
    /// correctness reference).
    public func stats() -> FanoutReaderStats {
        FanoutReaderStats(reads: nReads, fresh: nFresh, drops: nDrops,
                          skippedMidOverwrite: nSkip, tornExhausted: nExhausted)
    }
}
