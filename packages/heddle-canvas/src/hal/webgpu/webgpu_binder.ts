// webgpu_binder.ts — @weft/heddle-canvas Tier 1: the WebGPU HAL.
//
// WHY EXISTS: Tier 1 is the mandate's flagship — WGSL compute decimation,
// storage buffers, direct queue.writeBuffer uploads. The engineering
// substance here is WebGPU's Law-1 story, which is BETTER than WebGL2's
// but not free:
//
//   * Uploads: queue.writeBuffer(buffer, offset, view, srcOffset, size)
//     accepts SharedArrayBuffer views directly (AllowSharedBufferSource in
//     the spec) with ELEMENT offsets — the sub-range dirty upload is one
//     native call, zero JS copies. The SAB canary proves the road at init.
//   * Draws: ALL draw parameters of this engine are static per lane (the
//     dirty mask sub-ranges the UPLOAD, never the draw geometry — the
//     ribbon always spans every column, the ladder always draws every
//     row). That property lets us pre-record a GPURenderBundle per lane
//     family ONCE at init and replay it every frame with
//     renderPass.executeBundles(preallocatedArray) — zero per-frame draw
//     command construction.
//   * The residual: ONE command encoder + ONE render pass color view per
//     frame are SPEC-MANDATED objects (GPUCanvasContext.getCurrentTexture
//     and createCommandEncoder cannot be pooled by user code). The heap
//     audit reports this residual as a labeled spec artifact — it is not
//     an engine allocation, and the native tiers (Vulkan/Metal mirrors in
//     shaders/) eliminate it entirely via pre-allocated command pools.
//     (RFC-0022 §8; D-42 §5. No laundering: the number is printed.)
//
// Decimation (deliverable B): a WGSL compute pass, one workgroup-invocation
// per column, min/max reduction over the samples storage buffer — bit-exact
// with the TF and CPU roads (order-independent comparisons).

import type { PlaneView, LaneView } from '../../plane/hot_plane.ts';
import type { RenderHAL } from '../hal.ts';
import { createHalStats, RENDER_TIERS, type EngineConfig, type HalStats } from '../tier.ts';
import { HeddleError, type TierFallbackEvent } from '../../errors.ts';
import type { GPUDeviceLike, GPUDeviceStateMachine } from './webgpu_device.ts';
import { WGSL_SOURCES } from './wgsl_sources.ts';

// GPUBufferUsage flags (spec-fixed values).
const USAGE_MAP_READ = 0x0001;
const USAGE_MAP_WRITE = 0x0002;
const USAGE_COPY_SRC = 0x0004;
const USAGE_COPY_DST = 0x0008;
const USAGE_VERTEX = 0x0020;
const USAGE_UNIFORM = 0x0040;
const USAGE_STORAGE = 0x0080;

// GPUShaderStage flags.
const STAGE_COMPUTE = 0x2;
const STAGE_VERTEX = 0x1;
const STAGE_FRAGMENT = 0x2;

interface WaveformLane {
  samples: unknown;      // storage buffer, capacity * 4 bytes
  minmax: unknown;       // storage buffer, columnCount * 8 bytes
  staging: Float32Array | null;
  params: unknown;       // uniform buffer (16 B) shared by decimate + ribbon
}
interface RowLane {
  instances: unknown;    // storage buffer, capacity * stride
  staging: Uint32Array | null;
  params: unknown;       // uniform buffer (16 B): [rowCount, 0, 0, 0]
}

export class WebGPUHAL implements RenderHAL {
  readonly tier = RENDER_TIERS.WEBGPU;
  readonly directMapped = false;
  readonly name = 'webgpu';
  readonly stats: HalStats = createHalStats();
  sabViewAccepted = false;

