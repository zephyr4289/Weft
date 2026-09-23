#version 300 es
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
