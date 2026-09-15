// useWeft.ts — Vue 3 Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel as a Vue 3 composable.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
//
// 2026-09 hardening: this composable previously wrote `frameCount.value++` on
// every frame — a reactive write at display rate, which pushes hot state back
// through the reactive plane and re-renders whatever tracks it. That is the
// exact anti-pattern Weft exists to bypass (docs/PHILOSOPHY.md §1: "display
// state is a river, not a ledger"). The raw counter is now a plain closure
// variable; the reactive `frameCount` ref updates at most once per
// `hudIntervalMs` (default 1000 ms — HUD-appropriate, cold-state rate).

import { onMounted, onUnmounted, ref, type Ref } from 'vue';
import { Weft } from '@weft/core';

export interface UseWeftOptions {
  /**
   * Minimum interval in milliseconds between reactive `frameCount` updates.
   * The raw count is tracked per frame; the reactive surface is throttled to
   * keep the reactive plane cold. Default: 1000 (1 Hz — HUD statistics).
   */
  hudIntervalMs?: number;
}

export function useWeft(
  canvasRef: Ref<HTMLCanvasElement | null>,
  weft: Weft,
  draw: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void,
  options: UseWeftOptions = {}
) {
  const hudIntervalMs = options.hudIntervalMs ?? 1000;

  // Reactive surface: updates at HUD rate, never at frame rate.
  const frameCount = ref(0);
  // Raw counter: plain closure state, invisible to Vue's reactivity.
  let rawFrameCount = 0;
  let lastHudAt = 0;
  let raf = 0;
  // Latest draw indirection: a plain mutable field, read only inside the
  // frame callback — invisible to Vue's reactivity by construction.
  let currentDraw = draw;

  const tick = () => {
    const canvas = canvasRef.value;
    if (!canvas) {
      raf = requestAnimationFrame(tick);
      return;
    }
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      raf = requestAnimationFrame(tick);
      return;
    }
    // Draw phase discipline: claim + read the live buffer inside the frame
    // callback only — never during Vue render/effect flushes.
    weft.claim();
    const buf = weft.rLive();
    currentDraw(ctx, buf);

    rawFrameCount++;
    const now = Date.now();
    if (now - lastHudAt >= hudIntervalMs) {
      lastHudAt = now;
      frameCount.value = rawFrameCount;
    }
    raf = requestAnimationFrame(tick);
  };

  onMounted(() => {
    lastHudAt = Date.now();
    raf = requestAnimationFrame(tick);
  });

  onUnmounted(() => {
    if (raf) {
      cancelAnimationFrame(raf);
      raf = 0;
    }
  });

  return {
    /** Reactive frame count, updated at most once per hudIntervalMs. */
    frameCount,
    /** Replace the draw closure without restarting the loop. */
    setDraw(next: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void): void {
      currentDraw = next;
    },
    /** Read the unthrottled raw frame count (advisory, non-reactive). */
    getRawFrameCount(): number {
      return rawFrameCount;
    },
    /** Manual teardown for callers outside a component context. */
    dispose(): void {
      if (raf) {
        cancelAnimationFrame(raf);
        raf = 0;
      }
    },
  };
}
