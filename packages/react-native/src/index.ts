// weft-rn.ts — React Native Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel via Reanimated SharedValue.
// Per WHITEPAPER §8.4: Reanimated is the prior art Weft's RN story wraps —
// the weakest differentiator. RN ports last in the roadmap.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
//
// 2026-09 hardening (Law 4 — honesty about the boundary):
// 1. The previous version placed a `'worklet'` directive inside a closure
//    capturing a Weft class instance. Reanimated worklets cannot capture
//    class instances; that path would fail at runtime when actually
//    workletized. The directive is removed and the frame callback now runs
//    on the JS thread. True UI-thread reads require the native port that
//    Phase 7 (RN, last) is scheduled to deliver — stated, not implied.
// 2. Both branches now consistently return a disposer (the previous
//    useFrameCallback branch returned undefined while the rAF branch
//    returned a cleanup function — an API trap for callers).

import { Weft, WeftFanoutBroadcaster, type FanoutClaim } from '@weft/core';

/**
 * Draw-phase reader loop for React Native.
 *
 * @param weft - The kernel instance to read from.
 * @param draw - Called once per frame with the live payload view.
 * @param registerFrameCallback - Optional Reanimated `useFrameCallback`
 *   hook. When provided, the loop is driven by it — executing on the JS
 *   thread in this version (see the honesty note above). When absent, a
 *   requestAnimationFrame loop is used.
 * @returns A disposer that stops the loop. Safe to call twice.
 */
export function useWeftDraw(
  weft: Weft,
  draw: (buf: Uint8Array) => void,
  registerFrameCallback?: (cb: () => void) => (() => void) | void
): () => void {
  const tick = () => {
    // Draw phase discipline: claim + read the live buffer inside the frame
    // callback only.
    weft.claim();
    const buf = weft.rReadSlice(16, weft.payloadMax);
    draw(buf);
  };

  if (registerFrameCallback) {
    const unregister = registerFrameCallback(tick);
    let disposed = false;
    return () => {
      if (disposed) return;
      disposed = true;
      if (typeof unregister === 'function') unregister();
    };
  }

  if (typeof requestAnimationFrame !== 'undefined') {
    let raf = 0;
    let disposed = false;
    const loop = () => {
      if (disposed) return;
      tick();
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      if (disposed) return;
      disposed = true;
      if (raf) cancelAnimationFrame(raf);
    };
  }

  // No frame clock available (SSR / non-DOM host): return a no-op disposer
  // rather than silently never drawing — callers can detect the absence of
  // a frame clock before reaching this branch.
  return () => {};
}

// ---------------------------------------------------------------------------
// Fan-out Heddle (RFC 0004) — one surface = one consumer of a broadcaster.
//
// WHY EXISTS: RFC 0004 (accepted as a driver-layer pattern, round-6 §4)
// covers the 1-writer/N-reader case the 1:1 kernel Triad deliberately does
// not. The hook owns one WeftFanoutReader and draws from its own
// pre-allocated buffer; mount N hooks against the same broadcaster.
//
// Inherits the 2026-09 honesty contract of useWeftDraw above: the frame
// callback runs on the JS thread (no worklet directive — Reanimated
// worklets cannot capture class instances); both driver branches return a
// consistent, idempotent disposer.
// ---------------------------------------------------------------------------

/**
 * Fan-out Draw-phase reader loop for React Native (RFC 0004).
 *
 * @param broadcaster - The fan-out broadcaster to consume from.
 * @param draw - Called once per frame with this reader's own pre-allocated
 *   buffer and its (identity-stable, mutated-in-place) claim record.
 * @param registerFrameCallback - Optional Reanimated `useFrameCallback`
 *   hook (JS thread — see the honesty note above). Absent: rAF loop.
 * @returns A disposer that stops the loop. Safe to call twice.
 */
export function useWeftFanoutDraw(
  broadcaster: WeftFanoutBroadcaster,
  draw: (floats: Float32Array, claim: FanoutClaim) => void,
  registerFrameCallback?: (cb: () => void) => (() => void) | void
): () => void {
  // One heddle = one consumer slot: this hook owns its reader.
  const reader = broadcaster.createReader();

  const tick = () => {
    // Draw phase discipline: claim + read inside the frame callback only.
    const claim = reader.claim();
    draw(reader.view(), claim);
  };

  if (registerFrameCallback) {
    const unregister = registerFrameCallback(tick);
    let disposed = false;
    return () => {
      if (disposed) return;
      disposed = true;
      if (typeof unregister === 'function') unregister();
    };
  }

  if (typeof requestAnimationFrame !== 'undefined') {
    let raf = 0;
    let disposed = false;
    const loop = () => {
      if (disposed) return;
      tick();
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      if (disposed) return;
      disposed = true;
      if (raf) cancelAnimationFrame(raf);
    };
  }

  // No frame clock available (SSR / non-DOM host): a no-op disposer, same
  // contract as useWeftDraw — callers detect the absent frame clock first.
  return () => {};
}
