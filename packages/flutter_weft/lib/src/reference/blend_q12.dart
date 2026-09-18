// blend_q12.dart — the Q12 raster alpha-blend, Dart driver layer
// (Series 8; arithmetic-identical to core/c/blend_q12.c, blend.ts,
// BlendQ12.kt, BlendQ12.swift).
//
// WHY EXISTS: the cadence policies emit alphaQ12 decisions; this is the
// single spec'd raster op on the Dart side — integer only, zero
// allocation, bit-exact to the C SIMD kernel (the xlang gate,
// fixtures/xlang-blend/, byte-compares every port against the C golden
// digests). GovernedFanoutConsumer's blend delegates here.
//
// PERF TIERING (honest): the VM tier is this scalar loop (Dart AOT keeps
// it to a few ns/pixel); the SIMD tier is the C kernel — on Flutter, the
// optional FFI hook [nativeBlend] (bound by the embedder to
// weft_blend_q12 from core/c, which carries the AVX2/NEON paths and the
// runtime dispatch) replaces the per-pixel loop with a single native
// call at IDENTICAL bit-exactness (the golden digests pin both tiers to
// the same bytes). When the symbol is unlinked, [blendWords] stays pure
// Dart and the contract holds.

/// Q12 one (the saturated blend).
const int blendOneQ12 = 4096;

/// Optional native hook: bind to core/c's weft_blend_q12 via dart:ffi in
/// the embedder (weft_blend_q12(const uint32_t*, const uint32_t*,
/// uint32_t*, size_t, unsigned) — see weft_painter/fanout_ffi for the
/// binding pattern). Typed `Function?` so this module stays zero-
/// dependency pure Dart (run.sh compiles it without pub get); the
/// embedder's binding owns its pointer types and wraps them into this
/// seam. Null by default — the pure-Dart path is in charge.
Function? nativeBlend;

/// Blend ONE packed RGBA8888 pixel (u32 word) toward [b] by
/// alphaQ12/4096. The exact arithmetic every port reproduces.
int blendQ12Packed(int a, int b, int alphaQ12) {
  var alpha = alphaQ12;
  if (alpha < 0) alpha = 0;
  if (alpha > blendOneQ12) alpha = blendOneQ12;
  final inv = blendOneQ12 - alpha;
  final r = ((a & 0xff) * inv + (b & 0xff) * alpha) >>> 12;
  final g = (((a >>> 8) & 0xff) * inv + ((b >>> 8) & 0xff) * alpha) >>> 12;
  final bl = (((a >>> 16) & 0xff) * inv + ((b >>> 16) & 0xff) * alpha) >>> 12;
  final al = (((a >>> 24) & 0xff) * inv + ((b >>> 24) & 0xff) * alpha) >>> 12;
  return r | (g << 8) | (bl << 16) | (al << 24);
}

/// Blend two word lists into [out] (zero allocation, one pass). [out] may
/// alias [prev] (in-place blending — the governed consumer's history
/// pattern). Dispatches to [nativeBlend] when the embedder bound it; the
/// two tiers are pinned to identical bytes by the golden digests.
void blendWords(List<int> prev, List<int> newest, int alphaQ12, List<int> out) {
  final hook = nativeBlend;
  if (hook != null) {
    // The FFI tier owns buffer lifetime; the embedder's binding wraps its
    // own pointer types — this module never sees them (declared seam).
    hook(prev, newest, out,
        prev.length < newest.length ? prev.length : newest.length,
        alphaQ12.clamp(0, blendOneQ12));
    return;
  }
  final alpha = alphaQ12.clamp(0, blendOneQ12);
  final inv = blendOneQ12 - alpha;
  final n = prev.length < newest.length
      ? (prev.length < out.length ? prev.length : out.length)
      : (newest.length < out.length ? newest.length : out.length);
  for (var i = 0; i < n; i++) {
    final a = prev[i];
    final b = newest[i];
    final r = ((a & 0xff) * inv + (b & 0xff) * alpha) >>> 12;
    final g = (((a >>> 8) & 0xff) * inv + ((b >>> 8) & 0xff) * alpha) >>> 12;
    final bl =
        (((a >>> 16) & 0xff) * inv + ((b >>> 16) & 0xff) * alpha) >>> 12;
    final al =
        (((a >>> 24) & 0xff) * inv + ((b >>> 24) & 0xff) * alpha) >>> 12;
    out[i] = r | (g << 8) | (bl << 16) | (al << 24);
  }
}

/// FNV-1a 64 over the output words — the xlang-blend digest (matches the
/// C kernel's --digests CSV and the other ports' verifiers).
String fnv1a64Words(List<int> words) {
  var h = BigInt.parse('cbf29ce484222325', radix: 16);
  final mask = BigInt.parse('ffffffffffffffff', radix: 16);
  final prime = BigInt.parse('100000001b3', radix: 16);
  for (final w in words) {
    for (var shift = 0; shift < 32; shift += 8) {
      h ^= BigInt.from((w >> shift) & 0xff);
      h = (h * prime) & mask;
    }
  }
  return h.toRadixString(16).padLeft(16, '0');
}
