// glsl_sources.ts — the GLSL 300 es bundle (Tier 2).
//
// GENERATED from shaders/glsl/webgl2/* by scripts/emit_shaders.mjs — DO NOT EDIT HERE.
// The drift gate (test/shaders.test.ts) re-emits and diffs; edit the
// canonical file instead.

export const GLSL_SOURCES = {
  oscilloDecimateVert: `#version 300 es
// oscillo_decimate_tf.vert — the Tier-2 min/max decimation pass.
//
// WebGL2 has no compute shaders: the reduction runs as a TRANSFORM-
// FEEDBACK vertex pass (RASTERIZER_DISCARD, one POINT per column,
// gl_VertexID addressing, output captured into the interleaved minmax
// buffer). It implements the SAME reference window walk as the WGSL
// compute pass and the CPU oracle (RFC-0022 §4.2) — bit-exact by
// construction (order-independent min/max comparisons).
//
// Samples live in an R32F texture (the WebGL2 large-buffer road); slot s
// maps to texel (s % texW, s / texW). Uniforms carry the window:
//   uSampleCount  valid samples = min(writePos, capacity)
//   uWindowStart  writePos % capacity
//   uCapacity     lane capacity (ring modulus)
//   uColumnBucket ceil(vis / cols)

precision highp float;
precision highp int;
precision highp sampler2D;

uniform uvec2 uTexDims;
uniform uint uSampleCount;
uniform uint uColumnBucket;
uniform uint uWindowStart;
uniform uint uCapacity;
uniform sampler2D uSamples;

out vec2 vMinMax; // transform-feedback varying (INTERLEAVED_ATTRIBS)

void main() {
  uint c = uint(gl_VertexID);
  uint vis = min(uSampleCount, uCapacity);
  uint j0 = c * uColumnBucket;
  if (j0 >= vis) {
    vMinMax = vec2(0.0, 0.0); // tail-column sentinel (same as CPU/WGSL)
    return;
  }
  uint j1 = min(j0 + uColumnBucket, vis);
  uint base = uWindowStart + uCapacity - vis;
  float lo = 3.402823466e38;
  float hi = -3.402823466e38;
  for (uint j = j0; j < j1; j++) {
    uint slot = (base + j) % uCapacity;
    ivec2 tc = ivec2(int(slot % uTexDims.x), int(slot / uTexDims.x));
    float v = texelFetch(uSamples, tc, 0).r;
    lo = min(lo, v);
    hi = max(hi, v);
  }
  vMinMax = vec2(lo, hi);
  gl_Position = vec4(2.0, 2.0, 0.0, 1.0); // off-screen; rasterizer discarded anyway
  gl_PointSize = 1.0;
}
`,
  oscilloDecimateFrag: `#version 300 es
// oscillo_pass.frag — the trivial link-only fragment shader for the
// transform-feedback decimation pass (RASTERIZER_DISCARD means it never
// runs; WebGL2 still requires a fragment stage to link the program).
precision mediump float;
out vec4 oColor;
void main() { oColor = vec4(0.0); }
`,
  oscilloRibbonVert: `#version 300 es
// oscillo_ribbon.vert — the Tier-2 waveform ribbon (min/max bands).
// One instanced quad per column over the TF minmax buffer; the projection
// matches the WGSL ribbon and the Canvas2D raster (yScale 0.9, yOff 0.05).
precision highp float;

uniform vec2 uViewport;
uniform uint uColumnCount;

layout(location = 0) in vec2 aMinMax; // per-instance (divisor 1)

out vec4 vColor;

void main() {
  float w = uViewport.x;
  float h = uViewport.y;
  float cols = float(uColumnCount);
  float colW = w / max(cols, 1.0);
  uint c = uint(gl_InstanceID);
  uint vi = uint(gl_VertexID);
  float x0 = float(c) * colW;
  float x1 = x0 + colW;
  float yScale = (h - 1.0) * 0.9;
  float yOff = (h - 1.0) * 0.05;
  float yLo = clamp(h - 1.0 - (aMinMax.y * yScale + yOff), 0.0, h - 1.0);
  float yHi = clamp(h - 1.0 - (aMinMax.x * yScale + yOff), 0.0, h - 1.0);
  float x = x0;
  float y = yLo;
  if (vi == 1u || vi == 3u) x = x1;
  if (vi == 2u || vi == 3u) y = yHi;
  gl_Position = vec4(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  vColor = vec4(0.184, 0.749, 0.443, 1.0); // #2fbf71 — the house ribbon green
}
`,
  oscilloRibbonFrag: `#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 oColor;
void main() { oColor = vColor; }
`,
  ladderVert: `#version 300 es
// depth_ladder.vert — the Tier-2 Level 2/3 depth ladder.
// One instanced quad per row; attribs aPrice/aSize/aSide are per-instance
// (divisor 1) at the plane's own 64-B stride (Law 2 — the same words the
// WGSL storage buffer and the CPU raster read).
precision highp float;

uniform vec2 uViewport;
uniform uint uRowCount;

layout(location = 0) in float aPrice;
layout(location = 1) in float aSize;
layout(location = 2) in float aSide;

out vec4 vColor;

void main() {
  float w = uViewport.x;
  float h = uViewport.y;
  uint r = uint(gl_InstanceID);
  uint vi = uint(gl_VertexID);
  float bandH = h / float(max(uRowCount, 1u));
  float y0 = float(r) * bandH;
  float y1 = y0 + max(bandH - 1.0, 1.0);
  float xEnd = max(2.0, aSize * (w - 4.0));
  float x = 0.0;
  float y = y0;
  if (vi == 1u || vi == 3u) x = xEnd;
  if (vi == 2u || vi == 3u) y = y1;
  gl_Position = vec4(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  vColor = aSide < 0.5 ? vec4(0.239, 0.482, 0.839, 1.0)  // bid blue #3d7bd6
                       : vec4(0.839, 0.529, 0.239, 1.0); // ask orange #d6873d
}
`,
  ladderFrag: `#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 oColor;
void main() { oColor = vColor; }
`,
  candleVert: `#version 300 es
// candles.vert — the Tier-2 OHLC candlesticks. One instanced quad per
// candle covering high..low; the fragment stage carves body vs wick
// (same silhouette rule as the WGSL candle pass).
precision highp float;

uniform vec2 uViewport;
uniform uint uRowCount;

layout(location = 0) in vec4 aOhlc;   // open, high, low, close
layout(location = 1) in float aVolume;

out vec2 vLocal;  // xFrac in [0,1] of the column, y in pixels
out vec2 vBodyY;  // pixel extents of the body (open..close)
out float vUpDown;

void main() {
  float w = uViewport.x;
  float h = uViewport.y;
  uint r = uint(gl_InstanceID);
  uint vi = uint(gl_VertexID);
  float yScale = (h - 1.0) * 0.92;
  float yOff = (h - 1.0) * 0.04;
  float colW = w / float(max(uRowCount, 1u));
  float x0 = float(r) * colW;
  float x1 = x0 + colW;
  float yHigh = clamp(h - 1.0 - (aOhlc.y * yScale + yOff), 0.0, h - 1.0);
  float yLow  = clamp(h - 1.0 - (aOhlc.z * yScale + yOff), 0.0, h - 1.0);
  float yOpen = clamp(h - 1.0 - (aOhlc.x * yScale + yOff), 0.0, h - 1.0);
  float yClose= clamp(h - 1.0 - (aOhlc.w * yScale + yOff), 0.0, h - 1.0);
  float x = x0;
  float y = yHigh;
  float xFrac = 0.0;
  if (vi == 1u || vi == 3u) { x = x1; xFrac = 1.0; }
  if (vi == 2u || vi == 3u) y = yLow;
  gl_Position = vec4(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  vLocal = vec2(xFrac, y);
  vBodyY = vec2(min(yOpen, yClose), max(yOpen, yClose));
  vUpDown = aOhlc.w >= aOhlc.x ? 1.0 : -1.0;
}
`,
  candleFrag: `#version 300 es
// candles.frag — body/wick silhouette carving (see candles.vert).
precision mediump float;
in vec2 vLocal;
in vec2 vBodyY;
in float vUpDown;
out vec4 oColor;
void main() {
  bool wick = abs(vLocal.x - 0.5) < 0.06;
  bool inBodyX = abs(vLocal.x - 0.5) < 0.35;
  bool inBodyY = vLocal.y >= vBodyY.x && vLocal.y <= vBodyY.y;
  if (!wick && !(inBodyX && inBodyY)) discard;
  oColor = vUpDown > 0.0 ? vec4(0.184, 0.749, 0.443, 1.0)  // #2fbf71
                         : vec4(0.835, 0.314, 0.314, 1.0); // #d65050
}
`,
  pointcloudVert: `#version 300 es
// pointcloud.vert — the Tier-2 3D telemetry point cloud. One instanced
// quad per point (pos.xyz+size, quaternion, rgba at the plane's own
// 128-B stride); minimal perspective + quaternion-yaw sprite rotation.
precision highp float;

uniform vec2 uViewport;

layout(location = 0) in vec4 aPosSize; // pos.xyz + size
layout(location = 1) in vec4 aQuat;    // xyzw
layout(location = 2) in vec4 aColor;   // rgba

out vec4 vColor;

void main() {
  float w = uViewport.x;
  float h = uViewport.y;
  uint vi = uint(gl_VertexID);
  float zc = aPosSize.z + 2.6;
  float zg = max(zc, 0.15);
  float px = aPosSize.x / zg;
  float py = aPosSize.y / zg;
  float sx = (px * 0.5 + 0.5) * w;
  float sy = (0.5 - py * 0.5) * h;
  float halfPx = max(1.0, aPosSize.w / zg * h * 0.5);
  float dx = -halfPx;
  float dy = -halfPx;
  if (vi == 1u || vi == 3u) dx = halfPx;
  if (vi == 2u || vi == 3u) dy = halfPx;
  float yaw = 2.0 * atan(aQuat.z, aQuat.w);
  float cs = cos(yaw);
  float sn = sin(yaw);
  float rx = dx * cs - dy * sn;
  float ry = dx * sn + dy * cs;
  float x = clamp(sx + rx, 0.0, w);
  float y = clamp(sy + ry, 0.0, h);
  gl_Position = vec4(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  vColor = aColor;
}
`,
  pointcloudFrag: `#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 oColor;
void main() { oColor = vColor; }
`,
} as const;
