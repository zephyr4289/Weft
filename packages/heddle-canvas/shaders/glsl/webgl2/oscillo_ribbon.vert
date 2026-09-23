#version 300 es
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
