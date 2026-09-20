// TrendTrace.swift — RFC 0020 cross-language verdict-stream emitter, Swift side.
//
// Drives the trend estimator with the deterministic behind trace
// (behind = xorshift32(state) % 64, seed 0x00C0FFEE) and prints the packed
// verdict stream (verdict << 6 | min(skip_n, 63), hex, one line) —
// byte-identical to core/c/trend_runner.c (the C reference), trend_emitter.mjs
// (TS), and the Kotlin/Dart VM emitters beside this one.
// fixtures/xlang-trend/run.sh byte-compares them all.
//
// Pinned parity vector (from the C reference — do not "fix" it): the
// 2000-sample hex stream hashes (FNV-1a over the stream) to
// 0x11187b9a02b378ef.
//
// Build & run (standalone — WeftTrend.swift is stdlib-only):
//   swiftc -O core/swift/WeftTrend.swift \
//          fixtures/xlang-trend/vm/swift/TrendTrace.swift \
//          -o /tmp/trend-trace-swift
//   /tmp/trend-trace-swift [STEPS [SEED]]

import Foundation

@main
struct TrendTrace {
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

        let t = WeftTrend()
        var out = TrendOut()
        var packed = ""
        packed.reserveCapacity(Int(steps) * 2)
        for _ in 0..<steps {
            seed = xorshift32(seed)
            let behind = seed % 64
            _ = t.observe(behind: behind, out: &out)
            let byte = weftTrendPack(out)
            packed += String(format: "%02x", byte)
        }
        // One trailing newline — the exact format of the other emitters.
        print(packed)
    }
}
