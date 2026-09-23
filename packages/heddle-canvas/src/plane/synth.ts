// synth.ts — the synthetic Hot-Plane PRODUCER: the executable spec of the
// writer half of the WHP1 contract, and the 100,000 samples/sec load
// generator for the frame-budget bench and the browser rig.
//
// WHY THIS EXISTS: Engineer 1's Hot-Plane engine is the production writer,
// but every gate in this package must run without it. SynthProducer speaks
// byte-identical WHP1 (same header words, same dirty-mask protocol, same
// write_pos/seq commit) — when the real engine lands it replaces this
// class and NOTHING below the plane view changes. That substitution seam
// is the whole point: the contract, not the implementation, is the
// dependency.
//
// WRITER PROTOCOL (each publication, in order — the order IS the protocol,
// src/plane/dirty_mask.ts):
//   1. seq bump (odd)          — fence: readers see "write in flight"
//   2. payload stores          — plain f32 stores into the lane data
//   3. write_pos advance       — Atomics.store of the total-published count
//   4. seq bump (even)         — fence: window stable again
//   5. markDirty64             — ONE atomic 64-bit OR on the lane's dirty
//                                 word (the contract road; markDirty32 is
//                                 the proven-equivalent split road)
//   6. plane epoch + seq bump  — liveness heartbeat
// The render loop never blocks on this: it reads write_pos/seq under the
// tear fence and re-reads next frame when the fence moved.

import {
  HP_CANDLE_W_OHLC,
  HP_CANDLE_W_VOLUME,
  HP_KIND,
  HP_LADDER_W_PRICE,
  HP_LADDER_W_SIDE,
  HP_LADDER_W_SIZE,
  HP_PC_W_COLOR,
  HP_PC_W_POSSIZE,
  HP_PC_W_QUAT,
  WHP1_OFF_EPOCH,
  WHP1_OFF_PRODUCER_SEQ,
} from './whp1.ts';
import type { LaneView, PlaneView } from './hot_plane.ts';
import { markDirty64, producerCommit } from './dirty_mask.ts';

/** Deterministic RNG (mulberry32) — same sequence in node, Chromium and
 *  the oracle hasher, so pixel/word hashes are comparable across engines. */
