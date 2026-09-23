// heddle_shaders.metal — the Tier-1 NATIVE Metal mirror (MSL).
//
// The Metal side of the Pillar 4 tier matrix: the same decimation kernel
// and the same word-addressed row layouts as the WGSL/GLSL/Vulkan
// variants. Apple-gated like WeftMetalZeroCopy.swift (Series 10): the
// source ships in-tree and is compiled by the apple CI leg; on this
// linux sandbox it is a DECLARED mirror (D-42 §6 lists the on-hardware
// checklist) — the Vulkan twin (vk/osc_decimate.comp) is the MEASURED
// native leg here.

#include <metal_stdlib>
using namespace metal;

// The WHP1 params block (one uniform per dispatch, 32 B).
struct HedParams {
  uint sampleCount;
  uint columnCount;
  uint columnBucket;
  uint windowStart;
  uint capacity;
  uint viewportW;
  uint viewportH;
  uint reserved;
};

// The reference window walk (RFC-0022 §4.2) — identical to the WGSL
// compute pass, the GLSL TF pass, the Vulkan .comp and the CPU oracle.
kernel void osc_decimate(
    device const float* samples [[buffer(0)]],
    device float2* minmax [[buffer(1)]],
    constant HedParams& p [[buffer(2)]],
    uint gid [[thread_position_in_grid]]) {
  if (gid >= p.columnCount) { return; }
  uint vis = min(p.sampleCount, p.capacity);
  uint j0 = gid * p.columnBucket;
  if (j0 >= vis) {
    minmax[gid] = float2(0.0, 0.0);
    return;
  }
  uint j1 = min(j0 + p.columnBucket, vis);
  uint base = p.windowStart + p.capacity - vis;
  float lo = 3.402823466e38;
  float hi = -3.402823466e38;
  for (uint j = j0; j < j1; j++) {
    uint slot = (base + j) % p.capacity;
    float v = samples[slot];
    lo = min(lo, v);
    hi = max(hi, v);
  }
  minmax[gid] = float2(lo, hi);
}

// The depth-ladder vertex pass — 64-B word-addressed rows, one quad per
// instance (the Metal analogue of depth_ladder.wgsl).
struct HedRowParams {
  uint rowCount;
  uint viewportW;
  uint viewportH;
  uint reserved;
};

struct RasterOut {
  float4 position [[position]];
  float4 color;
};

static inline float hedValueToY(float v, float h, float yScale, float yOff) {
  return h - 1.0 - (v * yScale + yOff);
}

vertex RasterOut depth_ladder_vert(
    device const uint4* rows [[buffer(0)]],
    constant HedRowParams& p [[buffer(1)]],
    uint vid [[vertex_id]],
    uint r [[instance_id]]) {
  RasterOut out;
  if (r >= p.rowCount) {
    out.position = float4(-2.0, -2.0, 0.0, 1.0);
    out.color = float4(0.0);
    return out;
  }
  float w = float(p.viewportW);
  float h = float(p.viewportH);
  device const uint4& row = rows[r * 4]; // 4 x uint4 per 64-B row
  float price = as_type<float>(row.x);
  float size = as_type<float>(row.y);
  uint side = row.z;
  float bandH = h / float(max(p.rowCount, 1u));
  float y0 = float(r) * bandH;
  float y1 = y0 + max(bandH - 1.0, 1.0);
  float xEnd = max(2.0, size * (w - 4.0));
  float x = 0.0;
  float y = y0;
  if (vid == 1 || vid == 3) x = xEnd;
  if (vid == 2 || vid == 3) y = y1;
  out.position = float4(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  out.color = side == 0 ? float4(0.239, 0.482, 0.839, 1.0)
                        : float4(0.839, 0.529, 0.239, 1.0);
  return out;
}

fragment float4 depth_ladder_frag(RasterOut in [[stage_in]]) {
  return in.color;
}
