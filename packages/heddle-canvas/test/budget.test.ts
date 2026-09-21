// budget.test.ts — the frame-budget ledger math. The 240 Hz target is
// 4,166.67 µs; the ledger must count violations exactly and compute
// percentiles deterministically over its window. A virtual clock is
// injected so the math is exact (production uses performance.now).

import { describe, expect, it } from 'vitest';
import { FrameBudgetLedger } from '../src/loop/budget.ts';

/** A ledger plus a virtual-clock frame driver: frame(t0, dt). */
function makeLedger(windowSize = 8) {
  let now = 0;
  const l = new FrameBudgetLedger(240, windowSize, () => now);
  return {
    l,
    frame: (dt: number) => {
      now = 1000;
      l.begin();
      now = 1000 + dt;
      l.end();
    },
  };
}

describe('FrameBudgetLedger', () => {
  it('240 Hz target is 4166.67 µs (the mandate number)', () => {
    const l = new FrameBudgetLedger(240);
    expect(l.targetMicros).toBeCloseTo(4166.6667, 3);
  });

  it('counts violations exactly at the boundary', () => {
    const { l, frame } = makeLedger();
    frame(4000); // under
    frame(4166); // under (4166 < 4166.67)
    frame(4167); // over
    frame(9000); // over
    expect(l.frames).toBe(4);
    expect(l.budgetViolations).toBe(2);
    expect(l.worst).toBe(9000);
  });

  it('percentiles over the ring window (diagnostic path)', () => {
    let now = 0;
    const l = new FrameBudgetLedger(240, 16, () => now);
    for (let i = 0; i < 16; i++) {
      now = 0;
      l.begin();
      now = (i + 1) * 100;
      l.end();
    }
    const p = l.percentiles();
    expect(p.p50).toBe(900); // sorted index floor(0.5*16)=8 of 100..1600
    expect(p.p95).toBe(1600);
    expect(p.p99).toBe(1600);
  });

  it('ring wraps without losing the violation count', () => {
    let now = 0;
    const l = new FrameBudgetLedger(240, 4, () => now); // tiny window
    for (let i = 0; i < 100; i++) {
      now = 0;
      l.begin();
      now = i % 10 === 0 ? 5000 : 1000;
      l.end();
    }
    expect(l.frames).toBe(4); // window size
    expect(l.budgetViolations).toBe(10); // lifetime count
  });

  it('snapshot carries the evidence fields the report quotes', () => {
    let now = 0;
    const l = new FrameBudgetLedger(240, 8, () => now);
    now = 0;
    l.begin();
    now = 1000;
    l.end();
    const snap = l.snapshot({ backend: 'null' });
    expect(snap['target_micros']).toBe(4166.67);
    expect(snap['frames']).toBe(1);
    expect(snap['backend']).toBe('null');
  });
});
