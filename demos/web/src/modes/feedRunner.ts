// feedRunner.ts — W6 mode runners: the A/B/C/D methodology applied to feed
// ingestion.
//
// WHY EXISTS: WO-P5's mode semantics (runner.ts) define the four buffer
// strategies the whole demo matrix measures: A = reactive naive
// (reallocates), B = pooled best-practice, C = weft kernel, D = hand-rolled
// triple buffer. W1–W5 are stateless per-frame generators, so those runners
// only differ in BUFFER management. W6 adds the feed dimension — K delta
// messages arrive per display tick — which is exactly the regime where the
// four strategies diverge the most: Mode A pays one event object per
// MESSAGE plus a fresh snapshot per tick; B pools; C publishes through the
// kernel's public write cursor (wBeginFloat32, the Series-1 API); D swaps
// hand-rolled buffers.
//
// All four runners drive the SAME deterministic stream (SyntheticL2Feed,
// pinned seed) through arithmetic mirrors of the same book semantics, so
// the matrix's cross-mode equivalence test can assert bit-identical frames.
//
// Law 2 (zero steady-state allocation) holds by construction on the
// INGESTION path (produceFrame) of B/C/D: the feed writes into a runner-
// owned batch buffer, the engine is SoA typed arrays, and Mode C's export
// lands directly in the kernel's cached cursor view. The consume path pays
// one small view object per claim (rReadSlice's return — the kernel's
// public read API allocates its slice view by design); the Float32 wrapper
// is cached per slot (<= 3 total, keyed on the public byteOffset), which
// is one fewer per-frame allocation than the W1–W5 ModeCRunner pays. The
// measured evidence is the GC harness (scripts/feed_gc_bench.ts); the
// matrix asserts the cursor-identity discipline (<= 3 kernel views).

import { Weft, PubResult } from '@weft/core';
import type { ModeRunner } from './runner';
import {
  SyntheticL2Feed,
  L2BookEngine,
  ObjectL2Book,
  W6_FLOAT_COUNT,
  W6_FEED_TO_DISPLAY,
  W6_MSG_FIELDS,
} from '../workloads/l2feed.ts';

/// Bytes of one W6 frame payload (kernel payload_len field).
const W6_FRAME_BYTES = W6_FLOAT_COUNT * 4;

/// Shared produce step for the SoA modes (B/C/D): pull the next tick's
/// batch from the feed and fold it into the engine. Zero allocation.
function foldNextTick(feed: SyntheticL2Feed, engine: L2BookEngine, batch: Float64Array): void {
  feed.nextTick(batch);
  engine.setGrid(feed.midC, feed.bidOff, feed.askOff);
  engine.beginTick();
  engine.applyBatch(batch, W6_FEED_TO_DISPLAY);
}

// --- W6 Mode A: reactive naive — per-message event objects, object-graph
// book, fresh snapshot array per tick. The allocation baseline. ---
export class W6ModeARunner implements ModeRunner {
  private feed = new SyntheticL2Feed();
  private book = new ObjectL2Book();
  private batch = new Float64Array(W6_FEED_TO_DISPLAY * W6_MSG_FIELDS);
  private snapshot: Float32Array | null = null;

  produceFrame(frameIdx: number): void {
    this.feed.nextTick(this.batch);
    this.book.setGrid(this.feed.midC, this.feed.bidOff, this.feed.askOff);
    this.book.beginTick();
    // Materialize one event object per feed message — the naive reactive
    // model (an allocation the other three modes do not pay).
    for (let m = 0; m < W6_FEED_TO_DISPLAY; m++) {
      const o = m * W6_MSG_FIELDS;
      this.book.applyMsg({
        op: this.batch[o],
        side: this.batch[o + 1],
        level: this.batch[o + 2],
        qty: this.batch[o + 3],
        n: this.batch[o + 4],
        pxC: this.batch[o + 5],
      });
    }
    // Fresh snapshot array every tick (Mode A tradition).
    this.snapshot = this.book.exportFrame(frameIdx);
  }

  consumeFrame(target: Float32Array): boolean {
    if (!this.snapshot) return false;
    target.set(this.snapshot);
    return true;
  }

  getDropCount(): number {
    return 0;
  }

  dispose(): void {
    this.snapshot = null;
  }
}

