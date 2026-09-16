// WeftFanoutCanvas.test.tsx — @weft/react fan-out binding unit suite
//
// WHY EXISTS: RFC 0004 fan-out heddles need the same Draw-phase contract
// pinned as WeftCanvas (claim + read inside the frame callback only,
// latest-ref draw identity survival, exactly-one teardown), PLUS the
// multi-consumer property this binding exists for: N components on one
// broadcaster are N independent readers with their own buffers and claim
// records. Mirrors the WeftCanvas harness: jsdom + stubbed 2d context +
// deterministic rAF pump. Environment tag: node-vitest+jsdom (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import React from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { act } from 'react';
import { WeftFanoutBroadcaster, type FanoutClaim } from '@weft/core';
import { WeftFanoutCanvas } from '../src/index';

// ---------------------------------------------------------------------------
// Deterministic frame-clock + canvas stubs
// ---------------------------------------------------------------------------

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
  (HTMLCanvasElement.prototype as unknown as Record<string, unknown>).getContext =
    () => fakeCtx;
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

function mountFanout(
  broadcaster: WeftFanoutBroadcaster,
  draw: (floats: Float32Array, claim: FanoutClaim) => void
): { container: HTMLElement; root: Root } {
  const container = document.createElement('div');
  document.body.appendChild(container);
  const root = createRoot(container);
  act(() => {
    root.render(
      React.createElement(WeftFanoutCanvas, {
        broadcaster,
        draw: (_ctx: CanvasRenderingContext2D, floats: Float32Array, claim: FanoutClaim) => draw(floats, claim),
        width: 64,
        height: 64,
      })
    );
  });
  return { container, root };
}

function publishFrame(b: WeftFanoutBroadcaster, seq: number, payloadFloats: number): void {
  const v = b.begin();
  for (let i = 0; i < payloadFloats; i++) v[i] = (seq * 131 + i * 37) % 9973;
  b.publish();
}

describe('WeftFanoutCanvas', () => {
  it('renders a canvas element', () => {
    const b = new WeftFanoutBroadcaster(8);
    const { root, container } = mountFanout(b, () => {});
    expect(container.querySelector('canvas')).not.toBeNull();
    act(() => root.unmount());
  });

  it('claims and draws inside the frame callback (draw receives the reader buffer + claim)', () => {
    const b = new WeftFanoutBroadcaster(8);
    publishFrame(b, 1, 8);
    const seen: number[] = [];
    const seenSeq: number[] = [];
    const { root } = mountFanout(b, (floats, claim) => {
      seen.push(floats[0]);
      seenSeq.push(claim.seq);
    });
    pump(1);
    expect(seen).toEqual([(1 * 131) % 9973]);
    expect(seenSeq).toEqual([1]);
    expect(seen.length).toBe(1);

    act(() => root.unmount());
  });

  it('multi-canvas: two components on one broadcaster are two independent readers', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seenA: number[] = [];
    const seenB: number[] = [];
    const recA = mountFanout(b, (f, _c) => seenA.push(f[0]));
    const recB = mountFanout(b, (f, _c) => seenB.push(f[0]));

    publishFrame(b, 1, 8);
    pump(1);
    expect(seenA).toEqual([(1 * 131) % 9973]);
    expect(seenB).toEqual([(1 * 131) % 9973]); // both observed frame 1

    publishFrame(b, 2, 8);
    pump(1);
    expect(seenA).toEqual([(1 * 131) % 9973, (2 * 131) % 9973]);
    expect(seenB).toEqual([(1 * 131) % 9973, (2 * 131) % 9973]);

    act(() => recA.root.unmount());
    // A's teardown does not affect B's loop: B still observes new frames.
    publishFrame(b, 3, 8);
    pump(1);
    expect(seenA.length).toBe(2); // A is dead
    expect(seenB).toEqual([
      (1 * 131) % 9973,
      (2 * 131) % 9973,
      (3 * 131) % 9973,
    ]); // B is alive
    act(() => recB.root.unmount());
  });

  it('survives draw identity changes without restarting the loop', () => {
    const b = new WeftFanoutBroadcaster(8);
    publishFrame(b, 1, 8);
    let current = 'first';
    const seen: string[] = [];

    const container = document.createElement('div');
    document.body.appendChild(container);
    const root = createRoot(container);
    act(() => {
      root.render(React.createElement(WeftFanoutCanvas, { broadcaster: b, draw: () => seen.push(current) }));
    });
    pump(1);
    expect(seen).toEqual(['first']);
    expect(cancelled.size).toBe(0);

    act(() => {
      root.render(React.createElement(WeftFanoutCanvas, { broadcaster: b, draw: () => seen.push(current) }));
    });
    current = 'second';
    pump(1);
    expect(seen).toEqual(['first', 'second']);
    expect(cancelled.size).toBe(0); // THE hardening assertion

    act(() => root.unmount());
    expect(cancelled.size).toBe(1);
    const after = seen.length;
    pump(3);
    expect(seen.length).toBe(after); // loop is dead
  });

  it('the claim record is identity-stable across frames (zero alloc through the binding)', () => {
    const b = new WeftFanoutBroadcaster(8);
    const records: FanoutClaim[] = [];
    const { root } = mountFanout(b, (_f, claim) => records.push(claim));
    for (let f = 1; f <= 5; f++) {
      publishFrame(b, f, 8);
      pump(1);
    }
    expect(records.length).toBe(5);
    for (const r of records) expect(r).toBe(records[0]); // one mutated record
    expect(records[0].seq).toBe(5);
    act(() => root.unmount());
  });

  it('latest-wins through the binding: each pump draws the then-freshest frame', () => {
    const b = new WeftFanoutBroadcaster(8);
    const seen: number[] = [];
    const { root } = mountFanout(b, (f, _c) => seen.push(f[0]));

    publishFrame(b, 1, 8);
    pump(1);
    publishFrame(b, 2, 8);
    pump(1);
    expect(seen).toEqual([(1 * 131) % 9973, (2 * 131) % 9973]);

    act(() => root.unmount());
  });
});
