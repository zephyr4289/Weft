// index.d.ts — @weft/react-tensor typings.
import type {
  WeftTensorRing, WeftTensorFrameView, OverlayScratch,
} from '../../weft-tensor/src/index.js';

export interface TensorCanvasStats {
  ticks: number; frames: number; stalls: number; boxes: number;
}

export declare class TensorCanvasController {
  constructor(spec: {
    ring: WeftTensorRing;
    canvas: unknown;
    backend?: '2d' | 'webgl';
    width?: number;
    height?: number;
    overlay?: ((frame: WeftTensorFrameView | null, scratch: OverlayScratch, plane: unknown) => void) | null;
    drawBoxes?: boolean;
    pump?: { request(cb: () => void): number; cancel(id: number): void };
  });
  readonly ring: WeftTensorRing;
  readonly stats: TensorCanvasStats;
  start(): this;
  stop(): this;
  get running(): boolean;
}

export declare function createTensorCanvasHooks(hooks: {
  useRef: <T>(init: T) => { current: T };
  useEffect: (fn: () => void | (() => void), deps: unknown[]) => void;
}): (
  ring: WeftTensorRing | null,
  canvasRef: { current: unknown },
  opts?: {
    backend?: '2d' | 'webgl';
    overlay?: ((frame: WeftTensorFrameView | null, scratch: OverlayScratch, plane: unknown) => void) | null;
    drawBoxes?: boolean;
  },
) => { current: TensorCanvasStats };

export declare const useWeftTensorCanvas: ReturnType<typeof createTensorCanvasHooks>;