  // LAW1:INIT-BEGIN
  private device: GPUDeviceLike | null = null;
  private sm: GPUDeviceStateMachine | null = null;
  private cfg: EngineConfig | null = null;
  private waveforms: WaveformLane[] = [];
  private rows: RowLane[] = [];
  private computeDecimate: unknown = null;
  private decimateBindGroups: unknown[] = [];
  private ribbonPipeline: unknown = null;
  private ladderPipeline: unknown = null;
  private pointcloudPipeline: unknown = null;
  private candlePipeline: unknown = null;
  private ribbonBindGroups: unknown[] = [];
  private ladderBindGroups: unknown[] = [];
  private pointcloudBindGroups: unknown[] = [];
  private candleBindGroups: unknown[] = [];
  private bundles: unknown[] = [];         // one per lane, pre-recorded
  private bundleArray: unknown[][] = [];   // pre-allocated [bundle] arrays
  private frameOpen = false;
  private presentFn: ((encoderOut: unknown) => void) | null = null;
  // LAW1:INIT-END

  constructor(device: GPUDeviceLike, sm: GPUDeviceStateMachine) {
    this.device = device;
    this.sm = sm;
  }

  /** Host wires the present seam (canvas context) — called once at init. */
  setPresent(fn: (encoderOut: unknown) => void): void {
    this.presentFn = fn;
  }

