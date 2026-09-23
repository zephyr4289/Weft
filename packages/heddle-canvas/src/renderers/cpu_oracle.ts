// cpu_oracle.ts — the reference decimation + hashing oracle.
//
// WHY EXISTS: the Pillar 4 cross-tier ==-gate. Three roads reduce the same
// waveform to per-column min/max pairs — the WGSL compute pass (Tier 1),
// the GLSL transform-feedback pass (Tier 2), and this CPU pass (Tier 3's
// raster input AND the test oracle). All three implement the IDENTICAL
// window walk below, with order-independent min/max comparisons, so the
// results are bit-exact and comparable via FNV-1a over the f32 bits —
// the house discipline (a gate you can diff, not a screenshot you squint
// at). Any tier regression is a hash mismatch, not a "looks different".
//
// THE WINDOW WALK (RFC-0022 §4.2 — the one definition, everywhere):
//   visible     = min(writePos, capacity)
//   windowStart = writePos % capacity        (one past the newest slot)
//   slot(j)     = (windowStart - visible + j + capacity) % capacity
//     j = 0 is the OLDEST visible sample, j = visible-1 the NEWEST.
//   column c covers j in [c*bucket, min((c+1)*bucket, visible))
//     bucket = ceil(visible / cols)

/** FNV-1a over u32 words (house hash; stable across engines). */
export function fnv1a32(words: Uint32Array, seed = 0x811c9dc5): number {
  let h = seed >>> 0;
  for (let i = 0; i < words.length; i++) {
    h ^= words[i] >>> 0;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

/** f32 bits view helper (zero-copy over the lane words). */
export function bitsOf(f32: Float32Array): Uint32Array {
  return new Uint32Array(f32.buffer, f32.byteOffset, f32.length);
}

/**
 * The reference min/max decimation. Writes `cols` [min,max] pairs into
 * `out` (length >= cols*2). Columns with an empty range get (0, 0) —
 * the same sentinel every GPU tier emits for tail columns.
 */
export function decimateWindowMinMax(
  f32: Float32Array,
  capacity: number,
  visible: number,
  windowStart: number,
  cols: number,
  out: Float32Array,
): void {
  const vis = Math.min(visible, capacity);
  const bucket = Math.max(1, Math.ceil(vis / cols));
  for (let c = 0; c < cols; c++) {
    const j0 = c * bucket;
    if (j0 >= vis) {
      out[c * 2] = 0;
      out[c * 2 + 1] = 0;
      continue;
    }
    const j1 = Math.min((c + 1) * bucket, vis);
    let lo = Infinity;
    let hi = -Infinity;
    for (let j = j0; j < j1; j++) {
      const slot = (windowStart - vis + j + capacity) % capacity;
      const v = f32[slot];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    out[c * 2] = lo;
    out[c * 2 + 1] = hi;
  }
}

/**
 * FNV-1a over the decimated min/max pairs (f32 bits) — the cross-tier
 * comparison key. Callers pass a preallocated Float32Array scratch.
 */
export function decimateHash(
  f32: Float32Array,
  capacity: number,
  visible: number,
  windowStart: number,
  cols: number,
  scratch: Float32Array,
): number {
  decimateWindowMinMax(f32, capacity, visible, windowStart, cols, scratch);
  const bits = bitsOf(scratch.subarray(0, cols * 2));
  return fnv1a32(bits);
}
