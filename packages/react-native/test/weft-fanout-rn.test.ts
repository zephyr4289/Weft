// weft-fanout-rn.test.ts — @weft/react-native fan-out binding unit suite
//
// WHY EXISTS: RFC 0004 fan-out heddles need the same contracts pinned as
// useWeftDraw (rAF fallback + optional frame-callback registrar, consistent
// idempotent disposers from both branches, JS-thread honesty note), PLUS
// the multi-consumer property: N hooks on one broadcaster are N independent
// readers. Uses a stubbed frame-callback registrar — no
// react-native-reanimated dependency needed in CI.
//
// Environment tag: node-vitest (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { WeftFanoutBroadcaster, type FanoutClaim } from '@weft/core';
import { useWeftFanoutDraw } from '../src/index';

// Handle-keyed frame registry: cancel removes the pending callback, exactly
// like a real browser (a cancelled callback never fires).
const rafMap = new Map<number, FrameRequestCallback>();
let rafHandle = 0;
const cancelled = new Set<number>();

function pump(n = 1): void {
  for (let i = 0; i < n; i++) {
    const q = [...rafMap.values()];
    rafMap.clear();
    for (const cb of q) cb(16.7 * (i + 1));
  }
}

beforeEach(() => {
  rafMap.clear();
  rafHandle = 0;
  cancelled.clear();
  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    const h = ++rafHandle;
    rafMap.set(h, cb);
    return h;
  });
  vi.stubGlobal('cancelAnimationFrame', (h: number) => {
    rafMap.delete(h);
    cancelled.add(h);
  });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

function publishFrame(b: WeftFanoutBroadcaster, seq: number): void {
  const v = b.begin();
  for (let i = 0; i < v.length; i++) v[i] = (seq * 131 + i * 37) % 9973;
  b.publish();
}

describe('useWeftFanoutDraw', () => {
  it('rAF fallback: claims and draws inside the frame callback', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: number[] = [];
    const seenSeq: number[] = [];
    const dispose = useWeftFanoutDraw(b, (floats, claim) => {
      seen.push(floats[0]);
      seenSeq.push(claim.seq);
    });

    publishFrame(b, 1);
    pump(1);
    expect(seen).toEqual([(1 * 131) % 9973]);
    expect(seenSeq).toEqual([1]);

    dispose();
  });

  it('registerFrameCallback branch: the registrar drives the loop; disposer is idempotent', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: number[] = [];
    let registered: (() => void) | undefined;
    const registrar = (cb: () => void) => {
      registered = cb;
      return () => {
        registered = undefined;
      };
    };
    const dispose = useWeftFanoutDraw(b, (floats) => seen.push(floats[0]), registrar);

    expect(rafMap.size).toBe(0); // no rAF loop in this branch
    publishFrame(b, 1);
    registered?.();
    expect(seen).toEqual([(1 * 131) % 9973]);

    dispose();
    dispose(); // idempotent
    expect(registered).toBeUndefined();
    const after = seen.length;
    publishFrame(b, 2);
    registered?.(); // unregistered — nothing fires
    expect(seen.length).toBe(after);
  });

  it('multi-consumer: two hooks on one broadcaster are independent readers', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seenA: number[] = [];
    const seenB: number[] = [];
    const disposeA = useWeftFanoutDraw(b, (f) => seenA.push(f[0]));
    const disposeB = useWeftFanoutDraw(b, (f) => seenB.push(f[0]));

    publishFrame(b, 1);
    pump(1);
    expect(seenA).toEqual([(1 * 131) % 9973]);
    expect(seenB).toEqual([(1 * 131) % 9973]);

    disposeA();
    publishFrame(b, 2);
    pump(1);
    expect(seenA.length).toBe(1); // A is dead
    expect(seenB).toEqual([(1 * 131) % 9973, (2 * 131) % 9973]); // B is alive

    disposeB();
  });

  it('the claim record is identity-stable across frames (zero alloc through the binding)', () => {
    const b = new WeftFanoutBroadcaster(8);
    const records: FanoutClaim[] = [];
    const dispose = useWeftFanoutDraw(b, (_f, claim) => records.push(claim));
    for (let f = 1; f <= 5; f++) {
      publishFrame(b, f);
      pump(1);
    }
    expect(records.length).toBe(5);
    for (const r of records) expect(r).toBe(records[0]); // one mutated record
    expect(records[0].seq).toBe(5);
    dispose();
  });

  it('rAF fallback disposer is idempotent and cancels exactly once', () => {
    const b = new WeftFanoutBroadcaster(8);
    const dispose = useWeftFanoutDraw(b, () => {});
    pump(1);
    dispose();
    expect(cancelled.size).toBe(1);
    dispose();
    expect(cancelled.size).toBe(1); // no double cancel
    const seen: number[] = [];
    const dispose2 = useWeftFanoutDraw(b, (f) => seen.push(f[0]));
    publishFrame(b, 1);
    pump(2);
    dispose2();
    expect(seen.length).toBe(2); // rAF self-scheduling loop, exactly one per tick
  });
});
