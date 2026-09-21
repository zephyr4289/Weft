// viewer-engine.js — WebGL2 point cloud engine + Canvas2D attitude
// indicator engines. Framework-free: the React factories (react.js) and
// the demos (examples/adapters) drive the SAME objects.
//
// Zero-GC frame path (Law 2 / W4-02):
//   - PointCloudEngine: ONE preallocated VBO sized to the arena capacity.
//     Each frame uploads the ring's Float32Array window straight into the
//     GPU (bufferSubData from a typed array — NO intermediate JS objects,
//     no per-point allocation, 100k+ points at display refresh).
//   - AttitudeEngine: Canvas2D horizon drawn with reused primitives;
//     zero allocation per frame (no gradients/paths constructed hot).
//
// Transparent degradation (W4-04): context loss / bad geometry flips the
// engine to FALLBACK (returns false, banner text mutated by the caller).

export const PC_VERT = `#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMvp;
out vec3 vColor;
void main() {
  vColor = aColor;
  gl_Position = uMvp * vec4(aPos, 1.0);
  gl_PointSize = 2.0;
}`;

export const PC_FRAG = `#version 300 es
precision mediump float;
in vec3 vColor;
out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); }`;

function compile(gl, type, src) {
  const sh = gl.createShader(type);
  gl.shaderSource(sh, src);
  gl.compileShader(sh);
  if (gl.getShaderParameter(sh, gl.COMPILE_STATUS) !== true) {
    gl.deleteShader(sh);
    return null;
  }
  return sh;
}

const CAPACITY_POINTS = 262144; // 262144 * 6 f32 = 6 MiB VBO (xyz + rgb)

export class PointCloudEngine {
  constructor(gl, opts) {
    this.gl = gl;
    this.capacity = (opts && opts.capacityPoints) || CAPACITY_POINTS;
    this.pointsDrawn = 0;
    this.frames = 0;
    this.ok = false;
    const vs = compile(gl, gl.VERTEX_SHADER, PC_VERT);
    const fs = compile(gl, gl.FRAGMENT_SHADER, PC_FRAG);
    if (vs === null || fs === null) return;
    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (gl.getProgramParameter(prog, gl.LINK_STATUS) !== true) return;
    this.prog = prog;
    this.uMvp = gl.getUniformLocation(prog, 'uMvp');
    this.vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, this.capacity * 6 * 4, gl.STREAM_DRAW);
    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 24, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 24, 12);
    gl.bindVertexArray(null);
    gl.enable(gl.DEPTH_TEST);
    this.ok = true;
  }

  /** Upload + draw one POINTS_F32 window (Float32Array over ring bytes).
   *  Colors are derived in-shader-free: interleaved layout is x,y,z,r,g,b —
   *  producers pack color alongside (demo) or a vertex pull key; the
   *  managed contract is: the window IS the draw source (no copies). */
  draw(f32, pointCount, mvp) {
    if (this.ok !== true) return false;
    const gl = this.gl;
    const n = Math.min(pointCount, this.capacity);
    gl.useProgram(this.prog);
    gl.uniformMatrix4fv(this.uMvp, false, mvp);
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, f32, 0, n * 6);
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.clearColor(0.04, 0.05, 0.07, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.drawArrays(gl.POINTS, 0, n);
    gl.bindVertexArray(null);
    this.pointsDrawn = n;
    this.frames++;
    return true;
  }

  onContextLost() {
    this.ok = false; // W4-04: degrade, never throw
  }
}

/** Simple perspective * lookAt mvp into `out` (16 f32). Deterministic. */
export function makeMvp(out, az, el, dist) {
  const ce = Math.cos(el), se = Math.sin(el);
  const ca = Math.cos(az), sa = Math.sin(az);
  // eye
  const ex = dist * ce * sa, ey = dist * se, ez = dist * ce * ca;
  // forward / right / up
  let fx = -ex, fy = -ey, fz = -ez;
  const fl = Math.hypot(fx, fy, fz);
  fx /= fl; fy /= fl; fz /= fl;
  let rx = fy * 0 - fz * 1, ry = fz * 0 - fx * 0, rz = fx * 1 - fy * 0;
  const rl = Math.hypot(rx, ry, rz) || 1;
  rx /= rl; ry /= rl; rz /= rl;
  const ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
  // view (lookAt) * perspective(60deg, 1, 0.1, 1000)
  const f = 1 / Math.tan(Math.PI / 6);
  const m = out;
  m[0] = rx * f; m[1] = ux * f; m[2] = -fx; m[3] = 0;
  m[4] = ry * f; m[5] = uy * f; m[6] = -fy; m[7] = 0;
  m[8] = rz * f; m[9] = uz * f; m[10] = -fz; m[11] = 0;
  m[12] = -(rx * ex + ry * ey + rz * ez) * f;
  m[13] = -(ux * ex + uy * ey + uz * ez) * f;
  m[14] = (fx * ex + fy * ey + fz * ez);
  m[15] = 1;
  return out;
}

/** Attitude indicator engine (Canvas2D artificial horizon). */
export class AttitudeEngine {
  constructor(ctx) {
    this.ctx = ctx;
    this.frames = 0;
    this.ok = ctx !== null && ctx !== undefined &&
      typeof ctx.fillRect === 'function';
  }

  /** quat = [qw, qx, qy, qz]; draws roll/pitch horizon. */
  draw(qw, qx, qy, qz, w, h) {
    if (this.ok !== true) return false;
    const ctx = this.ctx;
    // yaw (unused for the instrument) + roll + pitch extraction
    const roll = Math.atan2(2 * (qw * qz + qx * qy), 1 - 2 * (qy * qy + qz * qz));
    const pitch = Math.asin(Math.max(-1, Math.min(1, 2 * (qw * qx - qy * qz))));
    const cx = w / 2, cy = h / 2, r = Math.min(w, h) * 0.45;
    ctx.save();
    ctx.clearRect(0, 0, w, h);
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.clip();
    ctx.translate(cx, cy);
    ctx.rotate(-roll);
    const off = (pitch / (Math.PI / 2)) * r * 2;
    ctx.fillStyle = '#1e78c8';
    ctx.fillRect(-r, -r - off, 2 * r, 2 * r);       // sky
    ctx.fillStyle = '#7a4a1f';
    ctx.fillRect(-r, off, 2 * r, 2 * r);            // ground
    ctx.strokeStyle = '#ffffff';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(-r, off); ctx.lineTo(r, off); ctx.stroke(); // horizon
    ctx.restore();
    // fixed aircraft symbol
    ctx.strokeStyle = '#ffd60a';
    ctx.lineWidth = 3;
    ctx.beginPath();
    ctx.moveTo(cx - r * 0.5, cy); ctx.lineTo(cx - r * 0.15, cy);
    ctx.moveTo(cx + r * 0.15, cy); ctx.lineTo(cx + r * 0.5, cy);
    ctx.moveTo(cx, cy - r * 0.08); ctx.lineTo(cx, cy + r * 0.08);
    ctx.stroke();
    // bezel
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.lineWidth = 2;
    ctx.strokeStyle = '#333333';
    ctx.stroke();
    this.frames++;
    return true;
  }
}
