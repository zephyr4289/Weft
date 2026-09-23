import Foundation

// clean.swift — clean Swift fixture: ZERO findings expected.

@weft_hot
func onPacket(_ pkt: UnsafeRawBufferPointer, into out: UnsafeMutableRawBufferPointer) {
    var acc: UInt = 0
    for i in 0..<64 {
        acc += UInt(pkt[i])
    }
    out.storeBytes(of: acc, as: UInt.self)
    let scaled = Double(acc) * 0.5
    out.storeBytes(of: scaled.bitPattern, toByteOffset: 8, as: UInt64.self)
}
