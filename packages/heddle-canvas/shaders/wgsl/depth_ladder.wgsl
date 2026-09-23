// depth_ladder.wgsl — the Tier-1 Level 2/3 order-book depth ladder.
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
    // Shader-side instance cull: bundles draw `capacity` instances; rows
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
