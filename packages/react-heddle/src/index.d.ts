// @weft/react-heddle — public type surface.
// Zero-re-render React bindings over the HPL1 Hot-Plane.
import type { HotPlaneView, HotPlaneProducer, FrameScheduler, FrameCtx, LaneOut } from '@weft/heddle-core';

export * from '@weft/heddle-core';

export interface PlaneContextEvents {
  (code: number, detail: string): void;
}

export declare class PlaneContext {
  constructor(buffer: ArrayBufferLike, opts?: {
    hz?: number; byteOffset?: number; raf?: ((cb: () => void) => number) | null; producer?: HotPlaneProducer | null;
  });
  readonly view: HotPlaneView;
  readonly scheduler: FrameScheduler;
  producer: HotPlaneProducer | null;
  acquire(): PlaneContext;
  release(): PlaneContext;
  addSubscriber(fn: (view: HotPlaneView, frameCtx: FrameCtx) => void): () => void;
  onEvent(fn: PlaneContextEvents): () => void;
  publish(lane: number, value: number, nowNs: number): number;
}

export declare function getPlaneContext(buffer: ArrayBufferLike, opts?: {
  hz?: number; byteOffset?: number; raf?: ((cb: () => void) => number) | null; producer?: HotPlaneProducer | null;
}): PlaneContext;

export interface HeddleHooks {
  useWeftPlane: (plane: ArrayBufferLike | PlaneContext, opts?: {
    hz?: number; producer?: HotPlaneProducer | null;
  }) => PlaneContext;
  useWeftSignal: (laneIndex: number, opts?: {
    plane?: ArrayBufferLike | PlaneContext;
    textDivider?: number;
    mutate?: (node: unknown, out: LaneOut, view: HotPlaneView) => void;
  }) => (node: { nodeValue: unknown } | null) => void;
  useWeftStats: (laneIndex: number, opts?: {
    plane?: ArrayBufferLike | PlaneContext;
  }) => {
    current: number; min: number; max: number; avg: number;
    samples: number; seq: number; publishNs: number; flags: number; drops: number;
    lastCode: number;
  };
  useWeftBuffer: (laneIndex: number, opts?: {
    plane?: ArrayBufferLike | PlaneContext;
  }) => { buffer: Float64Array; publish: (nowNs: number) => number };
  WeftPlaneProvider: (props: { value: PlaneContext; children?: unknown }) => unknown;
  PlaneCtxContext: unknown;
}

export declare function createHeddleHooks(React: unknown): HeddleHooks;

export interface RenderEngine {
  contextType: 'webgl2' | 'webgpu' | '2d';
  init(canvas: HTMLCanvasElement, ctx: unknown, view: HotPlaneView): unknown;
  render(state: unknown, frameCtx: FrameCtx, view: HotPlaneView): void;
  dispose?(state: unknown): void;
}

export declare function assertRenderEngine(engine: RenderEngine): RenderEngine;

export declare function createWeftCanvas(React: unknown, hooks?: HeddleHooks): (
  props: {
    plane?: ArrayBufferLike | PlaneContext;
    engine: RenderEngine;
    hz?: number;
    onEvent?: (code: number, detail: string) => void;
    onFatal?: (code: number, detail: string) => void;
    fallback?: unknown;
    style?: unknown;
    className?: string;
  } & Record<string, unknown>,
) => unknown;
