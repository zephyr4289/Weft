// index.d.ts — @weft/react-hooks public type surface.
//
// Views are structurally typed: any object with bind()/validateHeader()
// (i.e. every weftc-generated view) plugs in without importing codegen.
// Byte offsets in the docs refer to the canonical fixtures
// (tools/weftc/schema/fixtures).

export as namespace WeftReactHooks;

/** A bytes-like frame window (ArrayBuffer, SharedArrayBuffer, TypedArray). */
export type BufferLike = ArrayBufferLike | ArrayBufferView;

/**
 * Frame source: a reference-stable pub/sub over
 * (buffer, byteOffset, availBytes) triples.
 */
export interface FrameSource {
  subscribe(fn: FrameListener): () => void;
  emit(buffer: BufferLike, byteOffset: number, availBytes: number): void;
  readonly listenerCount: number;
}

export type FrameListener = (
  buffer: BufferLike,
  byteOffset: number,
  availBytes: number,
) => void;

/** Structural type of every weftc-generated view (flyweight). */
export interface WeftView {
  bind(buffer: BufferLike, byteOffset?: number): this;
  validateHeader(avail?: number): boolean;
}

/** Create a frame source (reference-stable subscribe/emit identities). */
export function createFrameSource(): FrameSource;

/** Attach a WebSocket as a frame producer (binary messages only). */
export function attachWebSocket(source: FrameSource, ws: WebSocket): () => void;

/**
 * Subscribe to a frame source without ever triggering a React re-render.
 * onFrame is held in a latest-ref (safe to pass a fresh closure per render).
 */
export function useWeftBuffer(
  source: FrameSource,
  onFrame: (buffer: BufferLike, byteOffset: number, availBytes: number) => void,
): void;

/**
 * Paint frames into a canvas at display cadence without re-rendering React.
 * Frames coalesce into the next animation tick; newest wins; the loop parks
 * when the document is hidden. Returns a ref for the <canvas> element.
 */
export function useWeftCanvas(
  source: FrameSource,
  draw: (
    ctx: CanvasRenderingContext2D,
    buffer: BufferLike,
    byteOffset: number,
    availBytes: number,
  ) => void,
): React.MutableRefObject<HTMLCanvasElement | null>;

/**
 * Fixed-capacity ring of Float32 samples (preallocated once; push overwrites
 * the oldest slot — zero steady-state allocation).
 */
export interface SampleRing {
  push(v: number): void;
  forEach(fn: (v: number, i: number) => void): void;
  readonly length: number;
  readonly capacity: number;
}

export function createSampleRing(capacity: number): SampleRing;

/** Auto-scaling polyline oscilloscope painter for one channel. */
export function drawFrameGraph(
  ctx: CanvasRenderingContext2D,
  ring: SampleRing,
  width: number,
  height: number,
): void;

// React type shims (avoid a hard @types/react dependency for consumers
// that use different React type sources).
declare namespace React {
  interface MutableRefObject<T> {
    current: T;
  }
}
declare var WebSocket: {
  new (url: string | URL): {
    addEventListener(type: 'message', fn: (e: { data: unknown }) => void): void;
    removeEventListener(type: 'message', fn: (e: { data: unknown }) => void): void;
  };
};
