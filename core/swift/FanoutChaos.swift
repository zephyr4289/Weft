// FanoutChaos.swift — RFC 0011 deterministic chaos engine, Swift port.
//
// "chaos contract" — mirrors core/c/fanout_chaos.c (the reference oracle)
// micro-step for micro-step, draw for draw. Given the same config, the
// stepped verdict JSON is BYTE-IDENTICAL to the C engine's. The stepped
// verdict is verified against the committed golden fixture
// (tools/chaos-fixtures/stepped-golden-200k.json) in FanoutChaosTests, and
// the CI parity script diffs live runs C-vs-Swift on macOS runners where
// both toolchains exist.
//
// u32 discipline: Swift's UInt32 is the exact analog of C's uint32_t —
// &* (wrapping multiply), &<< / &>> (wrapping/logical shifts), ^ (xor).
//
// LAW 4 honesty: the stepped engine models the RFC 0004 protocol exactly
// (FI1/FI2 brackets, the bounded 4-attempt claim loop, latest-wins).
//
// STATUS: SOURCE-ONLY IN THE LINUX SANDBOX (no swiftc) — COMPILED AND
// EXECUTED IN THE APPLE CI LEG (apple-packages.yml, swift test). The
// banner flips only when the test target proves it on a macOS runner.

import Foundation

public struct ChaosConfig: Sendable {
    public let seed: UInt32       // master seed (u32)
    public let steps: UInt64      // scheduler step budget (stepped mode)
    public let slots: Int         // M — ring depth
    public let words: Int         // W — payload words per slot
    public let readers: Int       // R — concurrent reader SMs
    public let frames: UInt32     // F — frames the writer publishes
    public let chaosRate: Int     // per-mille fault probability per step

    public init(seed: UInt32, steps: UInt64, slots: Int, words: Int,
                readers: Int, frames: UInt32, chaosRate: Int) {
        self.seed = seed; self.steps = steps; self.slots = slots
        self.words = words; self.readers = readers; self.frames = frames
        self.chaosRate = chaosRate
    }
}

public struct ChaosVerdict: Sendable {
    public let pass: Bool
    public let engine: String
    public let json: String // the contract JSON (byte-identical across ports)
}

// ---------------------------------------------------------------------------
// The chaos contract: PRNG + pattern (mirror of core/c/fanout_chaos.c)
// ---------------------------------------------------------------------------


struct ChaosRng {
    var a: UInt32 = 0, b: UInt32 = 0, c: UInt32 = 0, d: UInt32 = 0

    mutating func seed(_ seed: UInt32) {
        a = mix32(seed ^ 0xA341316C)
        b = mix32(seed ^ 0xC8013EA4)
        c = a ^ 0x9E3779B9
        d = b ^ 0x85EBCA6B
    }

    mutating func next() -> UInt32 {
        var t = d
        let s = a
        d = c; c = b; b = s
        t = t &<< 11
        t = t &>> 8
        a = t ^ s ^ (s >> 10)
        return a
    }
}

func tword(_ seq: UInt32, _ w: Int) -> UInt32 {
    mix32(seq &* 2654435761 &+ UInt32(w))
}

// ---------------------------------------------------------------------------
// Stepped engine — the deterministic scheduler (mirror of the C oracle)
// ---------------------------------------------------------------------------

private let W_IDLE = 0, W_FILL = 1, W_STAMP = 2, W_PUBLISH = 3, W_DONE = 4
private let R_IDLE = 0, R_STAMP_B = 1, R_COPY = 2, R_STAMP_A = 3, R_ACCEPT = 4
private let R_TICK_END = 5, R_SKIP_TICK = 6, R_EXHAUSTED_TICK = 7, R_DONE = 8

// preempt / stall / throttle / reorder
private let FREEZE_ADD = [1, 2, 3, 0]

