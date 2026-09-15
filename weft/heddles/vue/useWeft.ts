// useWeft.ts — Vue 3 Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel as a Vue 3 composable.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import { onMounted, onUnmounted, ref } from 'vue';
import { Weft } from '../../core/ts/weft';

export function useWeft(canvasRef: Ref<HTMLCanvasElement | null>, weft: Weft, draw: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void) {
  const frameCount = ref(0);
  let raf = 0;

  onMounted(() => {
    const canvas = canvasRef.value;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const tick = () => {
      weft.claim();
      const buf = weft.rReadSlice(16, weft.payloadMax);
      draw(ctx, buf);
      frameCount.value++;
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
  });

  onUnmounted(() => cancelAnimationFrame(raf));

  return { frameCount };
}

// Fix: Vue 3's Ref type
import type { Ref } from 'vue';
