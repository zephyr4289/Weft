// TraceEvents.swift — RFC 0014 kernel-trace emitter (issue #20 task 1), Swift side.
//
// Runs the deterministic scenario shared with the C reference
// (core/c/trace_dump.c) and the TS emitter (trace_emitter.mjs) and prints
// the packed event stream as lowercase hex. run.sh byte-compares all ports.
//
// Toolchain policy (per-port honesty pattern): runs only when swiftc is
// present; CI owns the leg (apple-packages workflow).
//
// Build & run:
//   swiftc fixtures/xlang-trace/swift/TraceEvents.swift core/swift/Weft.swift \
//     -o /tmp/trace-swift && /tmp/trace-swift 2000 0x00C0FFEE

import Foundation

var seedState: UInt32 = 0

func xorshift32() -> UInt32 {
    var x = seedState
    if x == 0 { x = 0x9E3779B9 }
    x ^= x << 13
    x ^= x >> 17
    x ^= x << 5
    seedState = x
    return x
}

func packEvent(_ kind: UInt16, _ aux: UInt16, _ data: UInt32) -> String {
    var out = ""
    var k = kind.littleEndian
    var a = aux.littleEndian
    var d = data.littleEndian
    withUnsafeBytes(of: &k) { for b in $0 { out += String(format: "%02x", b) } }
    withUnsafeBytes(of: &a) { for b in $0 { out += String(format: "%02x", b) } }
    withUnsafeBytes(of: &d) { for b in $0 { out += String(format: "%02x", b) } }
    return out
}

let args = CommandLine.arguments
let n = args.count > 1 ? Int(args[1]) ?? 2000 : 2000
let seedRaw = args.count > 2 ? args[2] : "0x00C0FFEE"
let seed = UInt32(seedRaw.hasPrefix("0x") ? String(seedRaw.dropFirst(2)), radix: 16) ?? 0x00C0FFEE

var out = ""
let w = Weft(payloadMax: 64)
seedState = seed
var seq: UInt32 = 0
let revokeStep = n / 2

for step in 0..<n {
    let plen = Int(xorshift32() % 65)
    seq += 1
    let cursor = w.wBegin
    for i in 0..<plen {
        cursor.storeBytes(of: pat(seq: seq, i: UInt32(i)), as: UInt8.self, toByteOffset: i)
    }
    let r = w.publish(seq: seq, payloadLen: UInt32(plen))
    if r == .droppedRevoked {
        out += packEvent(3, 0, seq)            // DROP
        out += packEvent(5, 0, w.epochVal())   // ACK
    } else {
        out += packEvent(1, UInt16(plen), seq) // PUBLISH
    }
    if xorshift32() % 3 == 0 {
        _ = w.claim()
        out += packEvent(2, 0, w.rSeq())       // CLAIM
        let canaryPtr = w.buffers[Int(w.rWork)].advanced(by: w.bufSize - 8)
        let canary = canaryPtr.load(as: UInt64.self)
        if canary != UInt64(w.rSeq()) {
            fatalError("canary check FAILED at step \(step)")
        }
    }
    if step == revokeStep {
        let e0 = w.epochVal()
        w.revoke()
        out += packEvent(4, 0, e0)             // REVOKE
    }
}
print(out)
