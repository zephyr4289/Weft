// WeftReplay.swift — RFC 0019: deterministic time-travel replay fold, Swift
// driver layer.
//
// WHY EXISTS: RFC-0019's fold (a pure state machine over a .weftrec v4
// event stream that reconstructs the full shadow kernel state per event and
// reduces it to a 64-bit FNV-1a hash) shipped as the C reference
// (core/c/weft_replay.{h,c}) with TS/Kotlin twins — this file is the Swift
// half, operation-identical to the C: the transition table and the 111-byte
// canonical serialization are NORMATIVE (RFC-0019) — cross-runtime hash
// logs are byte-compared by fixtures/xlang-replay/run.sh. Read the RFC's
// "modeling contract" before changing ANY line here: the three declared
// abstractions (null-frame len 0, PUBLISH-implies-unrevoked, claim
// validation) are what make the fold deterministic without payload bytes.
//
// ARITHMETIC PARITY: Swift's fixed-width unsigned types make the C's u32/
// u64 ops literal — UInt32/UInt64 arithmetic with &+ / &- / &* (wrapping,
// the honest unsigned ops), plain == and assignment on bit patterns. The
// speculative copy is REAL (the C's `weft_replay_state_t next = *s`): the
// struct is a value type, so `var next = self` copies and `self = next`
// assigns back ONLY on success — a DISAGREE leaves the fold at the last
// good step (rule 3: never silent).
//
// PINNED PARITY VECTORS (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (the RFC-0019 fixture grammar,
//                                     seed 0x00C0FFEE)
//
// Pure logic — no imports (Swift stdlib only): the module compiles
// standalone with plain `swiftc` for the xlang emitters.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (not compilable in
// the x86_64 Linux sandbox — no Swift toolchain; the arithmetic was
// validated op-for-op against the C reference before translation, and the
// fixture gate runs when apple-packages CI brings the toolchain).

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/// Fold verdicts — PROTOCOL (do not renumber).
public enum ReplayResult: Int {
    case ok = 0        // step applied, hash updated
    case disagree = -1 // trace/model disagreement (claim seq mismatch)
    case badKind = -2  // unknown event kind in the stream
}

/// RFC-0014 event kinds (u16) — PROTOCOL (the fold consumes them; do not
/// renumber).
public enum WeftTraceKind {
    public static let publish: UInt16 = 1     // aux = payload_len, data = seq
    public static let claim: UInt16 = 2       // data = seq claimed
    public static let drop: UInt16 = 3        // aux = epoch at ACK, data = seq refused
    public static let revoke: UInt16 = 4      // data = pre-revoke epoch
    public static let ack: UInt16 = 5         // data = epoch after ACK
    public static let stall: UInt16 = 6       // data = attempts (consumer skip)
    public static let tear: UInt16 = 7        // data = seq of torn frame
    public static let canaryFail: UInt16 = 8  // data = seq of bad frame
}

/// Checkpoint interval (jump cost bound) — RFC-0019.
public let weftReplayCheckpoint: UInt32 = 64

// ---------------------------------------------------------------------------
// Shadow kernel state (RFC-0019 fold state)
// ---------------------------------------------------------------------------

/// One ring slot of the shadow state (seq/len/ver — the fold carries no
/// payload bytes, only the slot bookkeeping).
public struct ReplayBuf {
    public var seq: UInt32
    public var len: UInt32
    public var ver: UInt16
    public init(seq: UInt32 = 0, len: UInt32 = 0, ver: UInt16 = 1) {
        self.seq = seq
        self.len = len
        self.ver = ver
    }
}

/// The RFC-0019 fold state. init() plants the RFC-0019 initial state
/// INCLUDING the pinned step-0 hash (the C's weft_replay_init contract);
/// weftReplayInit(_:) resets an existing value identically.
public struct WeftReplayState {
    public var latest: UInt32 = 0
    public var epoch: UInt32 = 0
    public var wWork: UInt32 = 0
    public var rWork: UInt32 = 0
    public var revoked: UInt8 = 0
    public var buf: [ReplayBuf] = [ReplayBuf(), ReplayBuf(), ReplayBuf()]
    public var tPublish: UInt64 = 0
    public var tClaim: UInt64 = 0
    public var tDrop: UInt64 = 0
    public var tInvalid: UInt64 = 0
    public var tWsteps: UInt64 = 0
    public var tRsteps: UInt64 = 0
    public var tStall: UInt32 = 0
    public var tTear: UInt32 = 0
    public var tCanary: UInt32 = 0
    public var step: UInt32 = 0 // events consumed

    /// FNV-1a-64 over the canonical serialization — u64.
    public var hash: UInt64 = 0

    /// The RFC-0019 initial state (including the pinned step-0 hash).
    public init() {
        wWork = 1
        rWork = 2
        hash = weftReplayFnv1a(serialize())
    }
}

// ---------------------------------------------------------------------------
// Serialization (canonical LE byte layout — RFC-0019, normative)
// ---------------------------------------------------------------------------

