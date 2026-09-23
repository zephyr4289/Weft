#version 300 es
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
