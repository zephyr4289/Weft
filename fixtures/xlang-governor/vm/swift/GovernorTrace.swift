// GovernorTrace.swift — G5 ladder-trace emitter (RFC-0009), Swift side.
//
// Emits the packed action log for the deterministic (behind, now_ms) trace
// shared with fixtures/xlang-governor/gov_trace.mjs (TS),
// core/c/governor-test xlang-dump (C), core/rust governor_xlang (Rust),
// and the Kotlin/Dart VM emitters beside this one:
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N: state = xorshift32(state);
//                  behind = state % 128; now_ms = i;
//                  action = governor.step(behind, now_ms)
//                  emit byte (kind << 6) | min(skip_n, 63)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — the exact
// format every other emitter produces. run.sh byte-compares them all.
//
// Build & run (standalone — Governor.swift is pure Foundation, no
// swift-atomics needed):
//   swiftc -O core/swift/Governor.swift \
//          fixtures/xlang-governor/vm/swift/GovernorTrace.swift \
//          -o /tmp/gov-trace-swift
//   /tmp/gov-trace-swift [STEPS [SEED]]

import Foundation

@main
struct GovernorTrace {
    static func xorshift32(_ x0: Int64) -> Int64 {
        var x = x0 & 0xffffffff
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        return x & 0xffffffff
    }

    static func main() {
        let args = CommandLine.arguments
        let steps = args.count > 1 ? Int64(args[1]) ?? 10_000 : 10_000
        var seed: Int64 = 0x00C0FFEE
        if args.count > 2 {
            let raw = args[2]
            seed = raw.hasPrefix("0x") ? Int64(raw.dropFirst(2), radix: 16) ?? seed
                       : Int64(raw) ?? seed
        }

        let gov = FreshnessGovernor()
        var out = ""
        out.reserveCapacity(Int(steps) * 2)
        var state = seed
        for i in 0..<steps {
            state = xorshift32(state)
            let behind = state % 128
            let a = gov.step(framesBehind: behind, nowMs: i)
            let packed = (Int(a.kind) << 6) | min(Int(a.skipN), 63)
            out += String(format: "%02x", packed)
        }
        // One trailing newline — the exact format of the other emitters.
        print(out)
    }
}