public enum WeftChaosStepped {
    public static func run(_ cfg: ChaosConfig) -> ChaosVerdict {
        precondition(cfg.slots >= 2 && cfg.slots <= 64, "chaos: slots out of [2,64]")
        precondition(cfg.words >= 1 && cfg.words <= 64, "chaos: words out of [1,64]")
        precondition(cfg.readers >= 1 && cfg.readers <= 4, "chaos: readers out of [1,4]")
        precondition(cfg.chaosRate >= 0 && cfg.chaosRate <= 1000, "chaos: chaosRate out of [0,1000]")

        let M = cfg.slots, W = cfg.words, R = cfg.readers, F = Int(cfg.frames)

        // Model ring.
        var slotSeq = [UInt64](repeating: 0, count: M)
        var payload = [[UInt32]](repeating: [UInt32](repeating: 0, count: W), count: M)
        var bracketOpen = [Bool](repeating: false, count: M)
        var latest: UInt64 = 0
        var publishes: UInt64 = 0

        // Writer SM
        var ws = W_IDLE
        var wSeq: UInt32 = 1
        var wSlot = 0
        var wWord = 0
        var wRev = false

        // Reader SMs
        var rs = [Int](repeating: R_IDLE, count: R)
        var rLast = [Int](repeating: 0, count: R)
        var rTarget = [[UInt64]](repeating: [UInt64](repeating: 0, count: W), count: R)
        var rL = [UInt64](repeating: 0, count: R)
        var rSlot = [Int](repeating: 0, count: R)
        var rWord = [Int](repeating: 0, count: R)
        var rAttempts = [Int](repeating: 0, count: R)
        var rRevActive = [Bool](repeating: false, count: R)

        // Scheduler
        var rng = ChaosRng()
        rng.seed(cfg.seed)
        var freeze = [Int](repeating: 0, count: R + 1)
        var revFlag = [Bool](repeating: false, count: R + 1)

        // Ledger
        var fresh = [Int](repeating: 0, count: R)
        var dropped = [Int](repeating: 0, count: R)
        var skips = [Int](repeating: 0, count: R)
        var exhausted = [Int](repeating: 0, count: R)
        var tornAccepted = 0
        var futureClaims = 0
        var bracketViolations = 0
        var injected = [Int](repeating: 0, count: 4)
        var stepsExecuted: UInt64 = 0

        func allDone() -> Bool {
            if ws != W_DONE { return false }
            for i in 0..<R where rs[i] != R_DONE { return false }
            return true
        }

        func faultDraw() {
            if rng.next() % 1000 >= UInt32(cfg.chaosRate) { return }
            let victim = Int(rng.next() % UInt32(R + 1))
            let kind = Int(rng.next() % 4)
            freeze[victim] += FREEZE_ADD[kind]
            if kind == 3 { revFlag[victim].toggle() }
            injected[kind] += 1
        }

        func writerStep() {
            switch ws {
            case W_IDLE:
                if wSeq > cfg.frames { ws = W_DONE; return }
                wSlot = Int((wSeq - 1) % UInt32(M))
                if bracketOpen[wSlot] { bracketViolations += 1 } // L-C6
                slotSeq[wSlot] = 0                               // FI1a: invalidate FIRST
                bracketOpen[wSlot] = true
                wRev = revFlag[0]                                // fault axis: reorder
                wWord = wRev ? (W - 1) : 0
                ws = W_FILL
            case W_FILL:
                payload[wSlot][wWord] = tword(wSeq, wWord)
                if wRev {
                    if wWord == 0 { ws = W_STAMP } else { wWord -= 1 }
                } else {
                    wWord += 1
                    if wWord == W { ws = W_STAMP }
                }
            case W_STAMP:
                slotSeq[wSlot] = UInt64(wSeq)                    // FI1b: stamp (Release)
                bracketOpen[wSlot] = false
                ws = W_PUBLISH
            case W_PUBLISH:
                latest = UInt64(wSeq)                            // the publication point
                publishes += 1
                wSeq &+= 1
                ws = W_IDLE
            default:
                break
            }
        }

        func readerStep(_ i: Int) {
            switch rs[i] {
            case R_IDLE:
                let L = latest
                if L == 0 || L == UInt64(rLast[i]) { rs[i] = R_TICK_END; return }
                rL[i] = L
                rAttempts[i] = 0
                rs[i] = R_STAMP_B
            case R_STAMP_B:
                let L = rL[i]
                rSlot[i] = Int((L - 1) % UInt64(M))
                let sB = slotSeq[rSlot[i]]
                if sB != L {
                    let l2 = latest
                    if l2 == L {
                        skips[i] += 1
                        rs[i] = R_SKIP_TICK
                        return
                    }
                    rL[i] = l2
                    rAttempts[i] += 1
                    rs[i] = rAttempts[i] >= 4 ? R_EXHAUSTED_TICK : R_STAMP_B
                    return
                }
                rWord[i] = revFlag[i + 1] ? (W - 1) : 0          // fault axis: reorder
                rRevActive[i] = revFlag[i + 1]
                rs[i] = R_COPY
            case R_COPY:
                let w = rWord[i]
                rTarget[i][w] = UInt64(payload[rSlot[i]][w])
                if rRevActive[i] {
                    if w == 0 { rs[i] = R_STAMP_A } else { rWord[i] = w - 1 }
                } else {
                    rWord[i] += 1
                    if rWord[i] == W { rs[i] = R_STAMP_A }
                }
            case R_STAMP_A:
                let sA = slotSeq[rSlot[i]]
                if sA == rL[i] { rs[i] = R_ACCEPT; return }
                rL[i] = latest                                   // torn copy — chase newest
                rAttempts[i] += 1
                rs[i] = rAttempts[i] >= 4 ? R_EXHAUSTED_TICK : R_STAMP_B
            case R_ACCEPT:
                let L = rL[i]
                if L > UInt64(F) { futureClaims += 1 }           // L-C2
                for w in 0..<W where rTarget[i][w] != UInt64(tword(UInt32(L), w)) {
                    tornAccepted += 1                            // L-C1
                    break
                }
                dropped[i] += Int(L) - rLast[i] - 1
                rLast[i] = Int(L)
                fresh[i] += 1
                rs[i] = R_TICK_END
            case R_TICK_END:
                rs[i] = (ws == W_DONE && rLast[i] == F) ? R_DONE : R_IDLE
            case R_SKIP_TICK:
                rs[i] = R_TICK_END
            case R_EXHAUSTED_TICK:
                exhausted[i] += 1
                rs[i] = R_TICK_END
            default:
                break
            }
        }

        func advance(_ tid: Int) {
            if tid == 0 { writerStep() } else { readerStep(tid - 1) }
        }

        // The scheduler loop (chaos contract shape — identical to the C oracle).
        while stepsExecuted < cfg.steps && !allDone() {
            let tid = Int(rng.next() % UInt32(R + 1))
            faultDraw()
            if freeze[tid] > 0 {
                freeze[tid] -= 1     // scheduled, but made no progress
            } else {
                advance(tid)
            }
            stepsExecuted += 1
        }

        // Drain: round-robin (writer, reader 1..R), no faults, until done.
        let drainBound = 64 * (F + 16 * R * (F + 8)) + 64
        var drainedSteps = 0
        while !allDone() && drainedSteps < drainBound {
            for tid in 0...R where !allDone() {
                if freeze[tid] > 0 { freeze[tid] = 0 } // faults end with the budget
                if !allDone() { advance(tid) }
            }
            drainedSteps += 1
        }
        let drained = allDone()

        // L-C3 telescoping + L-C4 completion (adjudicated once, at drain end).
        var telescopingOk = drained
        for i in 0..<R {
            if dropped[i] != rLast[i] - fresh[i] { telescopingOk = false }
            if drained && rLast[i] != F { telescopingOk = false }
        }
        if publishes != UInt64(F) { telescopingOk = false } // L-C4

        let pass = telescopingOk && tornAccepted == 0 && futureClaims == 0 &&
                   bracketViolations == 0
        let telescopingStr = pass ? "OK" : "VIOLATED"
        let verdictStr = pass ? "PASS" : "FAIL"

        // Contract JSON — byte-identical to the C oracle (fixed field order).
        let freshStr = fresh.map(String.init).joined(separator: ",")
        let droppedStr = dropped.map(String.init).joined(separator: ",")
        let lastStr = rLast.map(String.init).joined(separator: ",")
        let skipStr = skips.map(String.init).joined(separator: ",")
        let exhStr = exhausted.map(String.init).joined(separator: ",")
        let json =
            "{\"engine\":\"weft-chaos-stepped\",\"v\":1,\"seed\":\(cfg.seed)," +
            "\"steps\":\(cfg.steps)," +
            "\"slots\":\(M),\"words\":\(W),\"readers\":\(R),\"frames\":\(F)," +
            "\"chaosRate\":\(cfg.chaosRate)," +
            "\"stepsExecuted\":\(stepsExecuted)," +
            "\"injections\":{\"preempt\":\(injected[0]),\"stall\":\(injected[1])," +
            "\"throttle\":\(injected[2]),\"reorder\":\(injected[3])}," +
            "\"ledger\":{\"publishes\":\(publishes)," +
            "\"fresh\":[\(freshStr)]," +
            "\"dropped\":[\(droppedStr)]," +
            "\"lastSeq\":[\(lastStr)]," +
            "\"skips\":[\(skipStr)]," +
            "\"exhausted\":[\(exhStr)]," +
            "\"tornAccepted\":\(tornAccepted),\"futureClaims\":\(futureClaims)," +
            "\"bracketViolations\":\(bracketViolations),\"drained\":\(drained)}," +
            "\"telescoping\":\"\(telescopingStr)\"," +
            "\"verdict\":\"\(verdictStr)\"}"

        return ChaosVerdict(pass: pass, engine: "stepped", json: json)
    }

    // Pinned vectors — MUST match core/c/fanout_chaos.c's selftest (the
    // parity anchor; see tools/chaos-fixtures/).
    public static func selftest() -> Bool {
        if mix32(0xDEADBEEF) != 3861431939 { return false }
        if tword(1, 0) != 1834104592 { return false }
        if tword(7, 3) != 2500287888 { return false }
        if tword(100, 15) != 4197121613 { return false }
        var rng = ChaosRng()
        rng.seed(42)
        let draws: [UInt32] = [1034221180, 2302191726, 1921777443, 3822789115,
                               4193179225, 3051818586, 2559959645, 2724063783]
        for want in draws where rng.next() != want { return false }
        let v = WeftChaosStepped.run(ChaosConfig(seed: 7, steps: 4000, slots: 2,
                                                 words: 2, readers: 2, frames: 4,
                                                 chaosRate: 300))
        return v.pass && v.json.contains("\"verdict\":\"PASS\"")
    }
}
