import { describe, it, expect } from 'vitest';
import { WORKLOAD_INFO } from '../src/workloads/generators';
import { createModeRunner, ModeType } from '../src/modes/runner';
import { W6ModeCRunner } from '../src/modes/feedRunner';
import {
  W6_FLOAT_COUNT,
  W6_LEVELS,
  W6_HEADER_FLOATS,
  W6_LADDER_FIELDS,
  w6Checksum,
} from '../src/workloads/l2feed';

describe('Showcase Demos 6×4 Matrix & Hot-Switching', () => {
  const workloads = ['W1', 'W2', 'W3', 'W4', 'W5', 'W6'];
  const modes: ModeType[] = ['A', 'B', 'C', 'D'];

  // Test 1: 6×4 Matrix Execution
  for (const wid of workloads) {
    for (const mode of modes) {
      it(`Workload ${wid} · Mode ${mode} steady-state execution (1000 frames)`, () => {
        const info = WORKLOAD_INFO[wid];
        const runner = createModeRunner(mode, wid, info.floatCount);
        const target = new Float32Array(info.floatCount);

        for (let frame = 1; frame <= 1000; frame++) {
          runner.produceFrame(frame);
          const ok = runner.consumeFrame(target);
          expect(ok).toBe(true);
        }

        if (mode === 'C' || mode === 'D') {
          expect(runner.getDropCount()).toBe(0);
        }

        runner.dispose();
      });
    }
  }

  // Test 2: Mode Hot-Switching (100 switches without leaks)
  it('Hot-switches modes across 100 transitions without error', () => {
    let currentMode: ModeType = 'A';
    let currentWid = 'W1';
    let runner = createModeRunner(currentMode, currentWid, WORKLOAD_INFO[currentWid].floatCount);
    const target = new Float32Array(16384);

    for (let cycle = 1; cycle <= 100; cycle++) {
      currentMode = modes[cycle % 4];
      currentWid = workloads[cycle % workloads.length];
      runner.dispose();
      runner = createModeRunner(currentMode, currentWid, WORKLOAD_INFO[currentWid].floatCount);

      runner.produceFrame(cycle);
      const ok = runner.consumeFrame(target.subarray(0, WORKLOAD_INFO[currentWid].floatCount));
      expect(ok).toBe(true);
    }
    runner.dispose();
  });
});

// ---------------------------------------------------------------------------
// W6 feed-fold conformance — the properties that make the feed workload a
// measurement subject rather than a cosmetic animation.
// ---------------------------------------------------------------------------