// --- W6 Mode B: pooled best-practice — scalar batch, SoA engine, reused
// double-buffered snapshot. ---
export class W6ModeBRunner implements ModeRunner {
  private feed = new SyntheticL2Feed();
  private engine = new L2BookEngine();
  private batch = new Float64Array(W6_FEED_TO_DISPLAY * W6_MSG_FIELDS);
  private pool: [Float32Array, Float32Array];
  private activeIdx = 0;

  constructor() {
    this.pool = [new Float32Array(W6_FLOAT_COUNT), new Float32Array(W6_FLOAT_COUNT)];
  }

  produceFrame(frameIdx: number): void {
    foldNextTick(this.feed, this.engine, this.batch);
    const next = (this.activeIdx + 1) % 2;
    this.engine.exportFrame(this.pool[next], frameIdx);
    this.activeIdx = next;
  }

  consumeFrame(target: Float32Array): boolean {
    target.set(this.pool[this.activeIdx]);
    return true;
  }

  getDropCount(): number {
    return 0;
  }

  dispose(): void {}
}

// --- W6 Mode C: weft — the engine exports directly into the kernel's
// public typed write cursor; publish() carries the folded frame across.
// `weft` is public readonly for the matrix's cursor-identity test (the
// same observability the kernel itself exposes via public fields). ---
export class W6ModeCRunner implements ModeRunner {
  readonly weft: Weft;
  private feed = new SyntheticL2Feed();
  private engine = new L2BookEngine();
  private batch = new Float64Array(W6_FEED_TO_DISPLAY * W6_MSG_FIELDS);
  /// Cached Float32 read view, keyed on the public rReadSlice byteOffset
  /// (<= 3 distinct slots — see the file header's Law 2 note).
  private rView: Float32Array | null = null;

  constructor() {
    this.weft = new Weft(W6_FRAME_BYTES);
  }

  produceFrame(frameIdx: number): void {
    foldNextTick(this.feed, this.engine, this.batch);
    // Public cursor API (Series-1 parity): export straight into the
    // kernel's cached Float32 view — no intermediate buffer, no copy.
    this.engine.exportFrame(this.weft.wBeginFloat32(), frameIdx);
    const res = this.weft.publish(frameIdx, W6_FRAME_BYTES);
    if (res !== PubResult.Ok) {
      // dropped — counted in t_drop, surfaced via getDropCount()
    }
  }

  consumeFrame(target: Float32Array): boolean {
    this.weft.claim();
    const rPtr = this.weft.rReadSlice(16, W6_FRAME_BYTES);
    if (rPtr.length === 0) return false;
    if (this.rView === null || this.rView.byteOffset !== rPtr.byteOffset) {
      this.rView = new Float32Array(rPtr.buffer, rPtr.byteOffset, W6_FLOAT_COUNT);
    }
    target.set(this.rView);
    return true;
  }

  getDropCount(): number {
    return Number(this.weft.tDrop());
  }

  dispose(): void {
    // GC cleans up SAB
  }
}

// --- W6 Mode D: hand-rolled triple buffer — same SoA engine, swap-based
// transport with the same slot discipline as runner.ts's ModeDRunner. ---
export class W6ModeDRunner implements ModeRunner {
  private feed = new SyntheticL2Feed();
  private engine = new L2BookEngine();
  private batch = new Float64Array(W6_FEED_TO_DISPLAY * W6_MSG_FIELDS);
  private bufs: [Float32Array, Float32Array, Float32Array];
  private latest = 0;
  private wWork = 1;
  private rWork = 2;

  constructor() {
    this.bufs = [
      new Float32Array(W6_FLOAT_COUNT),
      new Float32Array(W6_FLOAT_COUNT),
      new Float32Array(W6_FLOAT_COUNT),
    ];
  }

  produceFrame(frameIdx: number): void {
    foldNextTick(this.feed, this.engine, this.batch);
    this.engine.exportFrame(this.bufs[this.wWork], frameIdx);
    const old = this.latest;
    this.latest = this.wWork;
    this.wWork = old;
  }

  consumeFrame(target: Float32Array): boolean {
    const mine = this.latest;
    this.latest = this.rWork;
    this.rWork = mine;
    target.set(this.bufs[this.rWork]);
    return true;
  }

  getDropCount(): number {
    return 0;
  }

  dispose(): void {}
}

export function createW6ModeRunner(mode: 'A' | 'B' | 'C' | 'D'): ModeRunner {
  switch (mode) {
    case 'A':
      return new W6ModeARunner();
    case 'B':
      return new W6ModeBRunner();
    case 'C':
      return new W6ModeCRunner();
    case 'D':
      return new W6ModeDRunner();
  }
}
