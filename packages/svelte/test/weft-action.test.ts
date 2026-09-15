// weft-action.test.ts — @weft/svelte binding unit suite
//
// WHY EXISTS: The framework Heddles shipped with zero tests (D-11 gap), and
// the action was missing its `update` lifecycle handler — Svelte calls
// update() when params change; the old code drew from a stale Weft forever.
// Svelte actions are plain functions, so this suite invokes the action
// directly against a fake canvas node: no Svelte compiler required.
//
// Environment tag: node-vitest (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { Weft } from '@weft/core';
import { weftCanvas, type WeftActionParams } from '../src/index';

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

const fakeCtx = {} as unknown as CanvasRenderingContext2D;

function fakeCanvas(): HTMLCanvasElement {
  return { getContext: () => fakeCtx } as unknown as HTMLCanvasElement;
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

describe('weftCanvas action', () => {
  it('claims and reads inside the frame callback; draw receives the payload', () => {
    const weft = new Weft(16);
    const seen: number[] = [];
    const params: WeftActionParams = {
      weft,
      draw: (_ctx, buf) => seen.push(buf[0]),
    };
    const action = weftCanvas(fakeCanvas(), params)!;

    weft.wBegin()[0] = 0x66;
    weft.publish(1, 16);
    pump(1);
    expect(seen).toEqual([0x66]);
    expect(weft.tClaim()).toBe(1n);
    action.destroy();
  });

  it('update() hot-swaps weft + draw params (the missing-handler fix)', () => {
    const weftA = new Weft(16);
    const weftB = new Weft(16);
    weftA.wBegin()[0] = 1;
    weftA.publish(1, 16);
    weftB.wBegin()[0] = 2;
    weftB.publish(1, 16);

    const seen: number[] = [];
    let current = weftA;
    const params: WeftActionParams = {
      weft: current,
      draw: (_ctx, buf) => seen.push(buf[0]),
    };
    const canvas = fakeCanvas();
    const action = weftCanvas(canvas, params)!;

    pump(1);
    expect(seen).toEqual([1]);
    expect(weftA.tClaim()).toBe(1n);
    expect(weftB.tClaim()).toBe(0n);

    // Param change: Svelte would call update() with the new params object.
    current = weftB;
    action.update({ weft: current, draw: (_ctx, buf) => seen.push(buf[0]) });
    pump(1);
    expect(seen).toEqual([1, 2]);
    expect(weftB.tClaim()).toBe(1n); // reads now come from weftB
    expect(cancelled.size).toBe(0); // the loop was never restarted

    action.destroy();
  });

  it('destroy() cancels the loop exactly once; the loop is dead afterwards', () => {
    const weft = new Weft(16);
    const action = weftCanvas(fakeCanvas(), { weft, draw: () => {} })!;
    pump(1);
    action.destroy();
    expect(cancelled.size).toBe(1);
    const claims = Number(weft.tClaim());
    pump(3);
    expect(Number(weft.tClaim())).toBe(claims);
  });

  it('returns undefined when the canvas has no 2D context (graceful no-op)', () => {
    const weft = new Weft(16);
    const noCtxCanvas = { getContext: () => null } as unknown as HTMLCanvasElement;
    const action = weftCanvas(noCtxCanvas, { weft, draw: () => {} });
    expect(action).toBeUndefined();
    expect(rafMap.size).toBe(0); // nothing was scheduled
  });
});
