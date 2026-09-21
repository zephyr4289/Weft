// webgpu_device.ts — @weft/heddle-canvas WebGPU acquisition + loss ladder.
//
// WHY EXISTS: Law 4 names WebGPU device loss as a first-class failure mode.
// WebGPU signals death via a PROMISE (gpuDevice.lost) that resolves exactly
// once — which makes it easy to miss and impossible to retry: a lost device
// never comes back; the only honest recovery is re-acquire adapter+device
// and re-carve every resource (or degrade to WebGL2). This module wires the
// promise ONCE into a state machine the frame loop can poll synchronously,
// so a dead device degrades through the ladder instead of throwing
// mid-frame. Device acquisition itself is async — the ladder awaits it and
// treats a null adapter as the named refusal (HC_E_BACKEND_REFUSED).

import { HeddleError } from '../../errors.ts';

export const GPU_DEVICE_STATE = {
  ALIVE: 0,
  LOST: 1,
} as const;
export type GPUDeviceState = (typeof GPU_DEVICE_STATE)[keyof typeof GPU_DEVICE_STATE];

/** Structural WebGPU surface (fake-able in the node battery). */
export interface GPUAdapterLike {
  requestDevice(): Promise<GPUDeviceLike>;
}
export interface GPUQueueLike {
  writeBuffer(
    buffer: unknown, bufferOffset: number, data: ArrayBufferView,
    dataOffset?: number, size?: number,
  ): void;
  submit(buffers: readonly unknown[]): void;
  onSubmittedWorkDone(): Promise<void>;
}
export interface GPUDeviceLike {
  createBuffer(descriptor: {
    size: number; usage: number; mappedAtCreation?: boolean;
  }): unknown;
  createShaderModule(descriptor: { code: string; label?: string }): unknown;
  createComputePipeline(descriptor: Record<string, unknown>): unknown;
  createRenderPipeline(descriptor: Record<string, unknown>): unknown;
  createPipelineLayout(descriptor: Record<string, unknown>): unknown;
  createBindGroupLayout(descriptor: Record<string, unknown>): unknown;
  createBindGroup(descriptor: Record<string, unknown>): unknown;
  createCommandEncoder(): {
    beginRenderPass(descriptor: Record<string, unknown>): {
      setPipeline(pipeline: unknown): void;
      setBindGroup(index: number, group: unknown, dynamicOffsets?: Float32Array): void;
      executeBundles(bundles: readonly unknown[]): void;
      end(): void;
    };
    beginComputePass(descriptor?: Record<string, unknown>): {
      setPipeline(pipeline: unknown): void;
      setBindGroup(index: number, group: unknown): void;
      dispatchWorkgroups(x: number, y?: number, z?: number): void;
      end(): void;
    };
    copyBufferToBuffer(src: unknown, srcOffset: number, dst: unknown, dstOffset: number, size: number): void;
    finish(): unknown;
  };
  createRenderBundleEncoder(descriptor: Record<string, unknown>): {
    setPipeline(pipeline: unknown): void;
    setBindGroup(index: number, group: unknown): void;
    draw(vertexCount: number, instanceCount: number, firstVertex?: number, firstInstance?: number): void;
    finish(): unknown;
  };
  readonly queue: GPUQueueLike;
  readonly lost: Promise<{ reason: string; message: string }>;
}

export interface WebGPUAcquireResult {
  readonly device: GPUDeviceLike;
  readonly machine: GPUDeviceStateMachine;
}

/**
 * The loss state machine. ONE wire of device.lost; canDraw() is the
 * synchronous frame-loop gate. Re-acquisition is the LADDER's call, not
// ours (we report; we do not silently resurrect — Law 4).
 */
export class GPUDeviceStateMachine {
  private state: GPUDeviceState = GPU_DEVICE_STATE.ALIVE;
  private lossReason = '';

  constructor(device: GPUDeviceLike) {
    // The lost promise resolves once; the closure is carved here and never
    // again (Law 1: listeners/handlers are init-time objects).
    void device.lost.then((info: { reason: string; message: string }) => {
      this.state = GPU_DEVICE_STATE.LOST;
      this.lossReason = `${info.reason}: ${info.message}`;
    });
  }

  canDraw(): boolean {
    return this.state === GPU_DEVICE_STATE.ALIVE;
  }

  currentState(): GPUDeviceState {
    return this.state;
  }

  lossDetail(): string {
    return this.lossReason;
  }
}

/**
 * Acquire a WebGPU device through an injected gpu entry point
 * (navigator.gpu in the browser rig, a fake in the node battery).
 * Returns null when the platform refuses — the named ladder refusal.
 */
export async function acquireWebGPU(
  gpu: { requestAdapter(): Promise<GPUAdapterLike | null> } | undefined,
): Promise<WebGPUAcquireResult | null> {
  if (gpu === undefined || gpu === null) return null;
  const adapter = await gpu.requestAdapter();
  if (adapter === null) return null;
  const device = await adapter.requestDevice();
  if (device === null || device === undefined) return null;
  if (typeof device.lost?.then !== 'function') {
    throw new HeddleError(
      'HC_E_BACKEND_REFUSED',
      'webgpu device lacks a lost promise (non-conforming host)',
    );
  }
  return { device, machine: new GPUDeviceStateMachine(device) };
}
