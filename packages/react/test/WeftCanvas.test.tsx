// WeftCanvas.test.tsx — @weft/react binding unit suite
//
// WHY EXISTS: The framework Heddles shipped with zero tests (D-11 gap).
// This suite pins the Draw-phase binding contract: the rAF loop claims +
// reads inside the frame callback only, the loop survives draw-closure
// identity changes without restart (2026-09 hardening), and teardown
// cancels the loop exactly once.
//
// jsdom provides no 2D canvas implementation, so getContext is patched with
// a stub; requestAnimationFrame is replaced with a deterministic manual
// pump. Environment tag: node-vitest+jsdom (sandbox).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import React from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { act } from 'react';
import { Weft } from '@weft/core';
import { WeftCanvas } from '../src/index';

// ---------------------------------------------------------------------------
// Deterministic frame-clock + canvas stubs
// ---------------------------------------------------------------------------

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

function cancelCount(): number {
  return cancelled.size;
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
  // jsdom has no 2D context; patch it with a stub.
  (HTMLCanvasElement.prototype as unknown as Record<string, unknown>).getContext =
    () => fakeCtx;
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

// ---------------------------------------------------------------------------
// Render helper (react-dom/client + act; no testing-library dependency)
// ---------------------------------------------------------------------------

function render(weft: Weft, draw: (buf: Uint8Array) => void): { container: HTMLElement; root: Root } {
  const container = document.createElement('div');
  document.body.appendChild(container);
  const root = createRoot(container);
  act(() => {
    root.render(
      React.createElement(WeftCanvas, {
        weft,
        draw: (_ctx: CanvasRenderingContext2D, buf: Uint8Array) => draw(buf),
        width: 64,
        height: 64,
      })
    );
  });
  return { container, root };
}

// ---------------------------------------------------------------------------
// Suite
// ---------------------------------------------------------------------------

describe('WeftCanvas', () => {
  it('renders a canvas element', () => {
    const weft = new Weft(64);
    const { root, container } = render(weft, () => {});
    expect(container.querySelector('canvas')).not.toBeNull();
    act(() => root.unmount());
  });

  it('claims and reads the live buffer inside the frame callback (draw receives payload)', () => {
    const weft = new Weft(16);
    const cursor = weft.wBegin();
    cursor[0] = 0x7f;
    weft.publish(1, 16);

    const seen: number[] = [];
    const { root } = render(weft, (buf) => seen.push(buf[0]));

    pump(1);
    expect(seen).toEqual([0x7f]);
    expect(weft.tClaim()).toBe(1n);

    act(() => root.unmount());
  });

  it('survives draw identity changes without restarting the loop (no teardown on rerender)', () => {
    const weft = new Weft(16);
    weft.publish(1, 16);
    let current = 'first';
    const seen: string[] = [];

    const container = document.createElement('div');
    document.body.appendChild(container);
    const root = createRoot(container);
    act(() => {
      root.render(React.createElement(WeftCanvas, { weft, draw: () => seen.push(current) }));
    });
    pump(1);
    expect(seen).toEqual(['first']);
    expect(cancelCount()).toBe(0);

    // Rerender with a DIFFERENT draw closure (inline lambda identity) —
    // the loop must NOT be torn down: zero cancels, still one live handle.
    act(() => {
      root.render(React.createElement(WeftCanvas, { weft, draw: () => seen.push(current) }));
    });
    current = 'second';
    pump(1);
    expect(seen).toEqual(['first', 'second']);
    expect(cancelCount()).toBe(0); // THE hardening assertion

    act(() => root.unmount());
    expect(cancelCount()).toBe(1); // exactly one teardown cancel
    const after = seen.length;
    pump(3);
    expect(seen.length).toBe(after); // loop is dead
  });

  it('restarts the loop only when the Weft instance changes', () => {
    const weftA = new Weft(16);
    const weftB = new Weft(16);
    weftA.publish(1, 16);
    weftB.wBegin()[0] = 0x33;
    weftB.publish(1, 16);

    const container = document.createElement('div');
    document.body.appendChild(container);
    const root = createRoot(container);
    let weft = weftA;
    act(() => {
      root.render(React.createElement(WeftCanvas, { weft, draw: () => {} }));
    });
    pump(1);
    expect(weftA.tClaim()).toBe(1n);
    expect(cancelCount()).toBe(0);

    weft = weftB;
    act(() => {
      root.render(React.createElement(WeftCanvas, { weft, draw: () => {} }));
    });
    expect(cancelCount()).toBe(1); // legitimate restart on weft identity change
    pump(1);
    expect(weftB.tClaim()).toBe(1n);

    act(() => root.unmount());
  });

  it('draws the freshest frame after additional publishes (latest-wins through the binding)', () => {
    const weft = new Weft(16);
    const seen: number[] = [];
    const { root } = render(weft, (buf) => seen.push(buf[0]));

    weft.wBegin()[0] = 1;
    weft.publish(1, 16);
    pump(1);
    weft.wBegin()[0] = 2;
    weft.publish(2, 16);
    pump(1);
    expect(seen).toEqual([1, 2]); // each pump drew the then-freshest frame

    act(() => root.unmount());
  });
});
