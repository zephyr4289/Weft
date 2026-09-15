// useWeft.test.ts — @weft/vue binding unit suite
//
// WHY EXISTS: The framework Heddles shipped with zero tests (D-11 gap), and
// this composable carried a Philosophy violation: frameCount.value++ on
// every frame pushed hot state back through the reactive plane (the exact
// anti-pattern Weft exists to bypass — docs/PHILOSOPHY.md §1). The 2026-09
// hardening throttles the reactive surface to hudIntervalMs and tracks the
// raw count in closure state; this suite pins that contract.
//
// Environment tag: node-vitest+jsdom (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { defineComponent, h, ref } from 'vue';
import { mount } from '@vue/test-utils';
import { Weft } from '@weft/core';
import { useWeft } from '../src/index';

// ---------------------------------------------------------------------------
// Deterministic frame-clock + canvas stubs + controlled clock
// ---------------------------------------------------------------------------

// Handle-keyed frame registry: cancel removes the pending callback, exactly
// like a real browser (a cancelled callback never fires).
const rafMap = new Map<number, FrameRequestCallback>();
let rafHandle = 0;
const cancelled = new Set<number>();
let now = 1_000_000;

function pump(n = 1): void {
  for (let i = 0; i < n; i++) {
    const q = [...rafMap.values()];
    rafMap.clear();
    for (const cb of q) cb(16.7 * (i + 1));
  }
}

function advance(ms: number): void {
  now += ms;
}

const fakeCtx = {} as unknown as CanvasRenderingContext2D;

beforeEach(() => {
  rafMap.clear();
  rafHandle = 0;
  cancelled.clear();
  now = 1_000_000;
  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    const h = ++rafHandle;
    rafMap.set(h, cb);
    return h;
  });
  vi.stubGlobal('cancelAnimationFrame', (h: number) => {
    rafMap.delete(h);
    cancelled.add(h);
  });
  vi.spyOn(Date, 'now').mockImplementation(() => now);
  (HTMLCanvasElement.prototype as unknown as Record<string, unknown>).getContext =
    () => fakeCtx;
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

// ---------------------------------------------------------------------------
// Mount helper: a real component exercising the composable's lifecycle
// ---------------------------------------------------------------------------

interface Harness {
  unmount: () => void;
  api: ReturnType<typeof useWeft>;
}

function mountComposable(weft: Weft, opts?: { hudIntervalMs?: number }): Harness {
  let api!: ReturnType<typeof useWeft>;
  const canvasRef = ref<HTMLCanvasElement | null>(null);
  const Comp = defineComponent({
    setup() {
      api = useWeft(canvasRef, weft, () => {}, opts);
      return () => h('canvas', { ref: canvasRef });
    },
  });
  const wrapper = mount(Comp);
  return { unmount: () => wrapper.unmount(), api };
}

// ---------------------------------------------------------------------------
// Suite
// ---------------------------------------------------------------------------

describe('useWeft', () => {
  it('mounts and starts the loop; unmount cancels it', () => {
    const weft = new Weft(16);
    const harness = mountComposable(weft);
    expect(rafMap.size).toBe(1); // loop scheduled on mount
    expect(cancelled.size).toBe(0);

    harness.unmount();
    expect(cancelled.size).toBe(1);
    const claims = Number(weft.tClaim());
    pump(3);
    expect(Number(weft.tClaim())).toBe(claims); // loop is dead
  });

  it('claims and reads inside the frame callback; draw receives the payload', () => {
    const weft = new Weft(16);
    const seen: number[] = [];
    const canvasRef = ref<HTMLCanvasElement | null>(null);
    let api!: ReturnType<typeof useWeft>;
    const Comp = defineComponent({
      setup() {
        api = useWeft(canvasRef, weft, (_ctx, buf) => seen.push(buf[0]));
        return () => h('canvas', { ref: canvasRef });
      },
    });
    const wrapper = mount(Comp);

    weft.wBegin()[0] = 0x55;
    weft.publish(1, 16);
    pump(1);
    expect(seen).toEqual([0x55]);
    expect(weft.tClaim()).toBe(1n);

    wrapper.unmount();
  });

  it('PHILOSOPHY GATE: reactive frameCount updates at HUD rate, never per frame', () => {
    const weft = new Weft(16);
    const harness = mountComposable(weft);

    // 10 frames within the same hud window: raw climbs, reactive does not.
    pump(10);
    expect(api_count(harness, 'raw')).toBe(10);
    expect(api_count(harness, 'reactive')).toBe(0); // zero reactive writes per frame

    // Cross the hud interval: exactly one reactive refresh.
    advance(1001);
    pump(1);
    expect(api_count(harness, 'raw')).toBe(11);
    expect(api_count(harness, 'reactive')).toBe(11);

    // And again inside the next window: frozen.
    pump(5);
    expect(api_count(harness, 'reactive')).toBe(11);

    harness.unmount();
  });

  it('setDraw swaps the closure without restarting the loop', () => {
    const weft = new Weft(16);
    let tag = 'a';
    const seen: string[] = [];
    const canvasRef = ref<HTMLCanvasElement | null>(null);
    let api!: ReturnType<typeof useWeft>;
    const Comp = defineComponent({
      setup() {
        api = useWeft(canvasRef, weft, () => seen.push(tag));
        return () => h('canvas', { ref: canvasRef });
      },
    });
    const wrapper = mount(Comp);

    pump(1);
    tag = 'b'; // closure would see it anyway; now prove setDraw works with a NEW closure:
    api.setDraw(() => seen.push('c'));
    pump(1);
    expect(seen).toEqual(['a', 'c']);
    expect(cancelled.size).toBe(0); // loop never restarted

    wrapper.unmount();
  });

  it('dispose stops the loop without unmounting (manual teardown path)', () => {
    const weft = new Weft(16);
    const canvasRef = ref<HTMLCanvasElement | null>(null);
    let api!: ReturnType<typeof useWeft>;
    const Comp = defineComponent({
      setup() {
        api = useWeft(canvasRef, weft, () => {});
        return () => h('canvas', { ref: canvasRef });
      },
    });
    const wrapper = mount(Comp);

    api.dispose();
    expect(cancelled.size).toBe(1);
    const claims = Number(weft.tClaim());
    pump(3);
    expect(Number(weft.tClaim())).toBe(claims);

    wrapper.unmount(); // idempotent second cancel is fine
  });
});

// Small accessors to keep assertions readable.
function api_count(h: Harness, kind: 'raw' | 'reactive'): number {
  return kind === 'raw' ? h.api.getRawFrameCount() : h.api.frameCount.value;
}
