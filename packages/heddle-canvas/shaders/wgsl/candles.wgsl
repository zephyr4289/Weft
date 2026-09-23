// candles.wgsl — the Tier-1 OHLC candlestick render pass.
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
    // Shader-side instance cull (bundles draw `capacity` instances).
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
