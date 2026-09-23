// oscillo_decimate.wgsl — the Tier-1 min/max decimation compute shader.
//
// ONE workgroup per column (dispatched x = columnCount, workgroup_size 1).
// Implements EXACTLY the reference window walk of
// packages/heddle-canvas/src/renderers/cpu_oracle.ts (RFC-0022 §4.2) so
// the WGSL road, the GLSL TF road and the CPU road are bit-exact on the
// same plane state — the cross-tier ==-gate hashes the min/max pairs:
//
//   vis     = min(sampleCount, capacity)
//   bucket  = max(1, ceil(vis / cols))
//   column c covers j in [c*bucket, min((c+1)*bucket, vis))
//   slot(j) = (windowStart - vis + j + capacity) % capacity
//
// Bindings: 0 samples (read-only storage, f32 per sample),
//           1 minmax  (storage, vec2<f32> per column),
//           2 params  (uniform, two vec4<u32>: a = (sampleCount, cols,
//              bucket, windowStart), b = (capacity, viewportW, viewportH, 0)).

struct Params {
  a: vec4<u32>,
  b: vec4<u32>,
};

@group(0) @binding(0) var<storage, read> samples: array<f32>;
@group(0) @binding(1) var<storage, read_write> minmax: array<vec2<f32>>;
@group(0) @binding(2) var<uniform> params: Params;

@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let c = gid.x;
  let sampleCount = params.a.x;
  let cols = params.a.y;
  let capacity = params.b.x;
  if (c >= u32(cols)) {
    return;
  }
  let vis = min(sampleCount, capacity);
  let bucket = max(1u, (vis + u32(cols) - 1u) / u32(cols));
  let j0 = c * bucket;
  if (j0 >= vis) {
    // Tail column beyond the window: the (0, 0) sentinel, same as CPU.
    minmax[c] = vec2<f32>(0.0, 0.0);
    return;
  }
  let j1 = min(j0 + bucket, vis);
  let base = params.a.w + capacity - vis; // windowStart + capacity - vis
  var lo = 3.402823466e38;                // f32 max (the CPU oracle's Infinity)
  var hi = -3.402823466e38;
  for (var j = j0; j < j1; j = j + 1u) {
    let slot = (base + j) % capacity;
    let v = samples[slot];
    lo = min(lo, v);
    hi = max(hi, v);
  }
  minmax[c] = vec2<f32>(lo, hi);
}
