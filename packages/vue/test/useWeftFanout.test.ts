// useWeftFanout.test.ts — @weft/vue fan-out binding unit suite
//
// WHY EXISTS: RFC 0004 fan-out heddles need the same contracts pinned as
// useWeft (PHILOSOPHY GATE: reactive frameCount at HUD rate only, never per
// frame; draw-phase discipline; idempotent teardown), PLUS the
// multi-consumer property: N composables on one broadcaster are N
// independent readers. Mirrors the useWeft harness: @vue/test-utils mount +
// deterministic rAF pump + controlled Date.now. Environment tag:
// node-vitest+jsdom (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { defineComponent, h, ref } from 'vue';
import { mount } from '@vue/test-utils';
import { WeftFanoutBroadcaster, type FanoutClaim } from '@weft/core';
import { useWeftFanout } from '../src/index';

// ---------------------------------------------------------------------------
// Deterministic frame-clock + canvas stubs + controlled clock
// ---------------------------------------------------------------------------

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
// Mount helper + frame fixture
// ---------------------------------------------------------------------------

interface Harness {
  unmount: () => void;
  api: ReturnType<typeof useWeftFanout>;
}

function mountFanout(
  broadcaster: WeftFanoutBroadcaster,
  draw: (floats: Float32Array, claim: FanoutClaim) => void,
  opts?: { hudIntervalMs?: number }
): Harness {
  let api!: ReturnType<typeof useWeftFanout>;
  const canvasRef = ref<HTMLCanvasElement | null>(null);
  const Comp = defineComponent({
    setup() {
      api = useWeftFanout(
        canvasRef,
        broadcaster,
        (_ctx, floats, claim) => draw(floats, claim),
        opts
      );
      return () => h('canvas', { ref: canvasRef });
    },
  });
  const wrapper = mount(Comp);
  return { unmount: () => wrapper.unmount(), api };
}

function publishFrame(b: WeftFanoutBroadcaster, seq: number): void {
  const v = b.begin();
  for (let i = 0; i < v.length; i++) v[i] = (seq * 131 + i * 37) % 9973;
  b.publish();
}

// ---------------------------------------------------------------------------
// Suite
// ---------------------------------------------------------------------------

describe('useWeftFanout', () => {
  it('mounts and starts the loop; unmount cancels it', () => {
    const b = new WeftFanoutBroadcaster(8);
    const harness = mountFanout(b, () => {});
    expect(rafMap.size).toBe(1);
    expect(cancelled.size).toBe(0);

    harness.unmount();
    expect(cancelled.size).toBe(1);
    const raw = harness.api.getRawFrameCount();
    pump(3);
    expect(harness.api.getRawFrameCount()).toBe(raw); // loop is dead
  });

  it('claims and draws inside the frame callback; draw receives buffer + claim', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: number[] = [];
    const seenSeq: number[] = [];
    const harness = mountFanout(b, (floats, claim) => {
      seen.push(floats[0]);
      seenSeq.push(claim.seq);
    });

    publishFrame(b, 1);
    pump(1);
    expect(seen).toEqual([(1 * 131) % 9973]);
    expect(seenSeq).toEqual([1]);

    harness.unmount();
  });

  it('PHILOSOPHY GATE: reactive frameCount updates at HUD rate, never per frame', () => {
    const b = new WeftFanoutBroadcaster(8);
    const harness = mountFanout(b, () => {});

    // 10 frames within the same hud window: raw climbs, reactive does not.
    pump(10);
    expect(harness.api.getRawFrameCount()).toBe(10);
    expect(harness.api.frameCount.value).toBe(0); // zero reactive writes per frame

    // Cross the hud interval: exactly one reactive refresh.
    advance(1001);
    pump(1);
    expect(harness.api.getRawFrameCount()).toBe(11);
    expect(harness.api.frameCount.value).toBe(11);

    // And frozen inside the next window.
    pump(5);
    expect(harness.api.frameCount.value).toBe(11);

    harness.unmount();
  });

  it('multi-consumer: two composables on one broadcaster are independent readers', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seenA: number[] = [];
    const seenB: number[] = [];
    const hA = mountFanout(b, (f, _c) => seenA.push(f[0]));
    const hB = mountFanout(b, (f, _c) => seenB.push(f[0]));

    publishFrame(b, 1);
    pump(1);
    expect(seenA).toEqual([(1 * 131) % 9973]);
    expect(seenB).toEqual([(1 * 131) % 9973]);

    hA.unmount();
    publishFrame(b, 2);
    pump(1);
    expect(seenA.length).toBe(1); // A is dead
    expect(seenB).toEqual([(1 * 131) % 9973, (2 * 131) % 9973]); // B is alive

    hB.unmount();
  });

  it('setDraw hot-swaps the closure without restarting the loop', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: string[] = [];
    let current = 'first';
    const harness = mountFanout(b, () => seen.push(current));

    pump(1);
    expect(seen).toEqual(['first']);
    expect(cancelled.size).toBe(0);

    harness.api.setDraw(() => seen.push(current));
    current = 'second';
    pump(1);
    expect(seen).toEqual(['first', 'second']);
    expect(cancelled.size).toBe(0); // no restart on draw swap

    harness.unmount();
  });

  it('the claim record is identity-stable across frames (zero alloc through the binding)', () => {
    const b = new WeftFanoutBroadcaster(8);
    const records: FanoutClaim[] = [];
    const harness = mountFanout(b, (_f, claim) => records.push(claim));
    for (let f = 1; f <= 5; f++) {
      publishFrame(b, f);
      pump(1);
    }
    expect(records.length).toBe(5);
    for (const r of records) expect(r).toBe(records[0]); // one mutated record
    expect(records[0].seq).toBe(5);
    harness.unmount();
  });

  it('dispose is idempotent and cancel happens exactly once', () => {
    const b = new WeftFanoutBroadcaster(8);
    const harness = mountFanout(b, () => {});
    harness.api.dispose();
    expect(cancelled.size).toBe(1);
    harness.api.dispose();
    expect(cancelled.size).toBe(1); // no double cancel
    harness.unmount(); // unmount after manual dispose: still one cancel total
    expect(cancelled.size).toBe(1);
  });
});
