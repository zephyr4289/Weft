// weft-rn.ts — React Native Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel via Reanimated SharedValue.
// Per WHITEPAPER §8.4: Reanimated is the prior art Weft's RN story wraps —
// the weakest differentiator. RN ports last in the roadmap.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import { useFrameCallback } from 'react-native-reanimated';
import { Weft } from '../../core/ts/weft';

export function useWeftDraw(
  weft: Weft,
  draw: (buf: Uint8Array) => void,
) {
  useFrameCallback(() => {
    'worklet';
    weft.claim();
    const buf = weft.rReadSlice(16, weft.payloadMax);
    draw(buf);
  });
}
