// governor.test.ts — RFC-0009 FreshnessGovernor conformance (G-series).
// Mirrors core/c/governor_test.c and core/rust/tests/governor_test.rs so
// the TS port is pinned by the same semantics (G5 closes the loop with the
// shared-trace xlang fixture).

import { describe, it, expect } from 'vitest';
import {
  FreshnessGovernor,
  GovernorActionKind,
  GOVERNOR_DEFAULTS,
} from '../src/governor';

/// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG.
/// Identical step in C/Rust/TS (G5's trace generator).
function xorshift32(state: number): number {
  let x = state >>> 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

describe('RFC-0009 FreshnessGovernor', () => {
  it('G1: ladder — every behind in 0..64 maps to the documented action', () => {
    const gov = new FreshnessGovernor();
    // now advances past the cooldown every step so Reseed is never suppressed
    // (G1 tests the LADDER, not the rate limit).
    for (let behind = 0; behind <= 64; behind++) {
      const a = gov.step(behind, behind * 1000);
      if (behind <= GOVERNOR_DEFAULTS.fastPathBehind) {
        expect(a.kind).toBe(GovernorActionKind.FastPath);
        expect(a.skipN).toBe(0);
      } else if (behind <= GOVERNOR_DEFAULTS.skipBehind) {
        expect(a.kind).toBe(GovernorActionKind.Skip);
        expect(a.skipN).toBe(behind - GOVERNOR_DEFAULTS.fastPathBehind);
      } else if (behind <= GOVERNOR_DEFAULTS.snapshotBehind) {
        expect(a.kind).toBe(GovernorActionKind.Snapshot);
        expect(a.skipN).toBe(0);
      } else {
        expect(a.kind).toBe(GovernorActionKind.Reseed);
        expect(a.skipN).toBe(0);
      }
    }
  });

  it('G2: monotone — larger behind never yields a fresher-class action', () => {
    const gov = new FreshnessGovernor();
    let prevClass = -1;
    for (let behind = 0; behind <= 64; behind++) {
      const a = gov.step(behind, behind * 1000);
      expect(a.kind).toBeGreaterThanOrEqual(prevClass);
      prevClass = a.kind;
    }
  });

  it('G3: reseed flap — 10k random spikes, at most ceil(10k / cooldown) Reseeds', () => {
    const gov = new FreshnessGovernor();
    let state = 0x00c0ffee;
    let reseeds = 0;
    let lastReseedAt = -1;
    const STEPS = 10_000;
    for (let i = 0; i < STEPS; i++) {
      state = xorshift32(state);
      const behind = state % 128; // spikes well past snapshotBehind=16
      const a = gov.step(behind, i); // 1 ms per step
      if (a.kind === GovernorActionKind.Reseed) {
        reseeds++;
        if (lastReseedAt >= 0) {
          expect(i - lastReseedAt).toBeGreaterThanOrEqual(
            GOVERNOR_DEFAULTS.reseedCooldownMs
          );
        }
        lastReseedAt = i;
      }
    }
    const bound = Math.ceil(STEPS / GOVERNOR_DEFAULTS.reseedCooldownMs);
    expect(reseeds).toBeLessThanOrEqual(bound);
    // A 10 s trace at ~50% spike probability must exercise the cooldown at
    // all — otherwise the test proves nothing about flap.
    expect(reseeds).toBeGreaterThan(0);
    expect(gov.reseeds).toBe(reseeds);
  });

  it('G3b: suppressed Reseed degrades to Snapshot (the documented fallback)', () => {
    const gov = new FreshnessGovernor();
    const first = gov.step(64, 1000);
    expect(first.kind).toBe(GovernorActionKind.Reseed);
    // 50 ms later — inside the 250 ms cooldown: degraded, not flapped.
    const second = gov.step(64, 1050);
    expect(second.kind).toBe(GovernorActionKind.Snapshot);
    // After the cooldown: Reseed again.
    const third = gov.step(64, 1300);
    expect(third.kind).toBe(GovernorActionKind.Reseed);
    expect(gov.reseeds).toBe(2);
  });

  it('G4: zero allocation — 1M steps leave the heap at the noise floor', () => {
    const gov = new FreshnessGovernor();
    // Warmup (JIT + lazy property maps).
    for (let i = 0; i < 100_000; i++) gov.step(i % 32, i);
    const gc = typeof globalThis.gc === 'function' ? globalThis.gc : undefined;
    gc?.();
    const before = process.memoryUsage().heapUsed;
    for (let i = 0; i < 1_000_000; i++) gov.step(i % 32, i);
    gc?.();
    const delta = process.memoryUsage().heapUsed - before;
    // 64 KiB budget — the same tolerance B5 uses; without --expose-gc the
    // delta is advisory but a real leak of 1M objects would be ~50 MB+ and
    // still blow past it, so the assertion keeps teeth either way.
    expect(Math.abs(delta)).toBeLessThanOrEqual(65536);
    // The identity-stable record: every step returns the SAME object.
    const a1 = gov.step(0, 0);
    const a2 = gov.step(64, 1000);
    expect(a1).toBe(a2);
  });

  it('Law 4: Skip(n) intermediates are counted as DECIDED drops', () => {
    const gov = new FreshnessGovernor();
    gov.step(3, 0); // Skip(2)
    gov.step(4, 1); // Skip(3)
    gov.step(1, 2); // FastPath — no decided drops
    gov.step(9, 3); // Snapshot — none
    expect(gov.decidedDrops).toBe(2 + 3);
    expect(gov.steps).toBe(4);
  });

  it('custom thresholds and reset()', () => {
    const gov = new FreshnessGovernor({
      fastPathBehind: 0,
      skipBehind: 8,
      snapshotBehind: 32,
      reseedCooldownMs: 10,
    });
    expect(gov.step(0, 0).kind).toBe(GovernorActionKind.FastPath);
    expect(gov.step(1, 0).kind).toBe(GovernorActionKind.Skip);
    expect(gov.step(1, 0).skipN).toBe(1);
    expect(gov.step(33, 0).kind).toBe(GovernorActionKind.Reseed);
    gov.reset();
    expect(gov.steps).toBe(0);
    expect(gov.decidedDrops).toBe(0);
    expect(gov.reseeds).toBe(0);
    // After reset the cooldown clock starts fresh.
    expect(gov.step(33, 0).kind).toBe(GovernorActionKind.Reseed);
  });
});
