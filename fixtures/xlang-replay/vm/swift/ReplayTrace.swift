// ReplayTrace.swift — RFC 0019 time-travel replay hash-log emitter, Swift side.
//
// Folds the deterministic RFC-0019 fixture scenario (the normative grammar
// below, xorshift32-seeded — the repo's canonical 04-LITMUS §0.2 generator)
// and prints the per-step u64 state hashes as one lowercase-hex line —
// byte-identical to core/c/replay_runner.c (the C reference), the Rust
// replay_xlang bin, replay_emitter.mjs (TS), and the Kotlin/Dart VM
// emitters beside this one. fixtures/xlang-replay/run.sh byte-compares
// them all.
//
// NORMATIVE SCENARIO GRAMMAR (mirror of replay_runner.c's scen_next —
// every emitter implements this EXACTLY):
//
//   state = SEED; latest=0 w_work=1 r_work=2 epoch=0 revoked=0 seq=0
//   bufseq = [0,0,0]                      # shadow seq per buffer slot
//   for i in 0..N:
//     state = xorshift32(state); op = state & 15; u = state (unsigned)
//     op < 7   : seq++; len = (u >> 4) % 1024
//                if !revoked: PUBLISH(aux=len, data=seq);
//                  bufseq[w_work] = seq; (latest,w_work) = (w_work,latest)
//                else: epoch++; DROP(aux=epoch & 0xffff, data=seq)
//     op < 12  : CLAIM(data=bufseq[latest]);
//                (latest,r_work) = (r_work,latest)
//     op == 12 : !revoked: REVOKE(data=epoch); revoked=1
//                else:     ACK(data=epoch)
//     op == 13 : revoked: ACK(data=epoch); revoked=0
//                else:    STALL(data=(u >> 4) % 8)
//     op == 14 : TEAR(data=seq)
//     else     : CANARY_FAIL(data=seq)
//
// The scenario mirror keeps its own shadow (same exchange rules as the
// fold) so CLAIM events carry the seq the model will reconstruct — the
// fold must therefore NEVER disagree; a disagreement is a hard error.
//
// Pinned parity vectors (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (this grammar, seed 0x00C0FFEE)
//
// Build & run (standalone — WeftReplay.swift is stdlib-only):
//   swiftc -O core/swift/WeftReplay.swift \
//          fixtures/xlang-replay/vm/swift/ReplayTrace.swift \
//          -o /tmp/replay-trace-swift
//   /tmp/replay-trace-swift [STEPS [SEED]]

import Foundation

@main
struct ReplayTrace {
    // UInt32 shifts match the C exactly: << discards overflow bits,
    // >> on UInt32 is logical.
    static func xorshift32(_ x: UInt32) -> UInt32 {
        var x = x
        x ^= x << 13
        x ^= x >> 17
        x ^= x << 5
        return x
    }

    static func main() {
        let args = CommandLine.arguments
        let steps = args.count > 1 ? Int64(args[1]) ?? 10_000 : 10_000
        var seed: UInt32 = 0x00C0FFEE
        if args.count > 2 {
            let raw = args[2]
            seed = raw.hasPrefix("0x") ? UInt32(raw.dropFirst(2), radix: 16) ?? seed
                       : UInt32(raw) ?? seed
        }

        // scenario shadow (RFC-0019 fixture grammar — normative table above)
        var latest: UInt32 = 0
        var wWork: UInt32 = 1
        var rWork: UInt32 = 2
        var epoch: UInt32 = 0
        var revoked = false
        var seq: UInt32 = 0
        var bufseq: [UInt32] = [0, 0, 0]

        var s = WeftReplayState()
        var out = ""
        out.reserveCapacity(Int(steps) * 16)
        for i in 0..<steps {
            seed = xorshift32(seed)
            let u = seed
            let op = u & 15
            var kind: UInt16 = 0
            var aux: UInt16 = 0
            var data: UInt32 = 0
            if op < 7 {
                seq &+= 1
                let len = (u >> 4) % 1024
                if !revoked {
                    bufseq[Int(wWork)] = seq
                    let old = latest
                    latest = wWork
                    wWork = old
                    kind = WeftTraceKind.publish
                    aux = UInt16(len)
                    data = seq
                } else {
                    epoch &+= 1
                    kind = WeftTraceKind.drop
                    aux = UInt16(epoch & 0xffff)
                    data = seq
                }
            } else if op < 12 {
                data = bufseq[Int(latest)]
                let mine = latest
                latest = rWork
                rWork = mine
                kind = WeftTraceKind.claim
            } else if op == 12 {
                if !revoked {
                    revoked = true
                    kind = WeftTraceKind.revoke
                    data = epoch
                } else {
                    kind = WeftTraceKind.ack
                    data = epoch
                }
            } else if op == 13 {
                if revoked {
                    revoked = false
                    kind = WeftTraceKind.ack
                    data = epoch
                } else {
                    kind = WeftTraceKind.stall
                    data = (u >> 4) % 8
                }
            } else if op == 14 {
                kind = WeftTraceKind.tear
                data = seq
            } else {
                kind = WeftTraceKind.canaryFail
                data = seq
            }
            let rc = s.step(kind: kind, aux: aux, data: data)
            if rc != .ok {
                FileHandle.standardError.write(
                    Data("replay_emitter: fold disagreement at step \(i)\n".utf8))
                exit(1)
            }
            // u64 rendered UNSIGNED — 16 lowercase hex digits.
            out += String(format: "%016llx", s.hash)
        }
        // One trailing newline — the exact format of the other emitters.
        print(out)
    }
}
