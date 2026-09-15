// weft-fanout-action.test.ts — @weft/svelte fan-out binding unit suite
//
// WHY EXISTS: RFC 0004 fan-out heddles need the same contracts pinned as
// weftCanvas (claims + draws inside the frame callback, update() hot-swap,
// exactly-one cancel on destroy), PLUS the multi-consumer property: N
// actions attached to canvases on one broadcaster are N independent
// readers. Svelte actions are plain functions, so this suite invokes the
// action directly against a fake canvas node — no Svelte compiler
// required. Environment tag: node-vitest (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { WeftFanoutBroadcaster, type FanoutClaim } from '@weft/core';
import { weftFanoutCanvas, type WeftFanoutActionParams } from '../src/index';

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

function publishFrame(b: WeftFanoutBroadcaster, seq: number): void {
  const v = b.begin();
  for (let i = 0; i < v.length; i++) v[i] = (seq * 131 + i * 37) % 9973;
  b.publish();
}

describe('weftFanoutCanvas action', () => {
  it('claims and draws inside the frame callback; draw receives buffer + claim', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: number[] = [];
    const seenSeq: number[] = [];
    const params: WeftFanoutActionParams = {
      broadcaster: b,
      draw: (_ctx, floats, claim) => {
        seen.push(floats[0]);
        seenSeq.push(claim.seq);
      },
    };
    const action = weftFanoutCanvas(fakeCanvas(), params);

    publishFrame(b, 1);
    pump(1);
    expect(seen).toEqual([(1 * 131) % 9973]);
    expect(seenSeq).toEqual([1]);

    action?.destroy();
  });

  it('update() hot-swaps the draw closure; the loop never restarts', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: string[] = [];
    let current = 'first';
    const action = weftFanoutCanvas(fakeCanvas(), {
      broadcaster: b,
      draw: () => seen.push(current),
    });

    pump(1);
    expect(seen).toEqual(['first']);
    expect(cancelled.size).toBe(0);

    action?.update({ broadcaster: b, draw: () => seen.push(current) });
    current = 'second';
    pump(1);
    expect(seen).toEqual(['first', 'second']);
    expect(cancelled.size).toBe(0); // no restart on param change

    action?.destroy();
  });

  it('destroy cancels the loop exactly once; a cancelled callback never fires', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: number[] = [];
    const action = weftFanoutCanvas(fakeCanvas(), {
      broadcaster: b,
      draw: (_ctx, floats) => seen.push(floats[0]),
    });
    publishFrame(b, 1);
    pump(1);
    expect(seen.length).toBe(1);

    action?.destroy();
    expect(cancelled.size).toBe(1);
    const after = seen.length;
    publishFrame(b, 2);
    pump(3);
    expect(seen.length).toBe(after); // loop is dead
  });

  it('multi-consumer: two actions on one broadcaster are independent readers', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seenA: number[] = [];
    const seenB: number[] = [];
    const actionA = weftFanoutCanvas(fakeCanvas(), {
      broadcaster: b,
      draw: (_ctx, f) => seenA.push(f[0]),
    });
    const actionB = weftFanoutCanvas(fakeCanvas(), {
      broadcaster: b,
      draw: (_ctx, f) => seenB.push(f[0]),
    });

    publishFrame(b, 1);
    pump(1);
    expect(seenA).toEqual([(1 * 131) % 9973]);
    expect(seenB).toEqual([(1 * 131) % 9973]);

    actionA?.destroy();
    publishFrame(b, 2);
    pump(1);
    expect(seenA.length).toBe(1); // A is dead
    expect(seenB).toEqual([(1 * 131) % 9973, (2 * 131) % 9973]); // B is alive

    actionB?.destroy();
  });

  it('the claim record is identity-stable across frames (zero alloc through the binding)', () => {
    const b = new WeftFanoutBroadcaster(8);
    const records: FanoutClaim[] = [];
    const action = weftFanoutCanvas(fakeCanvas(), {
      broadcaster: b,
      draw: (_ctx, _f, claim) => records.push(claim),
    });
    for (let f = 1; f <= 5; f++) {
      publishFrame(b, f);
      pump(1);
    }
    expect(records.length).toBe(5);
    for (const r of records) expect(r).toBe(records[0]); // one mutated record
    expect(records[0].seq).toBe(5);
    action?.destroy();
  });
});