export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Bit-preserving f32→u32 store tool (module-level, carved once — producers
// may allocate, but there is no reason to).
const _bc = new Float32Array(1);
const _bcv = new Uint32Array(_bc.buffer);
function storeF32(u32: Uint32Array, index: number, value: number): void {
  _bc[0] = value;
  u32[index] = _bcv[0];
}

/** The deterministic waveform sample: sine pair + quantized noise.
 *  Every term is f32-exact after Math.fround, in every engine. */
export function synthSample(i: number, t: number): number {
  const sweep = Math.sin(i * 0.00021 + t * 0.7);
  const carrier = Math.sin(i * 0.031 + t * 2.1);
  const noise = Math.floor(((i * 2654435761) >>> 24) / 16) / 64 - 0.25;
  return Math.max(-1.5, Math.min(1.5, 0.55 * sweep * carrier + 0.2 * noise));
}

export class SynthProducer {
  readonly plane: PlaneView;
  private cursor = 0; // total waveform samples ever written
  private rowCursor = 0; // total row publications (ladder/candle/cloud)

  constructor(plane: PlaneView) {
    this.plane = plane;
  }

  private laneOf(kind: number): LaneView {
    for (let i = 0; i < this.plane.laneCount; i++) {
      if (this.plane.lanes[i].kind === kind) return this.plane.lanes[i];
    }
    throw new RangeError(`no lane of kind ${kind} in this plane`);
  }

  /**
   * Publish `n` fresh waveform samples into the ring (wrapping at
   * capacity), then run the full writer protocol on that lane. This is
   * the per-frame 100k/sec feed: 100000 / 240 = 416.67 → the bench
   * alternates 417/416 samples per tick so the long-run rate is exact.
   */
  pumpWaveform(n: number, t: number): void {
    const lane = this.laneOf(HP_KIND.WAVEFORM_F32);
    const cap = lane.capacity;
    const f32 = lane.f32;
    const i32 = this.plane.i32;
    const seqOdd = Atomics.add(i32, lane.seqI32, 1); // 1. fence open
    void seqOdd;
    let w = this.cursor % cap; // 2. payload (wrapping stores)
    for (let k = 0; k < n; k++) {
      f32[w] = synthSample(this.cursor + k, t);
      w++;
      if (w === cap) w = 0;
    }
    this.cursor += n;
    Atomics.store(i32, lane.writePosI32, this.cursor); // 3. head advance
    Atomics.add(i32, lane.seqI32, 1); // 4. fence close
    // 5. dirty bits for EXACTLY the slots written (wrapping splits in two
    //    ranges; the consumer's bit-span upload is the conservative
    //    superset — documented in RFC-0022 §4.3).
    const w0 = (this.cursor - n) % cap;
    const wrap = w0 + n > cap;
    if (!wrap) {
      markDirty64(this.plane.big64, lane, w0, n);
    } else {
      markDirty64(this.plane.big64, lane, w0, cap - w0);
      markDirty64(this.plane.big64, lane, 0, w0 + n - cap);
    }
    Atomics.add(i32, WHP1_OFF_EPOCH / 4, 1); // 6. heartbeat
    Atomics.add(i32, WHP1_OFF_PRODUCER_SEQ / 4, 1);
  }

  /**
   * Publish a full ladder snapshot (rowCount rows). Prices walk a mean-
   * reverting random walk; sizes sweep; sides alternate. Deterministic
   * under the seed — the rig hashes the same rows in every tier.
   */
  pumpLadder(rowCount: number, t: number): void {
    const lane = this.laneOf(HP_KIND.DEPTH_LADDER_F32);
    const rows = Math.min(rowCount, lane.capacity);
    const u = lane.u32;
    const words = lane.strideBytes >> 2;
    const i32 = this.plane.i32;
    Atomics.add(i32, lane.seqI32, 1);
    for (let r = 0; r < rows; r++) {
      const price = 0.25 + 0.5 * ((r + Math.floor(t * 7)) % 64) / 64;
      const size = ((r * 37 + Math.floor(t * 240)) % 96) / 96;
      storeF32(u, r * words + HP_LADDER_W_PRICE, price);
      storeF32(u, r * words + HP_LADDER_W_SIZE, size);
      u[r * words + HP_LADDER_W_SIDE] = r & 1; // alternating bid/ask
    }
    Atomics.store(i32, lane.writePosI32, rows);
    Atomics.add(i32, lane.seqI32, 1);
    markDirty64(this.plane.big64, lane, 0, rows);
    Atomics.add(i32, WHP1_OFF_EPOCH / 4, 1);
    this.rowCursor++;
  }

  /**
   * Publish `rowCount` candles: a deterministic OHLC walk (each candle
   * opens at the previous close, wicks bounded by the walk noise).
   */
  pumpCandles(rowCount: number, t: number): void {
    const lane = this.laneOf(HP_KIND.CANDLE_OHLC_F32);
    const rows = Math.min(rowCount, lane.capacity);
    const f32 = lane.f32;
    const words = lane.strideBytes >> 2;
    const i32 = this.plane.i32;
    Atomics.add(i32, lane.seqI32, 1);
    let close = 0.5;
    for (let r = 0; r < rows; r++) {
      const drift = Math.sin((r + t * 12) * 0.31) * 0.04;
      const open = close;
      close = Math.min(0.98, Math.max(0.02, open + drift));
      const wick = 0.015 + 0.02 * ((r * 17 + Math.floor(t * 60)) % 32) / 32;
      const high = Math.min(1, Math.max(open, close) + wick);
      const low = Math.max(0, Math.min(open, close) - wick);
      const vol = 0.2 + 0.8 * ((r * 53 + Math.floor(t * 90)) % 128) / 128;
      const base = r * words + HP_CANDLE_W_OHLC;
      f32[base] = open;
      f32[base + 1] = high;
      f32[base + 2] = low;
      f32[base + 3] = close;
      f32[r * words + HP_CANDLE_W_VOLUME] = vol;
    }
    Atomics.store(i32, lane.writePosI32, rows);
    Atomics.add(i32, lane.seqI32, 1);
    markDirty64(this.plane.big64, lane, 0, rows);
    Atomics.add(i32, WHP1_OFF_EPOCH / 4, 1);
    this.rowCursor++;
  }

  /**
   * Publish a point-cloud frame: points on a rotating torus with per-point
   * quaternions (IMU-attitude stand-in) and depth-shaded colors.
   */
  pumpPointcloud(pointCount: number, t: number): void {
    const lane = this.laneOf(HP_KIND.POINTCLOUD_QUAT_F32);
    const pts = Math.min(pointCount, lane.capacity);
    const f32 = lane.f32;
    const words = lane.strideBytes >> 2;
    const i32 = this.plane.i32;
    Atomics.add(i32, lane.seqI32, 1);
    for (let p = 0; p < pts; p++) {
      const a = (p / pts) * Math.PI * 2 + t * 0.9;
      const b = (p * 0.027) % (Math.PI * 2);
      const R = 0.62, r = 0.26;
      const cx = (R + r * Math.cos(b)) * Math.cos(a);
      const cy = (R + r * Math.cos(b)) * Math.sin(a);
      const cz = r * Math.sin(b);
      const rot = t * 1.3 + a * 0.5;
      const qx = Math.sin(rot / 2) * 0.1;
      const qy = Math.sin(rot / 2) * 0.2;
      const qz = Math.sin(rot / 2) * 0.3;
      const qw = Math.cos(rot / 2);
      const n = 1 / Math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
      const shade = 0.35 + 0.65 * (cz / r + 1) * 0.5;
      const base = p * words;
      f32[base + HP_PC_W_POSSIZE + 0] = cx;
      f32[base + HP_PC_W_POSSIZE + 1] = cy;
      f32[base + HP_PC_W_POSSIZE + 2] = cz;
      f32[base + HP_PC_W_POSSIZE + 3] = 0.012; // point sprite size
      f32[base + HP_PC_W_QUAT + 0] = qx * n;
      f32[base + HP_PC_W_QUAT + 1] = qy * n;
      f32[base + HP_PC_W_QUAT + 2] = qz * n;
      f32[base + HP_PC_W_QUAT + 3] = qw * n;
      f32[base + HP_PC_W_COLOR + 0] = shade * 0.35;
      f32[base + HP_PC_W_COLOR + 1] = shade * 0.75;
      f32[base + HP_PC_W_COLOR + 2] = shade;
      f32[base + HP_PC_W_COLOR + 3] = 1;
    }
    Atomics.store(i32, lane.writePosI32, pts);
    Atomics.add(i32, lane.seqI32, 1);
    markDirty64(this.plane.big64, lane, 0, pts);
    Atomics.add(i32, WHP1_OFF_EPOCH / 4, 1);
    this.rowCursor++;
  }

  /** Total waveform samples published over this producer's life. */
  get published(): number {
    return this.cursor + this.rowCursor;
  }

  /** The commit seam producerCommit exists for (exported for tests). */
  commitLane(lane: LaneView, writePos: number): void {
    producerCommit(this.plane.i32, lane, writePos);
  }
}