describe('W6 feed workload conformance', () => {
  /// Structural book validation on a W6 frame: sizes/counts non-negative,
  /// grid prices monotone away from the mid, top-of-book uncrossed, tape
  /// sides in {-1, 0, +1}, and the payload checksum self-consistent.
  function validateW6Frame(f: Float32Array): void {
    // Checksum first — covers every float except [7] itself.
    expect(w6Checksum(f)).toBe(f[7]);
    const lad = W6_HEADER_FLOATS;
    for (let i = 0; i < W6_LEVELS; i++) {
      const o = lad + i * W6_LADDER_FIELDS;
      expect(f[o + 1]).toBeGreaterThanOrEqual(0); // bidSz
      expect(f[o + 2]).toBeGreaterThanOrEqual(0); // bidN
      expect(f[o + 4]).toBeGreaterThanOrEqual(0); // askSz
      expect(f[o + 5]).toBeGreaterThanOrEqual(0); // askN
      // Grid prices strictly monotone away from the mid.
      if (i > 0) {
        expect(f[o]).toBeLessThan(f[lad + (i - 1) * W6_LADDER_FIELDS]); // bid descending
        expect(f[o + 3]).toBeGreaterThan(f[lad + (i - 1) * W6_LADDER_FIELDS + 3]); // ask ascending
      }
    }
    // Uncrossed top of book.
    expect(f[lad]).toBeLessThan(f[lad + 3]);
    // Tape sides.
    const tape = lad + W6_LEVELS * W6_LADDER_FIELDS;
    for (let j = 0; j < 64; j++) {
      const s = f[tape + j * 3 + 2];
      expect(s === 0 || s === -1 || s === 1).toBe(true);
    }
    // Fold accounting: every tick folds exactly the pinned ratio.
    expect(f[1]).toBe(50);
  }

  it('Mode C maintains structural book invariants + checksum over 1000 ticks', () => {
    const runner = new W6ModeCRunner();
    const target = new Float32Array(W6_FLOAT_COUNT);
    for (let t = 1; t <= 1000; t++) {
      runner.produceFrame(t);
      // Validate every 10th claim (checksum walks the whole payload —
      // enough coverage without dominating runtime).
      if (t % 10 === 0) {
        const ok = runner.consumeFrame(target);
        expect(ok).toBe(true);
        expect(target[0]).toBe(t); // frame seq == tick
        validateW6Frame(target);
      }
    }
    expect(runner.getDropCount()).toBe(0);
    runner.dispose();
  });

  it('Mode C write path uses only the kernel\u2019s cached cursor views (Law 2 discipline)', () => {
    // The cursor-identity assertion (fanout test tradition): with claims
    // interleaved (the triad rotates all three slots through w_work), the
    // writer must use EXACTLY the 3 cached kernel views across 100 ticks —
    // the engine exports into wBeginFloat32() without wrapping or copying
    // it. (The measured allocation evidence is the GC harness:
    // scripts/feed_gc_bench.ts + evidence/feed-gc-bench.log.)
    const runner = new W6ModeCRunner();
    const seen = new Set<Float32Array>();
    const target = new Float32Array(W6_FLOAT_COUNT);
    for (let t = 1; t <= 100; t++) {
      runner.produceFrame(t);
      runner.consumeFrame(target);
      seen.add(runner.weft.wBeginFloat32());
    }
    expect(seen.size).toBe(3);
    runner.dispose();
  });

  it('all four modes produce BIT-IDENTICAL frames from the same seed (aggregation correctness)', () => {
    // The strongest correctness property of the feed workload: A/B/C/D
    // ingest the SAME deterministic stream through four buffer strategies
    // and arithmetic mirrors of the same book semantics. Byte equality at
    // a mid-run checkpoint AND at the end proves the fold is order-stable
    // and the Mode A object mirror is exact.
    const TICKS = 500;
    const CHECKPOINT = 250;
    const captures: Record<string, Float32Array> = {};
    const midCaptures: Record<string, Float32Array> = {};
    for (const mode of ['A', 'B', 'C', 'D'] as ModeType[]) {
      const runner = createModeRunner(mode, 'W6', W6_FLOAT_COUNT);
      const target = new Float32Array(W6_FLOAT_COUNT);
      for (let t = 1; t <= TICKS; t++) {
        runner.produceFrame(t);
        runner.consumeFrame(target);
        if (t === CHECKPOINT) midCaptures[mode] = target.slice();
      }
      captures[mode] = target.slice();
      runner.dispose();
    }
    for (const phase of [midCaptures, captures]) {
      for (const a of ['A', 'B', 'C', 'D'] as ModeType[]) {
        for (const b of ['A', 'B', 'C', 'D'] as ModeType[]) {
          if (a >= b) continue;
          for (let i = 0; i < W6_FLOAT_COUNT; i++) {
            expect(phase[a][i]).toBe(phase[b][i]);
          }
        }
      }
    }
    // Sanity: the run actually traded (tape non-empty, lastTrade set).
    expect(captures['C'][6]).toBeGreaterThan(0);
  });

  it('deterministic stream: same seed reproduces the same book bit-for-bit', () => {
    // Two independent Mode C runs must agree exactly — the W6 analog of
    // the W-suite's pinned-SEED discipline.
    const run = (): Float32Array => {
      const runner = new W6ModeCRunner();
      const target = new Float32Array(W6_FLOAT_COUNT);
      for (let t = 1; t <= 300; t++) {
        runner.produceFrame(t);
        runner.consumeFrame(target);
      }
      runner.dispose();
      return target;
    };
    const r1 = run();
    const r2 = run();
    for (let i = 0; i < W6_FLOAT_COUNT; i++) {
      expect(r1[i]).toBe(r2[i]);
    }
  });
});
