// trend.test.ts — RFC 0020 D-series, TS port (mirrors core/c/weft_trend_test.c).
// Pinned parity vector: xorshift verdict stream 0x11187b9a02b378ef.

import { describe, it, expect } from 'vitest';
import {
  trendInit, trendConfigure, trendObserve, trendPack, TrendVerdict,
  type TrendOut,
} from '../src/trend';

describe('RFC 0020 trend estimator (TS port)', () => {
  it('D1: constant plateau -> STABLE, level converges', () => {
    const t = trendInit();
    let o: TrendOut = { verdict: 0, pred_raw: 0, skip_n: 0 };
    for (let i = 0; i < 30; i++) trendObserve(t, 5, o);
    expect(o.verdict).toBe(TrendVerdict.Stable);
    expect(t.level_q16).toBeGreaterThanOrEqual(294912);
    expect(t.level_q16).toBeLessThanOrEqual(360448);
    expect(t.slope_q16).toBeLessThan(32768);
  });

  it('D2: predictive lead — RISING at raw 4, skip 6', () => {
    const t = trendInit();
    const o: TrendOut = { verdict: 0, pred_raw: 0, skip_n: 0 };
    for (let i = 0; i < 12; i++) trendObserve(t, 2, o);
    expect(o.verdict).toBe(TrendVerdict.Stable);
    let firstRising = -1;
    let firstSkip = 0;
    for (let b = 3; b <= 10; b++) {
      trendObserve(t, b, o);
      if (o.verdict === TrendVerdict.Rising && firstRising < 0) {
        firstRising = b;
        firstSkip = o.skip_n;
      }
    }
    expect(firstRising).toBe(4);
    expect(firstSkip).toBe(6);
  });

  it('D4: burst — jump 62 -> BURST, skip capped 63', () => {
    const t = trendInit();
    const o: TrendOut = { verdict: 0, pred_raw: 0, skip_n: 0 };
    trendObserve(t, 2, o);
    trendObserve(t, 2, o);
    trendObserve(t, 2, o);
    const v = trendObserve(t, 64, o);
    expect(v).toBe(TrendVerdict.Burst);
    expect(o.skip_n).toBe(63);
  });

  it('D5: pinned xorshift verdict stream (parity vector)', () => {
    const t = trendInit();
    const o: TrendOut = { verdict: 0, pred_raw: 0, skip_n: 0 };
    let state = 0x00C0FFEE | 0;
    const hex: string[] = [];
    for (let i = 0; i < 2000; i++) {
      state = (state ^ ((state << 13) | 0)) | 0;
      state = (state ^ (state >>> 17)) | 0;
      state = (state ^ ((state << 5) | 0)) | 0;
      trendObserve(t, (state >>> 0) % 64, o);
      hex.push(trendPack(o).toString(16).padStart(2, '0'));
    }
    const stream = hex.join('');
    let h = 0xcbf29ce484222325n;
    for (let i = 0; i < stream.length; i++) {
      h ^= BigInt(stream.charCodeAt(i));
      h *= 0x100000001b3n;
      h &= 0xffffffffffffffffn;
    }
    expect(h).toBe(0x11187b9a02b378efn);
    expect(t.t_samples).toBe(2000);
  });

  it('D7: hand-computed Q16 vectors', () => {
    const t = trendInit();
    const o: TrendOut = { verdict: 0, pred_raw: 0, skip_n: 0 };
    trendObserve(t, 3, o);
    expect(t.level_q16).toBe(3 << 16);
    expect(t.slope_q16).toBe(0);
    trendObserve(t, 4, o);
    expect(t.level_q16).toBe(208896);   // 3.1875
    expect(t.slope_q16).toBe(24576);    // 0.375
    expect(o.verdict).toBe(TrendVerdict.Stable);
    expect(o.pred_raw).toBe(6);         // 3.1875 + 0.375*8 = 6.1875
  });

  it('D6: configuration clamps', () => {
    const t = trendInit();
    trendConfigure(t, 999, 0, 0, 1 << 21, 1 << 21);
    expect(t.alpha_q8).toBe(256);
    expect(t.beta_q8).toBe(1);
    expect(t.burst_delta).toBe(1);
  });
});
