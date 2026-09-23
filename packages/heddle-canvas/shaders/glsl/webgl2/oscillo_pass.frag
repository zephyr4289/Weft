#version 300 es
// oscillo_pass.frag — the trivial link-only fragment shader for the
// transform-feedback decimation pass (RASTERIZER_DISCARD means it never
// runs; WebGL2 still requires a fragment stage to link the program).
precision mediump float;
out vec4 oColor;
void main() { oColor = vec4(0.0); }
