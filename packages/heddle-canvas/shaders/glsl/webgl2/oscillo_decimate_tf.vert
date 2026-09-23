#version 300 es
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
