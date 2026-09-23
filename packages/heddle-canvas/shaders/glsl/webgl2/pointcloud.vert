#version 300 es
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
