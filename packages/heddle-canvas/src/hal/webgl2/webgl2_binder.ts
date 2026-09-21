// webgl2_binder.ts — @weft/heddle-canvas Tier 2: the WebGL2 HAL.
//
// WHY EXISTS: WebGL2 is the mandate's Tier 2 — GLSL 300 es, instanced
// arrays, VBOs — and the tier that ships on effectively every browser
// today. The engineering substance of this backend is not "draw things":
// it is keeping the Pillar 4 laws on an API that was never designed for
// them.
//
//   Law 1 (zero per-frame allocation): every buffer/texture/VAO/program
//     is carved in initialize(); the SAB-view canary decides ONCE whether
//     bufferSubData accepts SharedArrayBuffer views. When the browser
//     accepts (Chromium does — MEASURED in the browser leg), the upload
//     path is the lane's own typed view with (srcOffset, length) — zero
//     intermediate arrays, zero copies on the JS side. When a host
//     refuses, ONE pre-allocated staging buffer per lane carries an
//     element-loop copy, counted in stats.uploadedBytes exactly like the
//     direct road (the copy is reported, never laundered).
//   Law 2 (alignment): row lanes upload at their 64 B / 128 B stride;
//     the min/max transform-feedback buffer is interleaved vec2 (8 B
//     columns); vertex attribs point into the SAME strides Engineer 1's
//     plane already guarantees.
//   Decimation (deliverable B, Tier-2 edition): WebGL2 has NO compute
//     shaders — the min/max column reduction runs as a TRANSFORM-FEEDBACK
//     vertex pass (RASTERIZER_DISCARD, one POINT per column, gl_VertexID
//     addressing, output captured into the minmax buffer). This is the
//     classic WebGL2 GPGPU road and it is bit-exact by construction with
//     the WGSL compute and the Canvas2D CPU reductions (order-independent
//     min/max comparisons — the cross-tier ==-gate).

import type { PlaneView, LaneView } from '../../plane/hot_plane.ts';
import type { RenderHAL } from '../hal.ts';
import { createHalStats, RENDER_TIERS, type EngineConfig, type HalStats } from '../tier.ts';
import { HeddleError, type TierFallbackEvent } from '../../errors.ts';
import { GLContextStateMachine } from './webgl2_context.ts';
import { GLSL_SOURCES } from './glsl_sources.ts';

