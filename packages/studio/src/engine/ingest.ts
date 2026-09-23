/**
 * Weft Studio — zero-alloc telemetry ingestion pipeline.
 * Decodes simulated market/robotics frames into the RingMap and preallocated
 * stat blocks. The ingest call path performs ZERO heap allocation: every
 * buffer, view and stat cell is preallocated at construction. Stage-2 of the
 * managed suite proves ≤ 64 KiB heap growth over 1,000,000 messages.
 */

import { RingMap } from './ring';

export const MSG_MARKET_TICK = 1;
export const MSG_IMU_SAMPLE = 2;
export const MSG_FRAME_EVENT = 3;
export const MSG_MUTATION = 4;
export const MSG_HEARTBEAT = 5;

export const TELEMETRY_COLS = 8;

/**
 * Preallocated telemetry accumulator: one f64 row per metric lane, ringed at
 * TELEMETRY_COLS. Lanes: 0=msg/s 1=ingest_ns 2=p99_frame_ns 3=occupancy
 * 4=dropped 5=torn 6=zero_copy_index 7=heap_hint
 */
export class TelemetryBlock {
  readonly data = new Float64Array(TELEMETRY_COLS * TELEMETRY_COLS);
  readonly col = new Int32Array(TELEMETRY_COLS);
  lastIngestNs = 0;
  lastMsgCount = 0;

  push(lane: number, v: number): void {
    const c = this.col[lane];
    this.data[lane * TELEMETRY_COLS + c] = v;
    this.col[lane] = (c + 1) % TELEMETRY_COLS;
  }

  latest(lane: number): number {
    const c = (this.col[lane] + TELEMETRY_COLS - 1) % TELEMETRY_COLS;
    return this.data[lane * TELEMETRY_COLS + c];
  }

  minMax(lane: number, out: Float64Array): void {
    let lo = Infinity, hi = -Infinity;
    for (let i = 0; i < TELEMETRY_COLS; i++) {
      const v = this.data[lane * TELEMETRY_COLS + i];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    out[0] = lo; out[1] = hi;
  }
}

/** Split a JS ns timestamp into (lo, hi) u32 pair without allocation. */
export function splitTs(ns: number, out: Int32Array): void {
  const lo = ns % 4294967296;
  out[0] = lo | 0;
  out[1] = (ns - lo) / 4294967296;
}

export class Ingestion {
  readonly ring: RingMap;
  readonly stats = new TelemetryBlock();
  /** 16B scratch payload — reused across every publish (zero alloc). */
  private readonly payload = new Uint8Array(16);
  private readonly payloadDv = new DataView(this.payload.buffer);
  private readonly tsPair = new Int32Array(2);
  /** message scratch view (read path) — caller-owned out buffers */
  readonly readOut = new Uint8Array(16);
  readonly readSeq = new Int32Array(1);
  private lastReadIdx = -1;

  constructor(ring: RingMap) {
    this.ring = ring;
  }

  /**
   * Ingest one raw message. `body` is a 16B window the caller owns.
   * Returns slot index. ZERO heap allocation on this path.
   */
  ingest(writerId: number, msgType: number, tsNs: number, body: Uint8Array): number {
    splitTs(tsNs, this.tsPair);
    return this.ring.publish(writerId, msgType, this.tsPair[0], this.tsPair[1], body, 0);
  }

  /**
   * Market-tick fast path: pack (price i64 fixed, qty u32, flags u8) into the
   * preallocated payload and publish. i64 is written as an exact lo/hi u32
   * pair (NO BigInt — BigInt construction would allocate). Zero alloc.
   */
  ingestTick(writerId: number, tsNs: number, priceFixed: number, qty: number, flags: number): number {
    const lo = priceFixed % 4294967296;
    this.payloadDv.setInt32(0, lo | 0, true);
    this.payloadDv.setInt32(4, ((priceFixed - lo) / 4294967296) | 0, true);
    this.payloadDv.setUint32(8, qty >>> 0, true);
    this.payloadDv.setUint8(12, flags & 0xff);
    return this.ingest(writerId, MSG_MARKET_TICK, tsNs, this.payload);
  }

  /** Drain the newest unread message into readOut; -1 when starved. */
  drainNewest(): number {
    const start = this.lastReadIdx < 0 ? Math.max(0, this.ring.writeIndex - 1) : this.lastReadIdx;
    const idx = this.ring.readNewestFrom(start, this.readOut, this.readSeq);
    if (idx >= 0) this.lastReadIdx = idx;
    return idx;
  }
}
