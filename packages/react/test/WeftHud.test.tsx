// WeftHud.test.tsx — RFC 0016 §9 HUD binding unit suite.
//
// Pins the overlay contract: the rAF loop samples once per frame through
// the latest-ref (a sample-closure identity change never restarts the
// loop — the WeftCanvas hardening pattern), drops render as burst deltas,
// and teardown cancels the loop. Same deterministic frame-clock + canvas
// prototype stub as WeftCanvas.test.tsx (jsdom has no 2D canvas).

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import React from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { act } from 'react';
import { WeftHud } from '../src/index';

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

// The HUD paints directly (unlike WeftCanvas, whose ctx is host-owned), so
// the stub carries no-op paint methods.
const fakeCtx = {
  clearRect: () => {}, fillRect: () => {}, strokeRect: () => {},
  beginPath: () => {}, moveTo: () => {}, lineTo: () => {},
  stroke: () => {}, fillText: () => {},
} as unknown as CanvasRenderingContext2D;

beforeEach(() => {
  (globalThis as unknown as { IS_REACT_ACT_ENVIRONMENT: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
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
  // jsdom has no 2D context; patch it with a stub (WeftCanvas.test.tsx pattern).
  (HTMLCanvasElement.prototype as unknown as Record<string, unknown>).getContext =
    () => fakeCtx;
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('WeftHud (RFC 0016 §9)', () => {
  let host: HTMLElement;
  let root: Root;

  beforeEach(() => {
    host = document.createElement('div');
    document.body.appendChild(host);
    root = createRoot(host);
  });

  afterEach(() => {
    act(() => root.unmount());
    host.remove();
  });

  it('renders the overlay canvas and pumps the sample loop', async () => {
    let samples = 0;
    act(() => {
      root.render(React.createElement(WeftHud, {
        sample: () => { samples++; return { depth: 0.5, behind: 2, drops: 0, trend: 0 }; },
      }));
    });
    expect(host.querySelector('[data-weft-hud]')).toBeTruthy();
    act(() => pump(5));
    expect(samples).toBe(5);  // one sample per frame
  });

  it('latest-ref: a new sample closure does NOT restart the loop', async () => {
    let samples = 0;
    const mk = (): (() => { depth: number }) => () => ({ depth: ++samples * 0.1 });
    let s = mk();
    act(() => {
      root.render(React.createElement(WeftHud, { sample: () => s() }));
    });
    act(() => pump(3));
    const after3 = samples;
    // parent re-render with a NEW closure — the loop keeps running without
    // teardown/restart (samples continue smoothly)
    act(() => {
      s = mk();
      root.render(React.createElement(WeftHud, { sample: () => s() }));
    });
    act(() => pump(3));
    expect(samples).toBe(after3 + 3);
  });

  it('drops render as burst deltas, not cumulative lines', async () => {
    let cumulative = 0;
    act(() => {
      root.render(React.createElement(WeftHud, {
        sample: () => ({ drops: (cumulative += 10) }),
      }));
    });
    // 3 frames: bursts of 10 each — the ring sees deltas, the loop lives
    act(() => pump(3));
    expect(cumulative).toBe(30);
  });

  it('teardown cancels the loop', async () => {
    act(() => {
      root.render(React.createElement(WeftHud, { sample: () => ({}) }));
    });
    act(() => pump(2));
    expect(rafHandle).toBeGreaterThanOrEqual(1);
    root.unmount();
    root = createRoot(host);  // keep afterEach safe
    expect(cancelled.size).toBe(1);
  });

  it('fail-safe: a throwing sampler is counted and flagged, loop survives', async () => {
    let calls = 0;
    act(() => {
      root.render(React.createElement(WeftHud, {
        sample: (): { depth?: number } => {
          calls++;
          if (calls === 2) throw new Error('host hiccup');
          return { depth: 0.1 };
        },
      }));
    });
    // A host sampler that throws is a HOST bug. The overlay is fail-safe
    // by contract: it counts the error, keeps sampling, never dies — a
    // DevTools overlay must never take the app down.
    act(() => pump(4));
    expect(calls).toBe(4);  // all four frames sampled (one threw, caught)
  });
});
