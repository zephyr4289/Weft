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

import { Weft } from '@weft/core';

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
