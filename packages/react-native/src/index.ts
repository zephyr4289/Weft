// weft-rn.ts — React Native Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel via Reanimated SharedValue.
// Per WHITEPAPER §8.4: Reanimated is the prior art Weft's RN story wraps —
// the weakest differentiator. RN ports last in the roadmap.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import { Weft } from '@weft/core';

export function useWeftDraw(
  weft: Weft,
  draw: (buf: Uint8Array) => void,
  useFrameCallback?: (cb: () => void) => void
) {
  // If useFrameCallback from react-native-reanimated is supplied, use it;
  // otherwise fallback to requestAnimationFrame on UI loop.
  if (useFrameCallback) {
    useFrameCallback(() => {
      'worklet';
      weft.claim();
      const buf = weft.rReadSlice(16, weft.payloadMax);
      draw(buf);
    });
  } else if (typeof requestAnimationFrame !== 'undefined') {
    let raf = 0;
    const tick = () => {
      weft.claim();
      const buf = weft.rReadSlice(16, weft.payloadMax);
      draw(buf);
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }
}
