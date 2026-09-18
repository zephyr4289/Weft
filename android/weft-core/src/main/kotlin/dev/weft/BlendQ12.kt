// BlendQ12.kt — the Q12 raster alpha-blend, Kotlin driver layer (Series 8).
//
// WHY EXISTS: the cadence policies emit alphaQ12 decisions; every raster
// consumer hand-rolled the same per-channel blend. This is the single
// spec'd implementation on the JVM — integer only, zero allocation, and
// bit-identical to the C SIMD kernel (core/c/blend_q12.{h,c},
// SSE4.1/AVX2/NEON with runtime dispatch), to blend.ts, BlendQ12.swift,
// and blend_q12.dart. The xlang gate (fixtures/xlang-blend/) byte-compares
// every port against the C golden digests; GovernedFanoutConsumer
// delegates here instead of its private helper.
//
// PERF TIERING (honest): the JVM tier is this scalar loop (the JIT keeps
// it to a few ns/pixel); the SIMD tiers are the C kernel (AVX2 measured
// 8.4x hot / 5.3x 4K in the sandbox — blend_runner) consumable on Android
// through weft-core's externalNativeBuild, and Swift's stdlib SIMD on
// Apple. Android apps that want the native path blend through JNI with
// the SAME bit-exact contract; the golden digests are the proof.

package dev.weft

/** Q12 one (the saturated blend). */
public const val BLEND_ONE_Q12: Int = 4096

/**
 * The Q12 raster alpha-blend. Integer only, zero allocation, no rounding
 * term: every port truncates identically (the xlang gate proves it).
 */
public object BlendQ12 {
    /**
     * Blend ONE packed RGBA8888 pixel (u32 word) toward [b] by
     * alphaQ12/4096. The exact arithmetic every port and every SIMD path
     * reproduces (alphaQ12 clamps to 0..4096 at the boundary — the cadence
     * policies never emit values outside the range).
     */
    public fun blendPacked(a: Int, b: Int, alphaQ12: Int): Int {
        val alpha = alphaQ12.coerceIn(0, BLEND_ONE_Q12)
        val inv = BLEND_ONE_Q12 - alpha
        val r = ((a and 0xff) * inv + (b and 0xff) * alpha) ushr 12
        val g = ((a ushr 8 and 0xff) * inv + (b ushr 8 and 0xff) * alpha) ushr 12
        val bl = ((a ushr 16 and 0xff) * inv + (b ushr 16 and 0xff) * alpha) ushr 12
        val al = ((a ushr 24 and 0xff) * inv + (b ushr 24 and 0xff) * alpha) ushr 12
        return r or (g shl 8) or (bl shl 16) or (al shl 24)
    }

    /**
     * Blend two word buffers into [out] (zero allocation, one pass).
     * [out] may alias [prev] (in-place blending — the governed consumer's
     * history pattern). Aliasing [newest] is not a supported pattern.
     */
    public fun blendWords(prev: IntArray, newest: IntArray, alphaQ12: Int, out: IntArray) {
        val alpha = alphaQ12.coerceIn(0, BLEND_ONE_Q12)
        val inv = BLEND_ONE_Q12 - alpha
        val n = minOf(prev.size, newest.size, out.size)
        for (i in 0 until n) {
            val a = prev[i]
            val b = newest[i]
            val r = ((a and 0xff) * inv + (b and 0xff) * alpha) ushr 12
            val g = ((a ushr 8 and 0xff) * inv + (b ushr 8 and 0xff) * alpha) ushr 12
            val bl = ((a ushr 16 and 0xff) * inv + (b ushr 16 and 0xff) * alpha) ushr 12
            val al = ((a ushr 24 and 0xff) * inv + (b ushr 24 and 0xff) * alpha) ushr 12
            out[i] = r or (g shl 8) or (bl shl 16) or (al shl 24)
        }
    }
}