/** Structural WebGL2 surface (constants are spec-fixed numbers). */
export interface GL2 {
  createBuffer(): unknown;
  bindBuffer(target: number, buffer: unknown): void;
  bufferData(target: number, sizeOrData: number | ArrayBufferView, usage: number): void;
  bufferSubData(
    target: number, dstByteOffset: number,
    srcData: ArrayBufferView, srcOffsetInElements?: number, lengthInElements?: number,
  ): void;
  createTexture(): unknown;
  bindTexture(target: number, texture: unknown): void;
  texImage2D(
    target: number, level: number, internalFormat: number,
    width: number, height: number, border: number,
    format: number, type: number, source?: ArrayBufferView,
  ): void;
  texSubImage2D(
    target: number, level: number, xoffset: number, yoffset: number,
    width: number, height: number, format: number, type: number,
    source?: ArrayBufferView, srcOffsetInElements?: number,
  ): void;
  texParameteri(target: number, pname: number, param: number): void;
  createVertexArray(): unknown;
  bindVertexArray(vao: unknown): void;
  enableVertexAttribArray(index: number): void;
  vertexAttribDivisor(index: number, divisor: number): void;
  vertexAttribPointer(
    index: number, size: number, type: number, normalized: boolean,
    stride: number, offsetInBytes: number,
  ): void;
  createShader(type: number): unknown;
  shaderSource(shader: unknown, source: string): void;
  compileShader(shader: unknown): void;
  getShaderParameter(shader: unknown, pname: number): unknown;
  getShaderInfoLog(shader: unknown): unknown;
  createProgram(): unknown;
  attachShader(program: unknown, shader: unknown): void;
  bindAttribLocation(program: unknown, index: number, name: string): void;
  linkProgram(program: unknown): void;
  getProgramParameter(program: unknown, pname: number): unknown;
  getProgramInfoLog(program: unknown): unknown;
  transformFeedbackVaryings(
    program: unknown, varyings: readonly string[], bufferMode: number,
  ): void;
  useProgram(program: unknown): void;
  getUniformLocation(program: unknown, name: string): unknown;
  uniform1f(location: unknown, x: number): void;
  uniform2f(location: unknown, x: number, y: number): void;
  uniform4f(location: unknown, x: number, y: number, z: number, w: number): void;
  uniform1ui(location: unknown, x: number): void;
  uniform2ui(location: unknown, x: number, y: number): void;
  uniform1i(location: unknown, x: number): void;
  enable(cap: number): void;
  disable(cap: number): void;
  scissor(x: number, y: number, w: number, h: number): void;
  viewport(x: number, y: number, w: number, h: number): void;
  drawArrays(mode: number, first: number, count: number): void;
  drawArraysInstanced(mode: number, first: number, count: number, instanceCount: number): void;
  beginTransformFeedback(mode: number): void;
  endTransformFeedback(): void;
  bindBufferBase(target: number, index: number, buffer: unknown): void;
  activeTexture(unit: number): void;
  getParameter(pname: number): unknown;
  clearColor(r: number, g: number, b: number, a: number): void;
  // Spec-fixed constants (values are stable across all WebGL2 impls).
  readonly ARRAY_BUFFER: number;
  readonly STATIC_DRAW: number;
  readonly DYNAMIC_DRAW: number;
  readonly TRANSFORM_FEEDBACK_BUFFER: number;
  readonly TRANSFORM_FEEDBACK_BUFFER_BINDING: number;
  readonly INTERLEAVED_ATTRIBS: number;
  readonly RASTERIZER_DISCARD: number;
  readonly VERTEX_SHADER: number;
  readonly FRAGMENT_SHADER: number;
  readonly COMPILE_STATUS: number;
  readonly LINK_STATUS: number;
  readonly MAX_TEXTURE_SIZE: number;
  readonly TEXTURE_2D: number;
  readonly TEXTURE_MIN_FILTER: number;
  readonly TEXTURE_MAG_FILTER: number;
  readonly TEXTURE_WRAP_S: number;
  readonly TEXTURE_WRAP_T: number;
  readonly CLAMP_TO_EDGE: number;
  readonly NEAREST: number;
  readonly RED: number;
  readonly R32F: number;
  readonly FLOAT: number;
  readonly POINTS: number;
  readonly TRIANGLE_STRIP: number;
  readonly SCISSOR_TEST: number;
  readonly TEXTURE0: number;
}

export const GL_FLOAT = 0x1406;

/** Attrib locations (bindAttribLocation keeps them stable across links). */
const A_MINMAX = 0;
const A_ROW_PRICE = 0;
const A_ROW_SIZE = 1;
const A_ROW_SIDE = 2;
const A_PC_POSSIZE = 0;
const A_PC_QUAT = 1;
const A_PC_COLOR = 2;
const A_CANDLE_OHLC = 0;
const A_CANDLE_VOL = 1;

interface WaveformLane {
  texture: unknown;
  texW: number;
  texH: number;
  minmaxBuffer: unknown;
  vao: unknown;
  staging: Float32Array | null; // non-null only on the refused-SAB road
}

interface RowLane {
  buffer: unknown;
  vao: unknown;
  staging: Uint32Array | null;
}

export class WebGL2HAL implements RenderHAL {
  readonly tier = RENDER_TIERS.WEBGL2;
  readonly directMapped = false;
  readonly name = 'webgl2';
  readonly stats: HalStats = createHalStats();
  sabViewAccepted = false;

  // LAW1:INIT-BEGIN — everything below is carved in initialize().
  private gl: GL2 | null = null;
  private sm: GLContextStateMachine | null = null;
  private cfg: EngineConfig | null = null;
  private waveforms: WaveformLane[] = [];
  private rows: RowLane[] = [];
  private progDecimate: unknown = null;
  private progRibbon: unknown = null;
  private progLadder: unknown = null;
  private progPointcloud: unknown = null;
  private progCandle: unknown = null;
  private uDecimate: Record<string, unknown> = {};
  private uRibbon: Record<string, unknown> = {};
  private uLadder: Record<string, unknown> = {};
  private uPointcloud: Record<string, unknown> = {};
  private uCandle: Record<string, unknown> = {};
  private frameOpen = false;
  private dirtyX0 = 0; private dirtyY0 = 0;
  private dirtyX1 = 0; private dirtyY1 = 0;
  private dirtyEmpty = true;
  // LAW1:INIT-END

