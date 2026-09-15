// runner.ts — Execution engines for Modes A, B, C, and D
//
// Mode semantics per WO-P5 release rulings:
// A = reactive naive (reallocates every frame)
// B = pooled best-practice (pooled buffers, main thread)
// C = weft (this Triad Protocol kernel, zero-copy atomic exchange)
// D = hand-rolled triple-buffer with envelope + I6

import { Weft, PubResult } from '@weft/core';
import { generateWorkloadFrame } from '../workloads/generators';

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
    const w = Atomics.load(this.weft.ctrl, Weft.SLOT_W_WORK);
    const off = this.weft.bufOffset(w) + 16;
    const f32View = new Float32Array(this.weft.sab, off, this.floatCount);
    generateWorkloadFrame(this.wid, frameIdx, f32View);
    const res = this.weft.publish(frameIdx, this.floatCount * 4);
    if (res !== PubResult.Ok) {
      // dropped
    }
  }

  consumeFrame(target: Float32Array): boolean {
    this.weft.claim();
    const rPtr = this.weft.rReadSlice(16, this.floatCount * 4);
    if (rPtr.length === 0) return false;
    const f32View = new Float32Array(rPtr.buffer, rPtr.byteOffset, this.floatCount);
    target.set(f32View);
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
