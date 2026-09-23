// pointcloud.wgsl — the Tier-1 3D telemetry / point-cloud render pass.
//
// One instanced quad per point: position.xyz (+ size in .w) from the row's
// first vec4, orientation from the quaternion vec4 (the IMU attitude), rgba
// from the third vec4. A minimal perspective projection + the quaternion
// rotates the sprite in screen space (yaw component) so attitude is VISIBLE
// without a full 3D pipeline — the mandate's "low-overhead" point cloud.
//
// PointRow128 layout (RFC-0022 §3.6, Law 2 — word-addressed):
//   w[0..3]  = pos.xyz + size (f32)
//   w[4..7]  = quaternion xyzw (f32)
//   w[8..11] = rgba (f32)

struct RowParams {
  a: vec4<u32>, // (pointCount, 0, 0, 0)
  b: vec4<u32>, // (viewportW, viewportH, 0, 0)
};

@group(0) @binding(0) var<storage, read> rows: array<vec4<u32>>;
@group(0) @binding(1) var<uniform> params: RowParams;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @builtin(instance_index) p: u32) -> VsOut {
  var out: VsOut;
  let pointCount = params.a.x;
  if (p >= pointCount) {
    out.pos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
    out.color = vec4<f32>(0.0);
    return out;
  }
  let w = f32(params.b.x);
  let h = f32(params.b.y);
  let row = p * 8u; // 8 vec4 words per 128-B row
  let pos = vec3<f32>(bitcast<f32>(rows[row].x), bitcast<f32>(rows[row].y), bitcast<f32>(rows[row].z));
  let size = bitcast<f32>(rows[row].w);
  let quat = vec4<f32>(bitcast<f32>(rows[row + 1u].x), bitcast<f32>(rows[row + 1u].y),
                       bitcast<f32>(rows[row + 1u].z), bitcast<f32>(rows[row + 1u].w));
  let rgba = vec4<f32>(bitcast<f32>(rows[row + 2u].x), bitcast<f32>(rows[row + 2u].y),
                       bitcast<f32>(rows[row + 2u].z), bitcast<f32>(rows[row + 2u].w));
  // Minimal perspective: camera at +z, fov ~60 degrees.
  let zc = pos.z + 2.6; // push the unit torus in front of the camera
  let zGuard = max(zc, 0.15);
  let px = pos.x / zGuard;
  let py = pos.y / zGuard;
  let sx = (px * 0.5 + 0.5) * w; // screen x (top-left origin)
  let sy = (0.5 - py * 0.5) * h; // screen y
  // Sprite half-extent in pixels (size in world units -> screen).
  let halfPx = max(1.0, size / zGuard * h * 0.5);
  // Quad corners from vertex_index (0..3, triangle strip).
  var dx = -halfPx;
  var dy = -halfPx;
  if (vi == 1u || vi == 3u) { dx = halfPx; }
  if (vi == 2u || vi == 3u) { dy = halfPx; }
  // Yaw from the quaternion (rotation about z) — attitude made visible.
  let yaw = 2.0 * atan2(quat.z, quat.w);
  let cs = cos(yaw);
  let sn = sin(yaw);
  let rx = dx * cs - dy * sn;
  let ry = dx * sn + dy * cs;
  let x = clamp(sx + rx, 0.0, w);
  let y = clamp(sy + ry, 0.0, h);
  out.pos = vec4<f32>(x / w * 2.0 - 1.0, 1.0 - y / h * 2.0, 0.0, 1.0);
  out.color = rgba;
  return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
  return in.color;
}
