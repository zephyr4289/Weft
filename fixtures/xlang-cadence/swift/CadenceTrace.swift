// CadenceTrace.swift — PC3 cadence-trace emitter (RFC-0009 §cadence), Swift side.
//
// Emits the packed decision log for the deterministic arrival trace shared
// with the TS/Kotlin/Dart emitters (fixtures/xlang-cadence/):
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N:
//     state = xorshift32(state); arrivals = state % 5; latest += arrivals
//     for policy in [LATEST_WINS, PACED_INTERPOLATE, BURST_COALESCE]:
//       d = policy.step(latest)
//       emit byte1 = (present<<7) | (interp<<6) | (alphaQ12 >> 7)
//       emit byte2 = min(coalesced, 255)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — 6 bytes
// per tick, three policies in kind order. run.sh byte-compares all ports.
//
// Build & run:
//   swiftc -O core/swift/Governor.swift \
//          fixtures/xlang-cadence/swift/CadenceTrace.swift \
//          -o /tmp/cad-trace-swift
//   /tmp/cad-trace-swift [STEPS [SEED]]

import Foundation

@main
struct CadenceTrace {
    static func xorshift32(_ x0: Int64) -> Int64 {
        var x = x0 & 0xffffffff
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        return x & 0xffffffff
    }

    static func main() {
        let args = CommandLine.arguments
        let steps = args.count > 1 ? Int(args[1]) ?? 10_000 : 10_000
        var seed: Int64 = 0x00C0FFEE
        if args.count > 2 {
            let raw = args[2]
            seed = raw.hasPrefix("0x") ? Int64(raw.dropFirst(2), radix: 16) ?? seed
                       : Int64(raw) ?? seed
        }

        let pols = [
            CadencePolicy(policy: CadencePolicyKind.latestWins),
            CadencePolicy(policy: CadencePolicyKind.pacedInterpolate),
            CadencePolicy(policy: CadencePolicyKind.burstCoalesce),
        ]
        var out = ""
        out.reserveCapacity(steps * 12)
        var state = seed
        var latest: Int64 = 0
        for _ in 0..<steps {
            state = xorshift32(state)
            latest += state % 5
            for p in pols {
                let a = p.step(latestSeq: latest)
                let b1 = (a.present ? 1 : 0) << 7 | (a.interp ? 1 : 0) << 6 | Int(a.alphaQ12 >> 7)
                out += String(format: "%02x%02x", b1, min(a.coalesced, 255))
            }
        }
        print(out)
    }
}