extension WeftReplayState {
    /// Canonical little-endian serialization (RFC-0019): 111 bytes —
    /// u32 latest@0, u32 epoch@4, u32 w_work@8, u32 r_work@12, u8 revoked@16,
    /// 3x(u32 seq, u32 len, u16 ver)@17, u64 t_publish/t_claim/t_drop/
    /// t_invalid/t_wsteps/t_rsteps@47..94, u32 t_stall@95, t_tear@99,
    /// t_canary@103, u32 step@107. Exposed for cross-port byte tests.
    public func serialize() -> [UInt8] {
        var out = [UInt8](repeating: 0, count: 111)
        var p = 0
        func putU32(_ v: UInt32) {
            out[p] = UInt8(v & 0xff)
            out[p + 1] = UInt8((v >> 8) & 0xff)
            out[p + 2] = UInt8((v >> 16) & 0xff)
            out[p + 3] = UInt8((v >> 24) & 0xff)
            p += 4
        }
        func putU16(_ v: UInt16) {
            out[p] = UInt8(v & 0xff)
            out[p + 1] = UInt8((v >> 8) & 0xff)
            p += 2
        }
        func putU64(_ v: UInt64) {
            for i in 0..<8 {
                out[p + i] = UInt8((v >> UInt64(8 * i)) & 0xff)
            }
            p += 8
        }
        putU32(latest)
        putU32(epoch)
        putU32(wWork)
        putU32(rWork)
        out[p] = revoked
        p += 1
        for i in 0..<3 {
            putU32(buf[i].seq)
            putU32(buf[i].len)
            putU16(buf[i].ver)
        }
        putU64(tPublish)
        putU64(tClaim)
        putU64(tDrop)
        putU64(tInvalid)
        putU64(tWsteps)
        putU64(tRsteps)
        putU32(tStall)
        putU32(tTear)
        putU32(tCanary)
        putU32(step)
        return out
    }
}

// ---------------------------------------------------------------------------
// FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3)
// ---------------------------------------------------------------------------

/// FNV-1a 64 over a byte range — one definition shared by every fold path.
/// UInt64 with the wrapping operators (&*, &^) IS the mod-2^64 arithmetic
/// FNV-1a needs.
public func weftReplayFnv1a(_ data: [UInt8]) -> UInt64 {
    var h: UInt64 = 0xcbf29ce484222325
    for b in data {
        h ^= UInt64(b)
        h = h &* 0x100000001b3
    }
    return h
}

// ---------------------------------------------------------------------------
// Fold
// ---------------------------------------------------------------------------

/// Reset to the RFC-0019 initial state (including the initial hash — the
/// hash of step 0 is a pinned parity vector across all ports).
public func weftReplayInit(_ s: inout WeftReplayState) {
    s = WeftReplayState()
}

extension WeftReplayState {
    /// Apply ONE event. Returns `.ok` and updates hash, or a verdict
    /// WITHOUT mutating state (a disagreement leaves the fold at the last
    /// good step — the debugger can inspect it).
    ///
    /// `kind` is the RFC-0014 u16 kind; `aux`/`data` are the u16/u32
    /// payloads. The transition table is NORMATIVE (weft_replay.c) —
    /// mirror, never redesign.
    @discardableResult
    public mutating func step(kind: UInt16, aux: UInt16, data: UInt32) -> ReplayResult {
        // speculative copy: disagreement leaves the caller's state untouched
        // (value semantics = the C's `weft_replay_state_t next = *s`)
        var next = self
        switch kind {
        case WeftTraceKind.publish:
            // buf[w_work] = {seq, aux, v1}; old = latest; latest = w_work;
            // w_work = old. THE exchange, exactly as weft.c's step 4-5.
            next.buf[Int(next.wWork)].seq = data
            next.buf[Int(next.wWork)].len = UInt32(aux)
            next.buf[Int(next.wWork)].ver = 1
            let old = next.latest
            next.latest = next.wWork
            next.wWork = old
            next.revoked = 0 // modeling rule 2: publish implies rebind
            next.tPublish &+= 1
            next.tWsteps &+= 1
        case WeftTraceKind.claim:
            // mine = latest; latest = r_work; r_work = mine.
            let mine = next.latest
            next.latest = next.rWork
            next.rWork = mine
            if next.buf[Int(mine)].seq != data {
                return .disagree // rule 3: never silent
            }
            next.tClaim &+= 1
            next.tRsteps &+= 1
        case WeftTraceKind.drop:
            // aux is the u16 epoch at ACK — UInt32(aux) zero-extends.
            next.epoch = UInt32(aux)
            next.tDrop &+= 1
        case WeftTraceKind.revoke:
            next.revoked = 1
        case WeftTraceKind.ack:
            next.epoch = data // epoch after ACK
        case WeftTraceKind.stall:
            next.tStall &+= 1
        case WeftTraceKind.tear:
            next.tTear &+= 1
        case WeftTraceKind.canaryFail:
            next.tCanary &+= 1
        default:
            return .badKind
        }
        next.step &+= 1
        next.hash = weftReplayFnv1a(next.serialize())
        self = next
        return .ok
    }
}
