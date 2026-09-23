// oscillo_ribbon.wgsl — the Tier-1 waveform ribbon render pass.
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
