#version 300 es
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
