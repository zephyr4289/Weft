// BlendQ12.swift — the Q12 raster alpha-blend, Swift driver layer
// (Series 8; arithmetic-identical to core/c/blend_q12.c, blend.ts,
// BlendQ12.kt, blend_q12.dart).
//
// WHY EXISTS: the cadence policies emit alphaQ12 decisions; the raster op
// that consumes them must be one spec'd, tested thing — not a per-view
// hand-rolled loop. This port uses the Swift standard library's SIMD4
// types (the compiler lowers them to NEON on arm64 and SSE on x86_64 —
// the "ARM NEON" tier the Series-8 work order asked for, expressed in
// safe stdlib code) with a scalar fallback for the tail, both bit-exact
// to the C kernel. The xlang gate (fixtures/xlang-blend/) byte-compares
// this port against the C golden digests on the Apple CI leg (swiftc is
// not available in the Linux sandbox — the repo's declared-skip honesty
// pattern).

import Foundation

/// Q12 one (the saturated blend).
public let blendOneQ12: Int32 = 4096

public enum BlendQ12 {
    /// Blend ONE packed RGBA8888 pixel (u32 word) toward `b` by
    /// alphaQ12/4096. The exact arithmetic every port reproduces.
    @inlinable
    public static func blendPacked(_ a: UInt32, _ b: UInt32, _ alphaQ12: Int32) -> UInt32 {
        let alpha = max(0, min(blendOneQ12, alphaQ12))
        let inv = UInt32(blendOneQ12 - alpha)
        let al = UInt32(alpha)
        let r = ((a & 0xff) * inv + (b & 0xff) * al) >> 12
        let g = (((a >> 8) & 0xff) * inv + ((b >> 8) & 0xff) * al) >> 12
        let bl = (((a >> 16) & 0xff) * inv + ((b >> 16) & 0xff) * al) >> 12
        let ah = (((a >> 24) & 0xff) * inv + ((b >> 24) & 0xff) * al) >> 12
        return r | (g << 8) | (bl << 16) | (ah << 24)
    }

    /// Blend two word buffers into `out` (zero allocation, one pass).
    /// `out` may alias `prev` (in-place blending — the governed consumer's
    /// history pattern). The vector path processes 4 pixels per iteration
    /// via SIMD4<UInt32> (NEON/SSE-native); the scalar tail and any
    /// sub-4 remainder use the exact per-pixel formula.
    @inlinable
    public static func blendWords(_ prev: [UInt32], _ newest: [UInt32],
                                  _ alphaQ12: Int32, _ out: inout [UInt32]) {
        let n = min(prev.count, newest.count, out.count)
        let alpha = max(0, min(blendOneQ12, alphaQ12))
        let inv = UInt32(blendOneQ12 - alpha)
        let al = UInt32(alpha)
        let invV = SIMD4<UInt32>(repeating: inv)
        let alV = SIMD4<UInt32>(repeating: al)
        let ffV = SIMD4<UInt32>(repeating: 0xff)
        let shift8V = SIMD4<UInt32>(repeating: 8)
        let shift16V = SIMD4<UInt32>(repeating: 16)
        let shift24V = SIMD4<UInt32>(repeating: 24)
        let shift12V = SIMD4<UInt32>(repeating: 12)
        var i = 0
        while i + 4 <= n {
            // Load 4 pixels, split channels into 32-bit lanes (each lane a
            // pure channel value 0..255), multiply-add exactly, shift,
            // re-interleave. Products <= 255*4096 < 2^21 — no overflow,
            // no masking between ops: the lanes ARE exact scalar values.
            let pa = SIMD4<UInt32>(prev[i], prev[i + 1], prev[i + 2], prev[i + 3])
            let pb = SIMD4<UInt32>(newest[i], newest[i + 1], newest[i + 2], newest[i + 3])
            let rA = pa & ffV
            let rB = pb & ffV
            let gA = (pa &>> shift8V) & ffV
            let gB = (pb &>> shift8V) & ffV
            let bA = (pa &>> shift16V) & ffV
            let bB = (pb &>> shift16V) & ffV
            let aA = (pa &>> shift24V) & ffV
            let aB = (pb &>> shift24V) & ffV
            let r = ((rA &* invV) &+ (rB &* alV)) &>> shift12V
            let g = ((gA &* invV) &+ (gB &* alV)) &>> shift12V
            let b = ((bA &* invV) &+ (bB &* alV)) &>> shift12V
            let a = ((aA &* invV) &+ (aB &* alV)) &>> shift12V
            out[i] = r.x | (g.x << 8) | (b.x << 16) | (a.x << 24)
            out[i + 1] = r.y | (g.y << 8) | (b.y << 16) | (a.y << 24)
            out[i + 2] = r.z | (g.z << 8) | (b.z << 16) | (a.z << 24)
            out[i + 3] = r.w | (g.w << 8) | (b.w << 16) | (a.w << 24)
            i += 4
        }
        while i < n {
            out[i] = blendPacked(prev[i], newest[i], alphaQ12)
            i += 1
        }
    }

    /// FNV-1a 64 over the output words — the xlang-blend digest (matches
    /// the C kernel's --digests CSV and the other ports' verifiers).
    @inlinable
    public static func fnv1a64Words(_ words: [UInt32]) -> UInt64 {
        var h: UInt64 = 0xcbf29ce484222325
        for w in words {
            for shift in stride(from: 0, to: 32, by: 8) {
                h ^= UInt64((w >> UInt32(shift)) & 0xff)
                h = h &* 0x100000001b3
            }
        }
        return h
    }
}
