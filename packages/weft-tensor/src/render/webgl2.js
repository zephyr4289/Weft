// render/webgl2.js — WebGL2 plane: ring bytes -> GPU texture -> screen.
//
// ONE texture + ONE fullscreen-triangle program, built at attach (Law 1).
// Per frame: texSubImage2D reads the slot's preallocated Uint8Array view
// DIRECTLY (WebGL2 srcOffset-in-elements signature) — the upload path never
// allocates and never leaves ring memory until the driver's DMA reads it.
// This is the 120/240 FPS path for AI overlays and segmentation masks.

import { LayoutError, DLPackCode } from '../layout.js';

const VERT = `#version 300 es
precision highp float;
const vec2 P[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));
out vec2 vUV;
void main() {
  vec2 p = P[gl_VertexID];
  vUV = p * 0.5 + 0.5;
  vUV.y = 1.0 - vUV.y;               // ring rows are top-down
  gl_Position = vec4(p, 0.0, 1.0);
}`;

const FRAG = `#version 300 es
precision highp float;
uniform sampler2D uTex;
in vec2 vUV;
out vec4 oColor;
void main() { oColor = texture(uTex, vUV); }`;

function compile(gl, type, src) {
  const sh = gl.createShader(type);
  gl.shaderSource(sh, src);
  gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(sh);
    gl.deleteShader(sh);
    throw new LayoutError('WTR1_GL_SHADER', `shader compile failed: ${log}`);
  }
  return sh;
}

export class WebGL2Plane {
  /**
   * @param {HTMLCanvasElement|OffscreenCanvas} canvas
   * @param {import('../ring.js').WeftTensorRing} ring u8 RGBA ring
   * @param {object} [opts] { width, height }
   */
  constructor(canvas, ring, opts = {}) {
    const { code, bits } = ring.layout.dtype;
    if (code !== DLPackCode.UINT || bits !== 8) {
      throw new LayoutError('WTR1_RENDER_DTYPE', 'WebGL2Plane needs a u8 RGBA ring');
    }
    const gl = canvas.getContext('webgl2', {
      alpha: false, antialias: false, depth: false, stencil: false,
      desynchronized: true, preserveDrawingBuffer: false,
    });
    if (gl === null) throw new LayoutError('WTR1_NO_GL', 'webgl2 context unavailable');
    this._gl = gl;
    this._canvas = canvas;
    const L = ring.layout;
    const w = opts.width ?? L.shape[L.rank - 2];
    const h = opts.height ?? L.shape[L.rank - 3];
    if (!w || !h) throw new LayoutError('WTR1_RENDER_SHAPE', 'cannot infer texture size — pass {width, height}');
    this._w = w; this._h = h;
    this._ring = ring;

    // Program (once).
    const prog = gl.createProgram();
    const vs = compile(gl, gl.VERTEX_SHADER, VERT);
    const fs = compile(gl, gl.FRAGMENT_SHADER, FRAG);
    gl.attachShader(prog, vs); gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      throw new LayoutError('WTR1_GL_LINK', `program link failed: ${gl.getProgramInfoLog(prog)}`);
    }
    gl.deleteShader(vs); gl.deleteShader(fs);
    gl.useProgram(prog);
    this._prog = prog;

    // Texture (once) with initial allocation.
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    this._tex = tex;

    this.stats = { draws: 0, frames: 0, uploads: 0 };
    Object.seal(this.stats);
  }

  get width() { return this._w; }
  get height() { return this._h; }

  /** Draw one acquired frame; null keeps the last texture on screen. */
  draw(frame) {
    const gl = this._gl;
    if (frame !== null && frame !== undefined) {
      gl.bindTexture(gl.TEXTURE_2D, this._tex);
      // Zero-alloc upload: slot's preallocated Uint8Array + srcOffset 0.
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, this._w, this._h,
        gl.RGBA, gl.UNSIGNED_BYTE, frame.payloadView(), 0);
      this.stats.uploads++;
      this.stats.frames++;
    }
    gl.viewport(0, 0, this._canvas.width, this._canvas.height);
    gl.useProgram(this._prog);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    this.stats.draws++;
    return this.stats.draws;
  }

  dispose() {
    const gl = this._gl;
    gl.deleteTexture(this._tex);
    gl.deleteProgram(this._prog);
  }
}
