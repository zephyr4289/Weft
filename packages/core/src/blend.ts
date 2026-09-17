// blend.ts — the Q12 raster alpha-blend, TS reference (Series 8).
//
// WHY EXISTS: the cadence policies (RFC-0009 §cadence + RFC-0012) emit
// `alphaQ12` decisions; the raster op that consumes them — per 8-bit
// channel of each packed RGBA8888 word:
//
//     out_c = (prev_c * (4096 - alpha) + newest_c * alpha) >> 12
//
// — was previously hand-rolled per consumer (the governed consumers'
// blendQ12 helpers). This module is the single spec'd reference: integer
// only, zero allocation on the word-array path, bit-identical to the C
// SIMD kernel (core/c/blend_q12.c — SSE4.1/AVX2/NEON with runtime
// dispatch, golden-gated), to BlendQ12.kt, BlendQ12.swift, and
// blend_q12.dart. The cross-language gate (fixtures/xlang-blend/)
// byte-compares every port against the C golden digests.
//
// PERF TIERING (honest): TS is the parity reference, not the speed tier.
// The native tiers are the C kernel (AVX2 8.4x hot / 5.3x 4K in the
// sandbox), Swift's stdlib SIMD (NEON on arm64), and Android's NDK build
// of the same C file via weft-core's externalNativeBuild. FFI consumers
// (Flutter) call the C kernel through blend_q12.dart's optional native
// hook and fall back to this exact arithmetic when unlinked.

/// Q12 one (the saturated blend).
export const BLEND_ONE_Q12 = 4096;

/// Blend ONE packed RGBA8888 pixel (u32 word) toward `newest` by
/// alphaQ12/4096. The exact arithmetic every port and every SIMD path
/// reproduces.
export function blendQ12Packed(a: number, b: number, alphaQ12: number): number {
  const alpha = alphaQ12 < 0 ? 0 : alphaQ12 > BLEND_ONE_Q12 ? BLEND_ONE_Q12 : alphaQ12;
  const inv = BLEND_ONE_Q12 - alpha;
  const r = ((a & 0xff) * inv + (b & 0xff) * alpha) >>> 12;
  const g = (((a >>> 8) & 0xff) * inv + ((b >>> 8) & 0xff) * alpha) >>> 12;
  const bl = (((a >>> 16) & 0xff) * inv + ((b >>> 16) & 0xff) * alpha) >>> 12;
  const al = (((a >>> 24) & 0xff) * inv + ((b >>> 24) & 0xff) * alpha) >>> 12;
  return r | (g << 8) | (bl << 16) | (al << 24);
}

/// Blend two word buffers into `out` (zero allocation, one pass). `out`
/// may alias `prev` (in-place blending — the governed consumer's history
/// pattern). Aliasing `newest` is safe for alpha < 4096 only and is NOT
/// the supported pattern.
export function blendQ12Words(
  prev: Uint32Array,
  newest: Uint32Array,
  alphaQ12: number,
  out: Uint32Array
): void {
  const alpha = alphaQ12 < 0 ? 0 : alphaQ12 > BLEND_ONE_Q12 ? BLEND_ONE_Q12 : alphaQ12;
  const inv = BLEND_ONE_Q12 - alpha;
  const n = Math.min(prev.length, newest.length, out.length);
  for (let i = 0; i < n; i++) {
    const a = prev[i];
    const b = newest[i];
    const r = ((a & 0xff) * inv + (b & 0xff) * alpha) >>> 12;
    const g = (((a >>> 8) & 0xff) * inv + ((b >>> 8) & 0xff) * alpha) >>> 12;
    const bl = (((a >>> 16) & 0xff) * inv + ((b >>> 16) & 0xff) * alpha) >>> 12;
    const al = (((a >>> 24) & 0xff) * inv + ((b >>> 24) & 0xff) * alpha) >>> 12;
    out[i] = r | (g << 8) | (bl << 16) | (al << 24);
  }
}

/// FNV-1a 64 over the output words (the xlang-blend digest — matches the
/// C kernel's --digests CSV and the other ports' verifiers byte-for-byte).
export function fnv1a64Words(words: Uint32Array): string {
  let h = 0xcbf29ce484222325n;
  const buf = new Uint8Array(words.buffer, words.byteOffset, words.byteLength);
  for (let i = 0; i < buf.length; i++) {
    h ^= BigInt(buf[i]);
    h = (h * 0x100000001b3n) & 0xffffffffffffffffn;
  }
  return h.toString(16).padStart(16, '0');
}
