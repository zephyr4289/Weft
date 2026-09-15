// weft-rn.test.ts — @weft/react-native binding unit suite
//
// WHY EXISTS: The framework Heddles shipped with zero tests (D-11 gap), and
// the RN binding had (a) a 'worklet' directive inside a closure capturing a
// class instance (cannot workletize — would fail at runtime with Reanimated)
// and (b) inconsistent return values (disposer from the rAF branch, undefined
// from the useFrameCallback branch). This suite pins the corrected contract
// using a stubbed frame-callback registrar — no react-native-reanimated
// dependency needed in CI.
//
// Environment tag: node-vitest (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { Weft } from '@weft/core';
import { useWeftDraw } from '../src/index';

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

describe('useWeftDraw', () => {
  it('rAF fallback: claims and reads inside the frame callback; draw receives payload', () => {
    const weft = new Weft(16);
    const seen: number[] = [];
    const dispose = useWeftDraw(weft, (buf) => seen.push(buf[0]));

    weft.wBegin()[0] = 0x44;
    weft.publish(1, 16);
    pump(1);
    expect(seen).toEqual([0x44]);
    expect(weft.tClaim()).toBe(1n);
    dispose();
  });

  it('rAF fallback: disposer stops the loop and is idempotent', () => {
    const weft = new Weft(16);
    const dispose = useWeftDraw(weft, () => {});
    pump(1);
    dispose();
    expect(cancelled.size).toBeGreaterThanOrEqual(1);
    const claims = Number(weft.tClaim());
    pump(3);
    expect(Number(weft.tClaim())).toBe(claims); // loop is dead
    dispose(); // idempotent — must not throw or double-cancel
  });

  it('registerFrameCallback branch: frames drive reads; disposer unregisters once', () => {
    const weft = new Weft(16);
    const seen: number[] = [];
    let registered: ((() => void) | null) = null;
    let unregistered = 0;

    const dispose = useWeftDraw(weft, (buf) => seen.push(buf[0]), (cb) => {
      registered = cb;
      return () => {
        unregistered++;
      };
    });

    expect(rafMap.size).toBe(0); // the rAF fallback is NOT used in this branch
    expect(registered).not.toBeNull();

    weft.wBegin()[0] = 0x11;
    weft.publish(1, 16);
    registered!(); // drive one frame through the registered callback
    expect(seen).toEqual([0x11]);
    expect(weft.tClaim()).toBe(1n);

    dispose();
    expect(unregistered).toBe(1); // exactly one unregister
    dispose(); // idempotent
    expect(unregistered).toBe(1);
  });

  it('returns a no-op disposer when no frame clock exists (SSR/host without rAF)', () => {
    vi.unstubAllGlobals();
    const had = (globalThis as Record<string, unknown>).requestAnimationFrame;
    delete (globalThis as Record<string, unknown>).requestAnimationFrame;
    try {
      const weft = new Weft(16);
      const dispose = useWeftDraw(weft, () => {});
      expect(() => dispose()).not.toThrow();
      expect(Number(weft.tClaim())).toBe(0); // nothing scheduled, nothing claimed
    } finally {
      if (had) (globalThis as Record<string, unknown>).requestAnimationFrame = had;
    }
  });
});
