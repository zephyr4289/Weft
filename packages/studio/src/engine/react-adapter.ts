/**
 * Weft Studio — pluggable React binding + render spy (STUDIO-SEAMS-V1 §4).
 *
 * Panels call hooks through `getReact()` so the managed suite can drive the
 * REAL panel components under a deterministic React shim (Stage 3) and count
 * every function invocation — the zero-re-render proof surface. The
 * production entry (`index.ts`) binds the host React instance.
 *
 * Spy cost on the hot path: ONE Int32Array increment. Zero allocation.
 */

export interface ReactImpl {
  useState<S>(initial: S | (() => S)): [S, (v: S | ((p: S) => S)) => void];
  useRef<T>(initial: T): { current: T };
  useEffect(fn: () => void | (() => void), deps?: unknown[]): void;
  useMemo<T>(fn: () => T, deps: unknown[]): T;
  useCallback<T extends (...a: never[]) => unknown>(fn: T, deps: unknown[]): T;
  createElement(type: string | symbol | ((...a: unknown[]) => unknown), props?: unknown, ...children: unknown[]): unknown;
}

let impl: ReactImpl | null = null;

export function bindReact(react: ReactImpl): void {
  impl = react;
}

export function getReact(): ReactImpl {
  if (!impl) throw new Error('E_SEAM: React not bound — call bindReact(React) before rendering Studio panels');
  return impl;
}

/**
 * Render spy — component invocation counters.
 * `counts[id]` = number of times component `id` was invoked (rendered).
 * `mutations` = setState-initiated re-render requests observed by the shim.
 */
export const spy = {
  counts: new Int32Array(64),
  mutations: 0,
  enabled: false,
  reset(): void {
    this.counts.fill(0);
    this.mutations = 0;
  },
};

/** Component ids (stable across the suite and the UI). */
export const SPY = {
  STUDIO_ROOT: 0,
  SCHEMA_DESIGNER: 1,
  CACHE_MAPPER: 2,
  RING_MONITOR: 3,
  TIME_TRAVEL: 4,
  TELEMETRY_HUD: 5,
  RENDER_SPY_HUD: 6,
  STATUS_BAR: 7,
  PROJECT_TREE: 8,
  DELIVERABLES: 9,
} as const;

/** Increment inside the body of every instrumented component. */
export function markRender(id: number): void {
  if (spy.enabled) spy.counts[id]++;
}
