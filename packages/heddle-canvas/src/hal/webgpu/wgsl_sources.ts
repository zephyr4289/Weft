// wgsl_sources.ts — the WGSL bundle (Tier 1).
//
// GENERATED from shaders/wgsl/*.wgsl by scripts/emit_shaders.mjs — DO NOT EDIT HERE.
// The drift gate (test/shaders.test.ts) re-emits and diffs; edit the
// canonical file instead.

export const WGSL_SOURCES = {
  oscilloDecimate: `// oscillo_decimate.wgsl — the Tier-1 min/max decimation compute shader.
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
`,
  oscilloRibbon: `// oscillo_ribbon.wgsl — the Tier-1 waveform ribbon render pass.
//
// One instanced quad per column: the vertical [min, max] band of that
// column (the classic oscilloscope min/max ribbon). Screen mapping is the
// SAME projection the Canvas2D tier uses (yScale 0.9, yOff 0.05) — free
// determinism, one less divergence to document.
//
// Bindings: 0 minmax (read-only storage, vec2<f32> per column),
//           1 params (uniform; a.y = cols, b.y/b.z = viewport w/h).

struct Params {
  a: vec4<u32>,
  b: vec4<u32>,
};

@group(0) @binding(0) var<storage, read> minmax: array<vec2<f32>>;
@group(0) @binding(1) var<uniform> params: Params;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) color: vec4<f32>,
};

fn valueToY(v: f32, h: f32) -> f32 {
  let yScale = (h - 1.0) * 0.9;
  let yOff = (h - 1.0) * 0.05;
  return h - 1.0 - (v * yScale + yOff);
}

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) c: u32) -> VsOut {
  let cols = f32(params.a.y);
  let w = f32(params.b.y);
  let h = f32(params.b.z);
  let colW = w / max(cols, 1.0);
  let mm = minmax[c];
  var out: VsOut;
  // 4-vertex triangle strip: (x0, lo) (x1, lo) (x0, hi) (x1, hi).
  let x0 = f32(c) * colW;
  let x1 = x0 + colW;
  let yLo = clamp(valueToY(mm.y, h), 0.0, h - 1.0); // max value -> low pixel
  let yHi = clamp(valueToY(mm.x, h), 0.0, h - 1.0); // min value -> high pixel
  var x = x0;
  var y = yLo;
  if (vi == 1u || vi == 3u) { x = x1; }
  if (vi == 2u) { y = yHi; }
  if (vi == 3u) { y = yHi; }
  // Screen (top-left origin) -> NDC.
  out.pos = vec4<f32>(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  out.color = vec4<f32>(0.184, 0.749, 0.443, 1.0); // #2fbf71 — same as Tier 3
  return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
  return in.color;
}
`,
  depthLadder: `// depth_ladder.wgsl — the Tier-1 Level 2/3 order-book depth ladder.
//
// One instanced quad per ladder row: a horizontal bar whose x extent is
// the size (quantity) and whose y band is the row's price slot. Bid/ask
// coloring from the row's side word (the Canvas2D tier's palette).
//
// Row64 layout (RFC-0022 §3.6, Law 2 — words, not struct guessing):
//   w[0] = price f32 | w[1] = size f32 | w[2] = side u32 (0 bid / 1 ask)
// The whole 64-B row is addressed as array<vec4<u32>, 4> — bitcast the
// words out. No layout ambiguity, no driver struct-packing surprises.

struct RowParams {
  a: vec4<u32>, // (rowCount, 0, 0, 0)
  b: vec4<u32>, // (viewportW, viewportH, 0, 0)
};

@group(0) @binding(0) var<storage, read> rows: array<vec4<u32>>;
@group(0) @binding(1) var<uniform> params: RowParams;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) r: u32) -> VsOut {
  var out: VsOut;
  let rowCount = params.a.x;
  if (r >= rowCount) {
    // Shader-side instance cull: bundles draw \`capacity\` instances; rows
    // beyond the live count collapse to a degenerate off-screen quad.
    out.pos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
    out.color = vec4<f32>(0.0);
    return out;
  }
  let w = f32(params.b.x);
  let h = f32(params.b.y);
  let row = r * 4u; // 4 vec4 words per 64-B row
  let price = bitcast<f32>(rows[row].x);
  let size = bitcast<f32>(rows[row].y);
  let side = rows[row].z;
  let bandH = h / f32(max(rowCount, 1u));
  let y0 = f32(r) * bandH;         // top of the price band
  let y1 = y0 + max(bandH - 1.0, 1.0); // bottom (>= 1 px tall)
  let xEnd = max(2.0, size * (w - 4.0));
  var x = 0.0;
  var y = y0;
  if (vi == 1u || vi == 3u) { x = xEnd; }
  if (vi == 2u) { y = y1; }
  if (vi == 3u) { y = y1; }
  out.pos = vec4<f32>(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  // Palette matches the Canvas2D tier: bid blue #3d7bd6, ask orange #d6873d.
  if (side == 0u) {
    out.color = vec4<f32>(0.239, 0.482, 0.839, 1.0);
  } else {
    out.color = vec4<f32>(0.839, 0.529, 0.239, 1.0);
  }
  return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
  return in.color;
}
`,
  candle: `// candles.wgsl — the Tier-1 OHLC candlestick render pass.
//
// One instanced quad per candle covering the FULL high..low extent; the
// fragment shader carves body vs wick from the interpolated coordinates
// (one draw call per candle family — body and wick ride the same quad).
//
// Candle64 layout (RFC-0022 §3.6, Law 2 — word-addressed):
//   w[0..3] = open, high, low, close (f32)
//   w[4]    = volume (f32)

struct RowParams {
  a: vec4<u32>, // (rowCount, 0, 0, 0)
  b: vec4<u32>, // (viewportW, viewportH, 0, 0)
};

@group(0) @binding(0) var<storage, read> rows: array<vec4<u32>>;
@group(0) @binding(1) var<uniform> params: RowParams;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) local: vec2<f32>,   // xFrac in [0,1] of the column, y in pixels
  @location(1) bodyY: vec2<f32>,   // pixel extents of the body (open..close)
  @location(2) upDown: f32,        // +1 bullish (close >= open), -1 bearish
};

fn valueToY(v: f32, h: f32) -> f32 {
  let yScale = (h - 1.0) * 0.92;
  let yOff = (h - 1.0) * 0.04;
  return h - 1.0 - (v * yScale + yOff);
}

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) r: u32) -> VsOut {
  var out: VsOut;
  let rowCount = params.a.x;
  if (r >= rowCount) {
    // Shader-side instance cull (bundles draw \`capacity\` instances).
    out.pos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
    out.local = vec2<f32>(0.0);
    out.bodyY = vec2<f32>(0.0);
    out.upDown = 0.0;
    return out;
  }
  let w = f32(params.b.x);
  let h = f32(params.b.y);
  let row = r * 4u;
  let open = bitcast<f32>(rows[row].x);
  let high = bitcast<f32>(rows[row].y);
  let low = bitcast<f32>(rows[row].z);
  let close = bitcast<f32>(rows[row].w);
  let colW = w / f32(max(rowCount, 1u));
  let x0 = f32(r) * colW;
  let x1 = x0 + colW;
  let yHigh = clamp(valueToY(high, h), 0.0, h - 1.0);
  let yLow = clamp(valueToY(low, h), 0.0, h - 1.0);
  let yOpen = clamp(valueToY(open, h), 0.0, h - 1.0);
  let yClose = clamp(valueToY(close, h), 0.0, h - 1.0);
  var x = x0;
  var y = yHigh;
  var xFrac = 0.0;
  if (vi == 1u || vi == 3u) { x = x1; xFrac = 1.0; }
  if (vi == 2u) { y = yLow; }
  if (vi == 3u) { y = yLow; }
  out.pos = vec4<f32>(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  out.local = vec2<f32>(xFrac, y);
  out.bodyY = vec2<f32>(min(yOpen, yClose), max(yOpen, yClose));
  out.upDown = select(-1.0, 1.0, close >= open);
  return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
  // Silhouette: the 1-px wick (middle 12% of the column) is always candle
  // colored inside the high..low quad; the body colors the middle 70% of
  // the width BETWEEN the open..close pixel band. Everything else inside
  // the quad is transparent (the quad is only a carrier).
  let wick = abs(in.local.x - 0.5) < 0.06;
  let inBodyX = abs(in.local.x - 0.5) < 0.35;
  let inBodyY = in.local.y >= in.bodyY.x && in.local.y <= in.bodyY.y;
  let bullish = in.upDown > 0.0;
  if (!wick && !(inBodyX && inBodyY)) {
    return vec4<f32>(0.0, 0.0, 0.0, 0.0);
  }
  if (bullish) {
    return vec4<f32>(0.184, 0.749, 0.443, 1.0); // #2fbf71 green
  }
  return vec4<f32>(0.835, 0.314, 0.314, 1.0);   // #d65050 red
}
`,
  pointcloud: `// pointcloud.wgsl — the Tier-1 3D telemetry / point-cloud render pass.
//
// One instanced quad per point: position.xyz (+ size in .w) from the row's
// first vec4, orientation from the quaternion vec4 (the IMU attitude), rgba
// from the third vec4. A minimal perspective projection + the quaternion
// rotates the sprite in screen space (yaw component) so attitude is VISIBLE
// without a full 3D pipeline — the mandate's "low-overhead" point cloud.
//
// PointRow128 layout (RFC-0022 §3.6, Law 2 — word-addressed):
//   w[0..3]  = pos.xyz + size (f32)
//   w[4..7]  = quaternion xyzw (f32)
//   w[8..11] = rgba (f32)

struct RowParams {
  a: vec4<u32>, // (pointCount, 0, 0, 0)
  b: vec4<u32>, // (viewportW, viewportH, 0, 0)
};

@group(0) @binding(0) var<storage, read> rows: array<vec4<u32>>;
@group(0) @binding(1) var<uniform> params: RowParams;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) p: u32) -> VsOut {
  var out: VsOut;
  let pointCount = params.a.x;
  if (p >= pointCount) {
    out.pos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
    out.color = vec4<f32>(0.0);
    return out;
  }
  let w = f32(params.b.x);
  let h = f32(params.b.y);
  let row = p * 8u; // 8 vec4 words per 128-B row
  let pos = vec3<f32>(bitcast<f32>(rows[row].x), bitcast<f32>(rows[row].y), bitcast<f32>(rows[row].z));
  let size = bitcast<f32>(rows[row].w);
  let quat = vec4<f32>(bitcast<f32>(rows[row + 1u].x), bitcast<f32>(rows[row + 1u].y),
                       bitcast<f32>(rows[row + 1u].z), bitcast<f32>(rows[row + 1u].w));
  let rgba = vec4<f32>(bitcast<f32>(rows[row + 2u].x), bitcast<f32>(rows[row + 2u].y),
                       bitcast<f32>(rows[row + 2u].z), bitcast<f32>(rows[row + 2u].w));
  // Minimal perspective: camera at +z, fov ~60 degrees.
  let zc = pos.z + 2.6; // push the unit torus in front of the camera
  let zGuard = max(zc, 0.15);
  let px = pos.x / zGuard;
  let py = pos.y / zGuard;
  let sx = (px * 0.5 + 0.5) * w; // screen x (top-left origin)
  let sy = (0.5 - py * 0.5) * h; // screen y
  // Sprite half-extent in pixels (size in world units -> screen).
  let halfPx = max(1.0, size / zGuard * h * 0.5);
  // Quad corners from vertex_index (0..3, triangle strip).
  var dx = -halfPx;
  var dy = -halfPx;
  if (vi == 1u || vi == 3u) { dx = halfPx; }
  if (vi == 2u || vi == 3u) { dy = halfPx; }
  // Yaw from the quaternion (rotation about z) — attitude made visible.
  let yaw = 2.0 * atan2(quat.z, quat.w);
  let cs = cos(yaw);
  let sn = sin(yaw);
  let rx = dx * cs - dy * sn;
  let ry = dx * sn + dy * cs;
  let x = clamp(sx + rx, 0.0, w);
  let y = clamp(sy + ry, 0.0, h);
  out.pos = vec4<f32>(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  out.color = rgba;
  return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
  return in.color;
}
`,
} as const;
