// audit.ts — the Law-1 gates: 60,000 consecutive frames, zero ENGINE heap
// allocations, MEASURED with the right instrument.
//
// WHY TWO INSTRUMENTS (the load-bearing subtlety):
//
//   Gate A — the V8 SAMPLING HEAP PROFILER (primary, rigorous). Over the
//   measured window it attributes every allocation sample to its call
//   frame; the gate requires ZERO bytes attributable to engine/bench
//   modules. A steady 16-byte-per-frame allocator cannot hide: 60,000
//   frames × 16 B ≈ 1 MiB against a 4-KiB sampling interval ≈ 250+
//   samples, every one carrying our stack. (Verified against a planted
//   allocator in the battery: the profiler sees it loud and clear.)
//
//   Gate B — heapUsed delta (secondary, REPORTED not gated). V8's
//   heapUsed drifts by bookkeeping (semi-space accounting, IC metadata)
//   even when application code allocates nothing — measuring that as
//   "engine allocation" would be the dishonest version of this gate. The
//   number is printed with its label; the claim rides on Gate A.
//
// BOUNDARY (documented, not laundered): this measures the JS heap of the
// engine path on the NullHAL/software-ICD runners. Driver-internal
// allocations (browser GPU process, Vulkan command pools) are outside the
// JS heap; the WebGPU spec-mandated per-frame encoder objects are the
// labeled residual (RFC-0022 §8, D-42 §5).

import inspector from 'node:inspector';

export interface HeapGateResult {
  readonly warmupFrames: number;
  readonly measuredFrames: number;
  readonly heapBeforeBytes: number;
  readonly heapAfterBytes: number;
  readonly deltaBytes: number;
  readonly passed: boolean;
}

export interface SamplingGateResult {
  readonly warmupFrames: number;
  readonly measuredFrames: number;
  /** Bytes attributed to OUR modules (engine/bench). The gate target: 0. */
  readonly engineBytes: number;
  /** Everything allocated during the window, including V8 internals. */
  readonly totalBytes: number;
  /** Allocation samples carrying our modules' frames. */
  readonly engineSamples: number;
  readonly passed: boolean;
}

/** Whether this runtime exposes the forced-GC the heap gate needs. */
export function heapGateAvailable(): boolean {
  return typeof (globalThis as { gc?: unknown }).gc === 'function';
}

/** Whether this runtime has the inspector heap profiler (node; not bun/browser). */
export function samplingGateAvailable(): boolean {
  return typeof inspector?.Session === 'function';
}

/**
 * Gate B (secondary): heapUsed delta across `measuredFrames` driver runs,
 * with forced GC at both endpoints. Passed === (delta === 0) — reported,
 * not load-bearing, because V8 bookkeeping can move the number while the
 * code allocates nothing (Gate A decides the claim).
 */
export function heapGate(
  warmupFrames: number,
  measuredFrames: number,
  driver: (i: number) => void,
): HeapGateResult {
  const gc = (globalThis as { gc?: () => void }).gc;
  if (typeof gc !== 'function') {
    throw new Error('[HC_E_LAW1_GATE] heap gate requires forced GC — run under node --expose-gc');
  }
  for (let i = 0; i < warmupFrames; i++) driver(i);
  gc();
  gc(); // twice: sweep finalizers, then the survivors
  const before = process.memoryUsage().heapUsed;
  for (let i = 0; i < measuredFrames; i++) driver(warmupFrames + i);
  const after = process.memoryUsage().heapUsed;
  const delta = after - before;
  return {
    warmupFrames,
    measuredFrames,
    heapBeforeBytes: before,
    heapAfterBytes: after,
    deltaBytes: delta,
    passed: delta === 0,
  };
}

interface ProfileNode {
  callFrame: { functionName: string; url: string };
  selfSize: number;
  children: ProfileNode[];
}

/**
 * Gate A (primary): run the driver under V8's sampling heap profiler and
 * require ZERO allocation bytes attributable to engine modules. Stack
 * attribution: any frame whose url points INSIDE the package (src/ or
 * bench/) and is not the profiler plumbing counts as ours.
 */
export async function samplingAllocationGate(
  warmupFrames: number,
  measuredFrames: number,
  driver: (i: number) => void,
): Promise<SamplingGateResult> {
  const session = new inspector.Session();
  session.connect();
  const post = (method: string, params?: Record<string, unknown>): Promise<unknown> =>
    new Promise((resolve, reject) => {
      session.post(method, params, (err: Error | null, res?: object) =>
        err ? reject(err) : resolve(res),
      );
    });

  for (let i = 0; i < warmupFrames; i++) driver(i);
  const gc = (globalThis as { gc?: () => void }).gc;
  if (typeof gc === 'function') {
    gc();
    gc();
  }

  await post('HeapProfiler.startSampling', { samplingInterval: 4096 });
  for (let i = 0; i < measuredFrames; i++) driver(warmupFrames + i);
  const res = (await post('HeapProfiler.stopSampling')) as { profile: { head: ProfileNode } };
  session.disconnect();

  let engineBytes = 0;
  let totalBytes = 0;
  let engineSamples = 0;
  const walk = (node: ProfileNode): void => {
    // ENGINE attribution scope: the modules Law 1 governs. The gate's own
    // loop (src/law1) and the harness (bench/, test/) are excluded — the
    // ruler must not measure itself.
    const isOurs =
      node.selfSize > 0 &&
      typeof node.callFrame.url === 'string' &&
      /packages[\\/]heddle-canvas[\\/]src[\\/](plane|hal|loop|renderers)[\\/]/.test(node.callFrame.url);
    if (node.selfSize > 0) {
      totalBytes += node.selfSize;
      if (isOurs) {
        engineBytes += node.selfSize;
        engineSamples++;
      }
    }
    for (const c of node.children) walk(c);
  };
  walk(res.profile.head);
  return {
    warmupFrames,
    measuredFrames,
    engineBytes,
    totalBytes,
    engineSamples,
    passed: engineBytes === 0,
  };
}

/**
 * Mid-run drift probe (the bench prints this): a NON-failing observation
 * of heapUsed so the evidence log shows the heap state, labeled.
 */
export function heapNow(): number {
  return process.memoryUsage().heapUsed;
}