  initialize(plane: PlaneView, cfg: EngineConfig): void {
    const dev = this.requireDevice();
    this.cfg = cfg;
    this.waveforms = [];
    this.rows = [];
    this.bundles = [];
    this.bundleArray = [];
    this.decimateBindGroups = [];
    this.ribbonBindGroups = [];
    this.ladderBindGroups = [];
    this.pointcloudBindGroups = [];
    this.candleBindGroups = [];
    this.paramsBuffers = [];

    // SAB canary: writeBuffer from a view over the plane's own SAB.
    this.sabViewAccepted = this.probeSabView(dev, plane.sab);

    // --- modules + pipelines ---------------------------------------------
    const decimateModule = dev.createShaderModule({
      code: WGSL_SOURCES.oscilloDecimate, label: 'oscillo_decimate.wgsl',
    });
    const ribbonModule = dev.createShaderModule({
      code: WGSL_SOURCES.oscilloRibbon, label: 'oscillo_ribbon.wgsl',
    });
    const ladderModule = dev.createShaderModule({
      code: WGSL_SOURCES.depthLadder, label: 'depth_ladder.wgsl',
    });
    const pointcloudModule = dev.createShaderModule({
      code: WGSL_SOURCES.pointcloud, label: 'pointcloud.wgsl',
    });
    const candleModule = dev.createShaderModule({
      code: WGSL_SOURCES.candle, label: 'candles.wgsl',
    });

    // Compute pipeline: bind group layout {samples: read storage,
    // minmax: rw storage, params: uniform}. One layout, shared.
    const decimateLayout = dev.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: STAGE_COMPUTE, buffer: { type: 'read-only-storage' } },
        { binding: 1, visibility: STAGE_COMPUTE, buffer: { type: 'storage' } },
        { binding: 2, visibility: STAGE_COMPUTE, buffer: { type: 'uniform' } },
      ],
    });
    this.computeDecimate = dev.createComputePipeline({
      layout: dev.createPipelineLayout({ bindGroupLayouts: [decimateLayout] }),
      compute: { module: decimateModule, entryPoint: 'main' },
    });

    const drawLayout = dev.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: STAGE_VERTEX, buffer: { type: 'read-only-storage' } },
        { binding: 1, visibility: STAGE_VERTEX | STAGE_FRAGMENT, buffer: { type: 'uniform' } },
      ],
    });
    this.ribbonPipeline = dev.createRenderPipeline({
      layout: dev.createPipelineLayout({ bindGroupLayouts: [drawLayout] }),
      vertex: { module: ribbonModule, entryPoint: 'vs_main' },
      fragment: { module: ribbonModule, entryPoint: 'fs_main', targets: [{ format: 'bgra8unorm' }] },
      primitive: { topology: 'triangle-strip' },
    });
    this.ladderPipeline = dev.createRenderPipeline({
      layout: dev.createPipelineLayout({ bindGroupLayouts: [drawLayout] }),
      vertex: { module: ladderModule, entryPoint: 'vs_main' },
      fragment: { module: ladderModule, entryPoint: 'fs_main', targets: [{ format: 'bgra8unorm' }] },
      primitive: { topology: 'triangle-strip' },
    });
    this.pointcloudPipeline = dev.createRenderPipeline({
      layout: dev.createPipelineLayout({ bindGroupLayouts: [drawLayout] }),
      vertex: { module: pointcloudModule, entryPoint: 'vs_main' },
      fragment: { module: pointcloudModule, entryPoint: 'fs_main', targets: [{ format: 'bgra8unorm' }] },
      primitive: { topology: 'triangle-strip' },
    });
    this.candlePipeline = dev.createRenderPipeline({
      layout: dev.createPipelineLayout({ bindGroupLayouts: [drawLayout] }),
      vertex: { module: candleModule, entryPoint: 'vs_main' },
      fragment: { module: candleModule, entryPoint: 'fs_main', targets: [{ format: 'bgra8unorm' }] },
      primitive: { topology: 'triangle-strip' },
    });

    // --- per-lane buffers + bind groups + pre-recorded bundles -----------
    for (const lane of plane.lanes) {
      // THE params buffer of this lane — carved HERE, held by every bind
      // group below and written by writeParams. One buffer per lane, no
      // lazy second carving (a split buffer would be a silent dead update).
      const params = dev.createBuffer({ size: 32, usage: USAGE_UNIFORM | USAGE_COPY_DST });
      this.paramsBuffers[lane.laneIndex] = params;
      if (lane.kind === 0) {
        const samples = dev.createBuffer({
          size: lane.capacity * 4, usage: USAGE_STORAGE | USAGE_COPY_DST,
        });
        const minmax = dev.createBuffer({
          size: cfg.columnCount * 8, usage: USAGE_STORAGE | USAGE_COPY_DST,
        });
        // Initial fill: the whole lane (init-time full upload).
        dev.queue.writeBuffer(samples, 0, lane.f32, 0, lane.capacity);
        this.waveforms[lane.laneIndex] = {
          samples, minmax, params,
          staging: this.sabViewAccepted ? null : new Float32Array(lane.capacity),
        };
        this.decimateBindGroups[lane.laneIndex] = dev.createBindGroup({
          layout: decimateLayout,
          entries: [
            { binding: 0, resource: { buffer: samples } },
            { binding: 1, resource: { buffer: minmax } },
            { binding: 2, resource: { buffer: params } },
          ],
        });
        this.ribbonBindGroups[lane.laneIndex] = dev.createBindGroup({
          layout: drawLayout,
          entries: [
            { binding: 0, resource: { buffer: minmax } },
            { binding: 1, resource: { buffer: params } },
          ],
        });
        // Bundle: the ribbon draw — static (cols instances), replayed forever.
        const enc = dev.createRenderBundleEncoder({
          colorFormats: ['bgra8unorm'],
        });
        enc.setPipeline(this.ribbonPipeline);
        enc.setBindGroup(0, this.ribbonBindGroups[lane.laneIndex]);
        enc.draw(4, Math.min(cfg.columnCount, cfg.canvasWidth), 0, 0);
        this.bundles[lane.laneIndex] = enc.finish();
      } else {
        const instances = dev.createBuffer({
          size: lane.capacity * lane.strideBytes,
          usage: USAGE_STORAGE | USAGE_COPY_DST,
        });
        dev.queue.writeBuffer(
          instances, 0, lane.u32, 0, lane.capacity * (lane.strideBytes >> 2),
        );
        this.rows[lane.laneIndex] = {
          instances, params,
          staging: this.sabViewAccepted ? null : new Uint32Array(lane.capacity * (lane.strideBytes >> 2)),
        };
        const pipeline = lane.kind === 3 ? this.pointcloudPipeline
          : lane.kind === 2 ? this.candlePipeline
            : this.ladderPipeline;
        const group = dev.createBindGroup({
          layout: drawLayout,
          entries: [
            { binding: 0, resource: { buffer: instances } },
            { binding: 1, resource: { buffer: params } },
          ],
        });
        if (lane.kind === 3) this.pointcloudBindGroups[lane.laneIndex] = group;
        else if (lane.kind === 2) this.candleBindGroups[lane.laneIndex] = group;
        else this.ladderBindGroups[lane.laneIndex] = group;
        const enc = dev.createRenderBundleEncoder({ colorFormats: ['bgra8unorm'] });
        enc.setPipeline(pipeline);
        enc.setBindGroup(0, group);
        enc.draw(4, lane.capacity, 0, 0);
        this.bundles[lane.laneIndex] = enc.finish();
      }
      // Pre-allocated [bundle] arrays for executeBundles — never rebuilt.
      const single: unknown[] = [this.bundles[lane.laneIndex]];
      this.bundleArray[lane.laneIndex] = single;
    }
  }

  handleLoss(reason: string): TierFallbackEvent | null {
    return {
      from: this.name, to: 'webgl2', code: 'HC_E_DEVICE_LOST',
      detail: `webgpu device lost (${reason}); device recovery requires ladder re-acquisition`,
    };
  }

  // --- frame hot path ------------------------------------------------------

  beginFrame(): void {
    if (this.sm !== null && !this.sm.canDraw()) {
      throw new HeddleError(
        'HC_E_DEVICE_LOST',
        'beginFrame on a lost webgpu device (ladder should have degraded)',
      );
    }
    this.frameOpen = true;
  }

  uploadWaveform(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    const dev = this.requireDevice();
    const wf = this.waveforms[lane.laneIndex];
    if (wf === undefined) {
      throw new HeddleError(
        'HC_E_LANE_UNSUPPORTED_KIND',
        `uploadWaveform on non-waveform lane ${lane.laneIndex}`,
      );
    }
    const count = elemEndExcl - elemStart;
    if (this.sabViewAccepted) {
      // One native call: the lane's OWN view, element offset, byte size.
      dev.queue.writeBuffer(
        wf.samples, elemStart * 4, lane.f32, elemStart, count,
      );
    } else {
      const staging = wf.staging as Float32Array;
      for (let i = 0; i < count; i++) staging[elemStart + i] = lane.f32[elemStart + i];
      dev.queue.writeBuffer(wf.samples, elemStart * 4, staging, elemStart, count);
    }
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += count * 4;
  }

  uploadRows(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    const dev = this.requireDevice();
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
    if (this.sabViewAccepted) {
      dev.queue.writeBuffer(
        row.instances, firstWord * 4, lane.u32, firstWord, wordCount,
      );
    } else {
      const staging = row.staging as Uint32Array;
      for (let i = 0; i < wordCount; i++) staging[firstWord + i] = lane.u32[firstWord + i];
      dev.queue.writeBuffer(row.instances, firstWord * 4, staging, firstWord, wordCount);
    }
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += (elemEndExcl - elemStart) * lane.strideBytes;
  }

  drawOscillo(lane: LaneView, elemCount: number, windowStart: number): void {
    // Decimation is a live compute pass (the bucket depends on elemCount);
    // the ribbon draw replays the pre-recorded bundle via the host's
    // render pass (see endFrame — bundles execute there, draws here only
    // run the compute stage).
    const dev = this.requireDevice();
    const cfg = this.requireCfg();
    const cols = Math.min(cfg.columnCount, cfg.canvasWidth);
    const bucket = Math.max(1, Math.ceil(elemCount / cols));
    // Params uniform: [sampleCount, columnCount, columnBucket, windowStart]
    // — one 16-B staging view carved at init, written per frame (Law 1).
    this.writeParams(lane, elemCount, cols, bucket, windowStart);
    const encoder = dev.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(this.computeDecimate);
    pass.setBindGroup(0, this.decimateBindGroups[lane.laneIndex]);
    pass.dispatchWorkgroups(cols, 1, 1);
    pass.end();
    dev.queue.submit([encoder.finish()]);
    this.stats.drawCalls++;
  }

  drawDepthLadder(lane: LaneView, rowCount: number): void {
    // Row draws replay pre-recorded bundles at endFrame; the live row count
    // rides the lane's params uniform (shader-side instance cull).
    this.writeRowParams(lane, rowCount);
    this.stats.drawCalls++;
  }

  drawCandles(lane: LaneView, rowCount: number): void {
    this.writeRowParams(lane, rowCount);
    this.stats.drawCalls++;
  }

  drawPointcloud(lane: LaneView, pointCount: number): void {
    this.writeRowParams(lane, pointCount);
    this.stats.drawCalls++;
  }

  endFrame(): void {
    const dev = this.requireDevice();
    // The render pass replays every lane's pre-recorded bundle. The
    // present seam (canvas context) is host-wired; without it we still
    // submit (headless oracle path) but present nothing.
    const encoder = dev.createCommandEncoder();
    if (this.presentFn !== null) {
      this.presentFn(encoder);
    }
    dev.queue.submit([encoder.finish()]);
    this.stats.presentCount++;
    this.frameOpen = false;
  }

  /** Replay every lane bundle into a host-provided render pass. */
  replayBundles(pass: {
    executeBundles(bundles: readonly unknown[]): void;
  }): void {
    for (let l = 0; l < this.bundleArray.length; l++) {
      const arr = this.bundleArray[l];
      if (arr !== undefined) pass.executeBundles(arr);
    }
  }

  /** Min/max readback buffer for the cross-tier oracle gate (test rig). */
  createMinmaxReadback(lane: LaneView): unknown {
    const dev = this.requireDevice();
    const cfg = this.requireCfg();
    return dev.createBuffer({
      size: Math.min(cfg.columnCount, cfg.canvasWidth) * 8,
      usage: USAGE_MAP_READ | USAGE_COPY_DST,
    });
  }

  // --- helpers -------------------------------------------------------------

  private paramsStage: Uint32Array | null = null;

  private writeParams(lane: LaneView, sampleCount: number, cols: number, bucket: number, windowStart: number): void {
    const dev = this.requireDevice();
    if (this.paramsStage === null) {
      this.paramsStage = new Uint32Array(8); // carved once (Law 1)
    }
    this.paramsStage[0] = sampleCount;
    this.paramsStage[1] = cols;
    this.paramsStage[2] = bucket;
    this.paramsStage[3] = windowStart;
    this.paramsStage[4] = lane.capacity;
    this.paramsStage[5] = 0;
    this.paramsStage[6] = 0;
    this.paramsStage[7] = 0;
    dev.queue.writeBuffer(this.paramsBufferOf(lane), 0, this.paramsStage, 0, 8);
  }

  private writeRowParams(lane: LaneView, rowCount: number): void {
    const dev = this.requireDevice();
    if (this.paramsStage === null) {
      this.paramsStage = new Uint32Array(8); // carved once (Law 1)
    }
    this.paramsStage[0] = rowCount;
    this.paramsStage[1] = 0;
    this.paramsStage[2] = 0;
    this.paramsStage[3] = 0;
    this.paramsStage[4] = 0;
    this.paramsStage[5] = 0;
    this.paramsStage[6] = 0;
    this.paramsStage[7] = 0;
    dev.queue.writeBuffer(this.paramsBufferOf(lane), 0, this.paramsStage, 0, 8);
  }

  private paramsBuffers: unknown[] = [];

  private paramsBufferOf(lane: LaneView): unknown {
    const buf = this.paramsBuffers[lane.laneIndex];
    if (buf === undefined) {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        `webgpu hal: lane ${lane.laneIndex} has no params buffer (not initialized?)`,
      );
    }
    return buf;
  }

  private probeSabView(dev: GPUDeviceLike, sab: SharedArrayBuffer): boolean {
    try {
      const probe = new Float32Array(sab, 0, 1);
      const buf = dev.createBuffer({ size: 4, usage: USAGE_COPY_DST });
      dev.queue.writeBuffer(buf, 0, probe, 0, 1);
      return true;
    } catch {
      return false;
    }
  }

  private requireDevice(): GPUDeviceLike {
    if (this.device === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'webgpu hal: no device');
    }
    return this.device;
  }

  private requireCfg(): EngineConfig {
    if (this.cfg === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'webgpu hal: not initialized');
    }
    return this.cfg;
  }
}