  constructor(gl: GL2, stateMachine: GLContextStateMachine) {
    this.gl = gl;
    this.sm = stateMachine;
  }

  initialize(plane: PlaneView, cfg: EngineConfig): void {
    const gl = this.requireGL();
    this.cfg = cfg;
    this.waveforms = [];
    this.rows = [];

    // --- the SAB-view canary: one 4-byte probe decides the upload road ---
    this.sabViewAccepted = this.probeSabView(gl, plane.sab);

    // --- programs (compile failures are named Law-4 refusals) -----------
    this.progDecimate = this.buildProgram(
      gl, 'oscillo_decimate_tf', GLSL_SOURCES.oscilloDecimateVert, GLSL_SOURCES.oscilloDecimateFrag,
      ['vMinMax'],
    );
    this.progRibbon = this.buildProgram(
      gl, 'oscillo_ribbon', GLSL_SOURCES.oscilloRibbonVert, GLSL_SOURCES.oscilloRibbonFrag,
    );
    this.progLadder = this.buildProgram(
      gl, 'depth_ladder', GLSL_SOURCES.ladderVert, GLSL_SOURCES.ladderFrag,
    );
    this.progPointcloud = this.buildProgram(
      gl, 'pointcloud', GLSL_SOURCES.pointcloudVert, GLSL_SOURCES.pointcloudFrag,
    );
    this.progCandle = this.buildProgram(
      gl, 'candles', GLSL_SOURCES.candleVert, GLSL_SOURCES.candleFrag,
    );

    const maxTex = gl.getParameter(gl.MAX_TEXTURE_SIZE) as number;
    if (typeof maxTex !== 'number' || maxTex < 256) {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        `webgl2 hal: MAX_TEXTURE_SIZE=${maxTex} unusable`,
      );
    }

