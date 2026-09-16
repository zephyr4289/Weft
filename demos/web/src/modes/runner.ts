// runner.ts — Execution engines for Modes A, B, C, and D
//
// Mode semantics per WO-P5 release rulings:
// A = reactive naive (reallocates every frame)
// B = pooled best-practice (pooled buffers, main thread)
// C = weft (this Triad Protocol kernel, zero-copy atomic exchange)
// D = hand-rolled triple-buffer with envelope + I6

import { Weft, PubResult } from '@weft/core';
import { generateWorkloadFrame } from '../workloads/generators';
import { createW6ModeRunner } from './feedRunner';

export type ModeType = 'A' | 'B' | 'C' | 'D';

export interface FrameStats {
  fps: number;
  p50: number;
  p99: number;
  tDrop: number;
  heapDeltaMb: number;
  mode: ModeType;
  workloadId: string;
}

export interface ModeRunner {
  produceFrame(frameIdx: number): void;
  consumeFrame(target: Float32Array): boolean;
  dispose(): void;
  getDropCount(): number;
}

// --- Mode A: Reactive Naive ---
export class ModeARunner implements ModeRunner {
  private currentFrame: Float32Array | null = null;
  private dropCount = 0;
  private readonly floatCount: number;
  private readonly wid: string;

  constructor(wid: string, floatCount: number) {
    this.wid = wid;
    this.floatCount = floatCount;
  }

  produceFrame(frameIdx: number): void {
    // Mode A allocates a fresh Float32Array on every single frame
    const fresh = new Float32Array(this.floatCount);
    generateWorkloadFrame(this.wid, frameIdx, fresh);
    this.currentFrame = fresh;
  }

  consumeFrame(target: Float32Array): boolean {
    if (!this.currentFrame) return false;
    target.set(this.currentFrame);
    return true;
  }

  getDropCount(): number {
    return this.dropCount;
  }

  dispose(): void {
    this.currentFrame = null;
  }
}

// --- Mode B: Pooled Best-Practice ---
export class ModeBRunner implements ModeRunner {
  private pool: [Float32Array, Float32Array];
  private activeIdx = 0;
  private dropCount = 0;
  private readonly wid: string;

  constructor(wid: string, floatCount: number) {
    this.wid = wid;
    this.pool = [new Float32Array(floatCount), new Float32Array(floatCount)];
  }

  produceFrame(frameIdx: number): void {
    const nextIdx = (this.activeIdx + 1) % 2;
    generateWorkloadFrame(this.wid, frameIdx, this.pool[nextIdx]);
    this.activeIdx = nextIdx;
  }

  consumeFrame(target: Float32Array): boolean {
    target.set(this.pool[this.activeIdx]);
    return true;
  }

  getDropCount(): number {
    return this.dropCount;
  }

  dispose(): void {}
}

// --- Mode C: Weft Protocol Kernel ---
export class ModeCRunner implements ModeRunner {
  private weft: Weft;
  private readonly wid: string;
  private readonly floatCount: number;

  constructor(wid: string, floatCount: number) {
    this.wid = wid;
    this.floatCount = floatCount;
    this.weft = new Weft(floatCount * 4);
  }

  produceFrame(frameIdx: number): void {
    // 2026-09: writes go through the PUBLIC typed write cursor
    // (Weft#wBeginFloat32 — port parity with weft_w_begin), replacing the
    // previous reach into kernel internals (weft.ctrl / Weft.SLOT_W_WORK /
    // weft.bufOffset + manual Float32Array construction). The encapsulation
    // breach was the symptom of the missing writer API; the API now exists.
    const f32View = this.weft.wBeginFloat32();
    generateWorkloadFrame(this.wid, frameIdx, f32View);
    const res = this.weft.publish(frameIdx, this.floatCount * 4);
    if (res !== PubResult.Ok) {
      // dropped
    }
  }

  consumeFrame(target: Float32Array): boolean {
    this.weft.claim();
    // Hot path (Law 2): the kernel's CACHED live Float32 view of the
    // reader-held buffer — zero allocation per frame. The old path allocated
    // TWO view objects per consume (rReadSlice's fresh Uint8Array + a fresh
    // Float32Array wrapper) — that was the feed-gc-bench P+C remainder.
    // rLiveFloat32() is the documented draw-phase API (the TS analog of C's
    // weft_r_live_ptr); its element count is exactly floatCount because
    // payloadMax = floatCount * 4.
    target.set(this.weft.rLiveFloat32());
    return true;
  }

  getDropCount(): number {
    return Number(this.weft.tDrop());
  }

  dispose(): void {
    // GC cleans up SAB
  }
}

// --- Mode D: Hand-Rolled Triple Buffer ---
export class ModeDRunner implements ModeRunner {
  private bufs: [Float32Array, Float32Array, Float32Array];
  private latest = 0;
  private wWork = 1;
  private rWork = 2;
  private dropCount = 0;
  private readonly wid: string;

  constructor(wid: string, floatCount: number) {
    this.wid = wid;
    this.bufs = [
      new Float32Array(floatCount),
      new Float32Array(floatCount),
      new Float32Array(floatCount),
    ];
  }

  produceFrame(frameIdx: number): void {
    generateWorkloadFrame(this.wid, frameIdx, this.bufs[this.wWork]);
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
    return this.dropCount;
  }

  dispose(): void {}
}

export function createModeRunner(mode: ModeType, wid: string, floatCount: number): ModeRunner {
  // W6 is the stateful feed workload — its runners own a SyntheticL2Feed +
  // book engine and fold K messages per tick (modes/feedRunner.ts). The
  // floatCount argument is redundant for W6 (the layout is the shared
  // W6_FLOAT_COUNT contract) but stays in the uniform signature.
  if (wid === 'W6') {
    return createW6ModeRunner(mode);
  }
  switch (mode) {
    case 'A':
      return new ModeARunner(wid, floatCount);
    case 'B':
      return new ModeBRunner(wid, floatCount);
    case 'C':
      return new ModeCRunner(wid, floatCount);
    case 'D':
      return new ModeDRunner(wid, floatCount);
  }
}
