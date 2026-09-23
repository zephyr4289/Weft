// runtime.js — Law 3 enforcement helpers: environment detection + guard rails.
//
// Core rule: everything in packages/weft-tensor/src must run on plain Web
// primitives so the SAME bytes work on browsers (main + workers, SAB),
// Node.js, Deno, Bun and Electron. This module never imports node:.

/**
 * Detect the current runtime from an injectable global object (tests pass a
 * fake; production passes nothing).
 */
export function detectRuntime(g = globalThis) {
  const node = typeof g.process !== 'undefined' && !!g.process.versions?.node;
  const deno = typeof g.Deno !== 'undefined' && !!g.Deno.version;
  const bun = typeof g.Bun !== 'undefined' && !!g.Bun.version;
  const windowLike = typeof g.window !== 'undefined' || typeof g.document !== 'undefined';
  const workerLike = typeof g.importScripts === 'function' ||
    (typeof g.WorkerGlobalScope !== 'undefined' && g instanceof g.WorkerGlobalScope);
  return {
    node, deno, bun,
    browser: windowLike || workerLike,
    worker: workerLike,
    electron: node && windowLike,
    sharedArrayBuffer: typeof g.SharedArrayBuffer !== 'undefined',
    webCodecs: typeof g.VideoFrame === 'function',
    webgpu: typeof g.GPU !== 'undefined',
    atomics: typeof g.Atomics !== 'undefined' && typeof g.Atomics.store === 'function',
  };
}

/** Compact runtime fingerprint for CI evidence lines. */
export function runtimeMatrix(g = globalThis) {
  const r = detectRuntime(g);
  const where = r.deno ? 'deno' : r.bun ? 'bun' : r.node ? 'node' : r.worker ? 'worker' : r.browser ? 'browser' : 'unknown';
  return {
    where,
    features: [
      'esm',
      r.sharedArrayBuffer ? 'sab' : null,
      r.webCodecs ? 'webcodecs' : null,
      r.webgpu ? 'webgpu' : null,
      r.atomics ? 'atomics' : null,
      r.electron ? 'electron' : null,
    ].filter(Boolean),
  };
}

/** Assert that a buffer is shareable cross-thread when the caller needs SAB. */
export function assertShareable(buffer, why) {
  if (typeof SharedArrayBuffer !== 'undefined' &&
      buffer[Symbol.toStringTag] !== 'SharedArrayBuffer') {
    throw new TypeError(`${why}: buffer is not a SharedArrayBuffer (postMessage/Atomics.wait need shared memory)`);
  }
}