    // --- per-lane resources ----------------------------------------------
    for (const lane of plane.lanes) {
      if (lane.kind === 0) {
        // WAVEFORM: r32f texture (sample store) + TF minmax buffer + VAO.
        const texW = Math.min(maxTex, 4096);
        const texH = Math.ceil(lane.capacity / texW);
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texImage2D(
          gl.TEXTURE_2D, 0, gl.R32F, texW, texH, 0,
          gl.RED, gl.FLOAT, lane.f32,
        );
        const minmaxBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, minmaxBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, cfg.columnCount * 8, gl.DYNAMIC_DRAW);
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, minmaxBuffer);
        gl.enableVertexAttribArray(A_MINMAX);
        gl.vertexAttribPointer(A_MINMAX, 2, GL_FLOAT, false, 8, 0);
        gl.vertexAttribDivisor(A_MINMAX, 1);
        gl.bindVertexArray(null);
        this.waveforms[lane.laneIndex] = {
          texture, texW, texH, minmaxBuffer, vao,
          staging: this.sabViewAccepted ? null : new Float32Array(lane.capacity),
        };
      } else {
        // Row lane: instance buffer at the plane's own stride (Law 2).
        const buffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
        gl.bufferData(gl.ARRAY_BUFFER, lane.capacity * lane.strideBytes, gl.DYNAMIC_DRAW);
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
        const stride = lane.strideBytes;
        if (lane.kind === 1) {
          // DEPTH_LADDER — 64-B rows: price, size, side.
          gl.enableVertexAttribArray(A_ROW_PRICE);
          gl.vertexAttribPointer(A_ROW_PRICE, 1, GL_FLOAT, false, stride, 0);
          gl.vertexAttribDivisor(A_ROW_PRICE, 1);
          gl.enableVertexAttribArray(A_ROW_SIZE);
          gl.vertexAttribPointer(A_ROW_SIZE, 1, GL_FLOAT, false, stride, 4);
          gl.vertexAttribDivisor(A_ROW_SIZE, 1);
          gl.enableVertexAttribArray(A_ROW_SIDE);
          gl.vertexAttribPointer(A_ROW_SIDE, 1, GL_FLOAT, false, stride, 8);
          gl.vertexAttribDivisor(A_ROW_SIDE, 1);
        } else if (lane.kind === 2) {
          // CANDLE_OHLC — 64-B rows: OHLC vec4 + volume.
          gl.enableVertexAttribArray(A_CANDLE_OHLC);
          gl.vertexAttribPointer(A_CANDLE_OHLC, 4, GL_FLOAT, false, stride, 0);
          gl.vertexAttribDivisor(A_CANDLE_OHLC, 1);
          gl.enableVertexAttribArray(A_CANDLE_VOL);
          gl.vertexAttribPointer(A_CANDLE_VOL, 1, GL_FLOAT, false, stride, 16);
          gl.vertexAttribDivisor(A_CANDLE_VOL, 1);
        } else {
          // POINTCLOUD_QUAT — 128-B rows: pos+size, quat, color.
          gl.enableVertexAttribArray(A_PC_POSSIZE);
          gl.vertexAttribPointer(A_PC_POSSIZE, 4, GL_FLOAT, false, stride, 0);
          gl.vertexAttribDivisor(A_PC_POSSIZE, 1);
          gl.enableVertexAttribArray(A_PC_QUAT);
          gl.vertexAttribPointer(A_PC_QUAT, 4, GL_FLOAT, false, stride, 16);
          gl.vertexAttribDivisor(A_PC_QUAT, 1);
          gl.enableVertexAttribArray(A_PC_COLOR);
          gl.vertexAttribPointer(A_PC_COLOR, 4, GL_FLOAT, false, stride, 32);
          gl.vertexAttribDivisor(A_PC_COLOR, 1);
        }
        gl.bindVertexArray(null);
        this.rows[lane.laneIndex] = {
          buffer, vao,
          staging: this.sabViewAccepted ? null : new Uint32Array(lane.capacity * (stride >> 2)),
        };
      }
    }

    // Uniform locations resolved ONCE (never per frame).
    this.cacheUniforms(gl);

    gl.viewport(0, 0, cfg.canvasWidth, cfg.canvasHeight);
    gl.clearColor(12 / 255, 12 / 255, 16 / 255, 1);

    // Restoration contract: on webglcontextrestored the whole surface is
    // re-carved by re-running THIS method (the sm calls us back).
    this.sm?.onRestore(() => {
      if (this.cfg !== null) this.initialize(plane, this.cfg);
    });
  }

  handleLoss(reason: string): TierFallbackEvent | null {
    // The state machine keeps the restore window open; the LADDER decides
    // whether to wait or degrade. We report the honest transition.
    return {
      from: this.name, to: 'canvas2d', code: 'HC_E_CONTEXT_LOST',
      detail: `webgl2 context lost (${reason}); restore pending via WEBGL_lose_context protocol`,
    };
  }

  // --- frame hot path ------------------------------------------------------

  beginFrame(): void {
    const gl = this.requireGL();
    if (this.sm !== null && !this.sm.canDraw()) {
      throw new HeddleError(
        'HC_E_CONTEXT_LOST',
        'beginFrame on a lost webgl2 context (ladder should have degraded)',
      );
    }
    this.frameOpen = true;
    this.dirtyEmpty = true;
    this.dirtyX0 = 0; this.dirtyY0 = 0; this.dirtyX1 = 0; this.dirtyY1 = 0;
    gl.disable(gl.SCISSOR_TEST);
  }

  uploadWaveform(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    const gl = this.requireGL();
    const wf = this.waveforms[lane.laneIndex];
    if (wf === undefined) {
      throw new HeddleError(
        'HC_E_LANE_UNSUPPORTED_KIND',
        `uploadWaveform on non-waveform lane ${lane.laneIndex}`,
      );
    }
    const count = elemEndExcl - elemStart;
    if (this.sabViewAccepted) {
      // Zero-copy road: the lane's OWN view, element-offset addressing.
      // A range spanning texture rows splits at row boundaries — each
      // texSubImage2D still slices NOTHING (srcOffset advances instead).
      let remaining = count;
      let elem = elemStart;
      while (remaining > 0) {
        const x = elem % wf.texW;
        const y = (elem / wf.texW) | 0;
        const w = Math.min(wf.texW - x, remaining);
        gl.bindTexture(gl.TEXTURE_2D, wf.texture);
        gl.texSubImage2D(
          gl.TEXTURE_2D, 0, x, y, w, 1,
          gl.RED, gl.FLOAT, lane.f32, elem,
        );
        elem += w;
        remaining -= w;
      }
    } else {
      // Staging road: element-loop copy into the pre-allocated mirror.
      const staging = wf.staging as Float32Array;
      for (let i = 0; i < count; i++) staging[elemStart + i] = lane.f32[elemStart + i];
      let remaining = count;
      let elem = elemStart;
      while (remaining > 0) {
        const x = elem % wf.texW;
        const y = (elem / wf.texW) | 0;
        const w = Math.min(wf.texW - x, remaining);
        gl.bindTexture(gl.TEXTURE_2D, wf.texture);
        gl.texSubImage2D(
          gl.TEXTURE_2D, 0, x, y, w, 1,
          gl.RED, gl.FLOAT, staging, elem,
        );
        elem += w;
        remaining -= w;
      }
    }
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += count * lane.strideBytes;
    const cfg = this.requireCfg();
    const colScale = Math.min(cfg.columnCount, cfg.canvasWidth) / lane.capacity;
    const c0 = Math.floor(elemStart * colScale);
    const c1 = Math.ceil(elemEndExcl * colScale);
    this.expandDirty(c0, 0, c1, cfg.canvasHeight);
  }

  uploadRows(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    const gl = this.requireGL();
    const row = this.rows[lane.laneIndex];
    if (row === undefined) {
      throw new HeddleError(
        'HC_E_LANE_UNSUPPORTED_KIND',
        `uploadRows on waveform lane ${lane.laneIndex}`,
      );
    }
    const wordsPerRow = lane.strideBytes >> 2;
    const firstWord = elemStart * wordsPerRow;
    const wordCount = (elemEndExcl - elemStart) * wordsPerRow;
    gl.bindBuffer(gl.ARRAY_BUFFER, row.buffer);
    if (this.sabViewAccepted) {
      gl.bufferSubData(
        gl.ARRAY_BUFFER, firstWord * 4,
        lane.u32, firstWord, wordCount,
      );
    } else {
      const staging = row.staging as Uint32Array;
      for (let i = 0; i < wordCount; i++) staging[firstWord + i] = lane.u32[firstWord + i];
      gl.bufferSubData(
        gl.ARRAY_BUFFER, firstWord * 4,
        staging, firstWord, wordCount,
      );
    }
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += (elemEndExcl - elemStart) * lane.strideBytes;
    this.expandDirty(0, 0, this.requireCfg().canvasWidth, this.requireCfg().canvasHeight);
  }

  drawOscillo(lane: LaneView, elemCount: number, windowStart: number): void {
    const gl = this.requireGL();
    const cfg = this.requireCfg();
    const wf = this.waveforms[lane.laneIndex];
    const cols = Math.min(cfg.columnCount, cfg.canvasWidth);
    const bucket = Math.max(1, Math.ceil(elemCount / cols));

    // Pass 1: TF decimation (RASTERIZER_DISCARD, one POINT per column).
    gl.useProgram(this.progDecimate);
    gl.uniform2ui(this.uDecimate['uTexDims'], wf.texW, wf.texH);
    gl.uniform1ui(this.uDecimate['uSampleCount'], elemCount);
    gl.uniform1ui(this.uDecimate['uColumnBucket'], bucket);
    gl.uniform1ui(this.uDecimate['uWindowStart'], windowStart);
    gl.uniform1ui(this.uDecimate['uCapacity'], lane.capacity);
    gl.uniform1i(this.uDecimate['uSamples'], 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, wf.texture);
    gl.enable(gl.RASTERIZER_DISCARD);
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, wf.minmaxBuffer);
    gl.beginTransformFeedback(gl.POINTS);
    gl.drawArrays(gl.POINTS, 0, cols);
    gl.endTransformFeedback();
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, null);
    gl.disable(gl.RASTERIZER_DISCARD);

    // Pass 2: the ribbon — instanced over the minmax buffer.
    gl.useProgram(this.progRibbon);
    gl.uniform2f(this.uRibbon['uViewport'], cfg.canvasWidth, cfg.canvasHeight);
    gl.uniform1ui(this.uRibbon['uColumnCount'], cols);
    gl.uniform4f(this.uRibbon['uColor'], 0.184, 0.749, 0.443, 1.0);
    gl.bindVertexArray(wf.vao);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, cols);
    gl.bindVertexArray(null);
    this.stats.drawCalls += 2;
  }

  drawDepthLadder(lane: LaneView, rowCount: number): void {
    const gl = this.requireGL();
    const cfg = this.requireCfg();
    const row = this.rows[lane.laneIndex];
    gl.useProgram(this.progLadder);
    gl.uniform2f(this.uLadder['uViewport'], cfg.canvasWidth, cfg.canvasHeight);
    gl.uniform1ui(this.uLadder['uRowCount'], rowCount);
    gl.bindVertexArray(row.vao);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, rowCount);
    gl.bindVertexArray(null);
    this.stats.drawCalls++;
  }

  drawPointcloud(lane: LaneView, pointCount: number): void {
    const gl = this.requireGL();
    const cfg = this.requireCfg();
    const row = this.rows[lane.laneIndex];
    gl.useProgram(this.progPointcloud);
    gl.uniform2f(this.uPointcloud['uViewport'], cfg.canvasWidth, cfg.canvasHeight);
    gl.bindVertexArray(row.vao);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, pointCount);
    gl.bindVertexArray(null);
    this.stats.drawCalls++;
  }

  drawCandles(lane: LaneView, rowCount: number): void {
    const gl = this.requireGL();
    const cfg = this.requireCfg();
    const row = this.rows[lane.laneIndex];
    gl.useProgram(this.progCandle);
    gl.uniform2f(this.uCandle['uViewport'], cfg.canvasWidth, cfg.canvasHeight);
    gl.uniform1ui(this.uCandle['uRowCount'], rowCount);
    gl.bindVertexArray(row.vao);
    // Two strips per candle (wick + body) ride one instanced quad draw:
    // the vertex shader emits the union geometry from OHLC (see shader).
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, rowCount);
    gl.bindVertexArray(null);
    this.stats.drawCalls++;
  }

  endFrame(): void {
    const gl = this.requireGL();
    const cfg = this.requireCfg();
    if (this.dirtyEmpty) {
      this.stats.skippedCleanLanes++;
    } else {
      if (this.dirtyX0 > 0 || this.dirtyY0 > 0
        || this.dirtyX1 < cfg.canvasWidth || this.dirtyY1 < cfg.canvasHeight) {
        gl.enable(gl.SCISSOR_TEST);
        this.stats.scissorClips++;
      }
      // Present = swap (host compositor); the draw calls above are the
      // frame. Scissor stays enabled only for this frame's draws — reset
      // happens on the next beginFrame.
    }
    this.frameOpen = false;
    this.stats.presentCount++;
  }

  // --- helpers (init-time only) -------------------------------------------

  /**
   * The SAB canary: attempt a 4-byte bufferSubData from a view over the
   * plane's SharedArrayBuffer. A refusing host (or any throw) selects the
   * staging road — ONCE, here, never per frame.
   */
  private probeSabView(gl: GL2, sab: SharedArrayBuffer): boolean {
    try {
      const probe = new Float32Array(sab, 0, 1);
      const buf = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.bufferData(gl.ARRAY_BUFFER, 4, gl.DYNAMIC_DRAW);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, probe, 0, 1);
      return true;
    } catch {
      return false;
    }
  }

  private buildProgram(
    gl: GL2, name: string,
    vsSource: string, fsSource: string,
    tfVaryings?: readonly string[],
  ): unknown {
    const vs = this.compileShader(gl, gl.VERTEX_SHADER, vsSource, `${name}.vert`);
    const fs = this.compileShader(gl, gl.FRAGMENT_SHADER, fsSource, `${name}.frag`);
    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    // Stable attrib locations across every program family.
    gl.bindAttribLocation(program, A_MINMAX, 'aMinMax');
    gl.bindAttribLocation(program, A_ROW_PRICE, 'aPrice');
    gl.bindAttribLocation(program, A_ROW_SIZE, 'aSize');
    gl.bindAttribLocation(program, A_ROW_SIDE, 'aSide');
    gl.bindAttribLocation(program, A_PC_POSSIZE, 'aPosSize');
    gl.bindAttribLocation(program, A_PC_QUAT, 'aQuat');
    gl.bindAttribLocation(program, A_PC_COLOR, 'aColor');
    gl.bindAttribLocation(program, A_CANDLE_OHLC, 'aOhlc');
    gl.bindAttribLocation(program, A_CANDLE_VOL, 'aVolume');
    if (tfVaryings !== undefined) {
      gl.transformFeedbackVaryings(program, tfVaryings, gl.INTERLEAVED_ATTRIBS);
    }
    gl.linkProgram(program);
    if (gl.getProgramParameter(program, gl.LINK_STATUS) !== true) {
      throw new HeddleError(
        'HC_E_SHADER_COMPILE',
        `webgl2 program ${name} link failed: ${String(gl.getProgramInfoLog(program))}`,
      );
    }
    return program;
  }

  private compileShader(
    gl: GL2, type: number, source: string, name: string,
  ): unknown {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (gl.getShaderParameter(shader, gl.COMPILE_STATUS) !== true) {
      throw new HeddleError(
        'HC_E_SHADER_COMPILE',
        `webgl2 shader ${name} compile failed: ${String(gl.getShaderInfoLog(shader))}`,
      );
    }
    return shader;
  }

  private cacheUniforms(gl: GL2): void {
    this.uDecimate = {
      uTexDims: gl.getUniformLocation(this.progDecimate, 'uTexDims'),
      uSampleCount: gl.getUniformLocation(this.progDecimate, 'uSampleCount'),
      uColumnBucket: gl.getUniformLocation(this.progDecimate, 'uColumnBucket'),
      uWindowStart: gl.getUniformLocation(this.progDecimate, 'uWindowStart'),
      uCapacity: gl.getUniformLocation(this.progDecimate, 'uCapacity'),
      uSamples: gl.getUniformLocation(this.progDecimate, 'uSamples'),
    };
    this.uRibbon = {
      uViewport: gl.getUniformLocation(this.progRibbon, 'uViewport'),
      uColumnCount: gl.getUniformLocation(this.progRibbon, 'uColumnCount'),
      uColor: gl.getUniformLocation(this.progRibbon, 'uColor'),
    };
    this.uLadder = {
      uViewport: gl.getUniformLocation(this.progLadder, 'uViewport'),
      uRowCount: gl.getUniformLocation(this.progLadder, 'uRowCount'),
    };
    this.uPointcloud = {
      uViewport: gl.getUniformLocation(this.progPointcloud, 'uViewport'),
    };
    this.uCandle = {
      uViewport: gl.getUniformLocation(this.progCandle, 'uViewport'),
      uRowCount: gl.getUniformLocation(this.progCandle, 'uRowCount'),
    };
  }

  private requireGL(): GL2 {
    if (this.gl === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'webgl2 hal: no context');
    }
    return this.gl;
  }

  private requireCfg(): EngineConfig {
    if (this.cfg === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'webgl2 hal: not initialized');
    }
    return this.cfg;
  }

  private expandDirty(x0: number, y0: number, x1: number, y1: number): void {
    if (this.dirtyEmpty) {
      this.dirtyX0 = x0; this.dirtyY0 = y0;
      this.dirtyX1 = x1; this.dirtyY1 = y1;
      this.dirtyEmpty = false;
    } else {
      if (x0 < this.dirtyX0) this.dirtyX0 = x0;
      if (y0 < this.dirtyY0) this.dirtyY0 = y0;
      if (x1 > this.dirtyX1) this.dirtyX1 = x1;
      if (y1 > this.dirtyY1) this.dirtyY1 = y1;
    }
  }
}
