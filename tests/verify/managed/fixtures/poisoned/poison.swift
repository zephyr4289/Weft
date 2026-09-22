import Foundation

// poison.swift — poisoned Swift fixture. Every commented rule id MUST fire.

@weft_hot
func onPacket(_ pkt: UnsafeRawBufferPointer, into out: UnsafeMutableRawBufferPointer) {
    let scratch = UnsafeMutableRawPointer.allocate(byteCount: 64, alignment: 16) // expect: WV-SW-001
    let label = String(pkt.count)            // expect: WV-SW-002
    let copy = Array(pkt)                    // expect: WV-SW-002
    let cb = { (n: Int) in n + 1 }           // expect: WV-SW-003
    _ = scratch; _ = label; _ = copy; _ = cb(1)
}
