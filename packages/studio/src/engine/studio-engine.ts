/**
 * Weft Studio — engine facade.
 * Owns the ring map, ingestion, simulators, scheduler, flight recorder and
 * the hot-plane draw registry. The UI mounts ONCE, subscribes its canvases
 * into `onFrame` via refs, and never re-renders from streaming data.
 */

import { RingMap, SLOT_DROPPED, SLOT_COMMITTED } from './ring';
import { Ingestion, TelemetryBlock, MSG_MUTATION } from './ingest';
import { StreamSimulator } from './sim';
import { FrameScheduler } from './scheduler';
import {
  FlightRecorder, TimeTravelReplayer, SBURST_TRAILER_SIZE,
} from './flightrec';
import { DEFAULT_RING_CAPACITY, FRAME_BUDGET_NS_240, hex8, clamp } from './types';
import { fnv1a32 } from './types';

export interface FrameDrawers {
  /** called in registration order inside the 240 Hz governor work callback */
  ring?: (frame: number) => void;
  telemetry?: (frame: number) => void;
  cloud?: (frame: number) => void;
  attitude?: (frame: number) => void;
  renderSpy?: (frame: number) => void;
  memoryGrid?: (frame: number) => void;
}

const P = new Float64Array(3);

/** Zero-copy efficiency index: committed payload bytes / touched bytes. */
function zeroCopyIndex(published: number, dropped: number, slotBytes: number): number {
  if (published === 0) return 1;
  const wasted = dropped * slotBytes;
  return clamp(1 - wasted / (published * slotBytes + wasted), 0, 1);
}

export class StudioEngine {
  readonly ring: RingMap;
  readonly ingestion: Ingestion;
  readonly sim: StreamSimulator;
  readonly scheduler: FrameScheduler;
  readonly flight: FlightRecorder;
  replayer: TimeTravelReplayer | null = null;
  drawers: FrameDrawers = {};
  /** live status values (status bar reads these directly — no React state) */
  live = {
    fps: 0,
    frame: 0,
    occupancy: 0,
    writers: 2,
    readers: 1,
    dropped: 0,
    torn: 0,
    zeroCopyIndex: 1,
    heapKib: 0,
    msgPerSec: 0,
    incidents: 0,
  };

  private vNowNs = 0;
  private lastWallMs = 0;
  private wallMsgAcc = 0;
  private wallMsgMark = 0;
  private readonly clock: () => number;
  private readonly scratchU32 = new Uint32Array(16);
  private readonly beforeU32 = new Int32Array(2);
  private readonly afterU32 = new Int32Array(2);

  constructor(
    readonly schemaHash: string,
    capacity: number = DEFAULT_RING_CAPACITY,
    rate = 1_000_000,
  ) {
    this.ring = new RingMap(capacity);
    this.ingestion = new Ingestion(this.ring);
    this.sim = new StreamSimulator(this.ingestion, 0x57e57100, rate);
    this.flight = new FlightRecorder(schemaHash, 0);
    this.clock = typeof performance !== 'undefined' ? () => performance.now() * 1e6 : () => Number(process.hrtime.bigint());
    this.scheduler = new FrameScheduler((frame) => this.work(frame));
  }

  /** One governed 240 Hz frame of studio work. */
  private work(frame: number): void {
    const t0 = this.clock();
    // feed streams for one 240Hz period worth of virtual time
    this.sim.tick(1e9 / 240);
    // record flight-log mutation rows for ring tail writes (throttled x1/8)
    if ((frame & 7) === 0) this.recordMutations();
    // drain newest into stats (market lane)
    const idx = this.ingestion.drainNewest();
    if (idx >= 0) {
      // sample decode — fixed-point price from payload (lo u32 / 1e9)
      const dv = this.ingestion.readOut;
      const priceLo = dv[0] | (dv[1] << 8) | (dv[2] << 16) | (dv[3] << 24);
      this.ingestion.stats.push(0, this.sim.nowNs);
      void priceLo;
    }
    // hot planes
    if (this.drawers.ring) this.drawers.ring(frame);
    if (this.drawers.telemetry) this.drawers.telemetry(frame);
    if (this.drawers.cloud) this.drawers.cloud(frame);
    if (this.drawers.attitude) this.drawers.attitude(frame);
    if (this.drawers.memoryGrid) this.drawers.memoryGrid(frame);
    if (this.drawers.renderSpy) this.drawers.renderSpy(frame);
    // status ledger
    const t1 = this.clock();
    const workNs = t1 - t0;
    this.live.frame = frame;
    this.live.occupancy = this.ring.occupancy();
    this.live.dropped = this.ring.counters.dropped;
    this.live.torn = this.ring.counters.tornRetries;
    this.live.zeroCopyIndex = zeroCopyIndex(
      this.ring.counters.published, this.ring.counters.dropped, 32,
    );
    this.ingestion.stats.push(1, workNs);
    this.ingestion.stats.push(3, this.live.occupancy);
    this.ingestion.stats.push(4, this.live.dropped);
    this.ingestion.stats.push(5, this.live.torn);
    this.ingestion.stats.push(6, this.live.zeroCopyIndex);
  }

  /** Fold ring-tail seqlock transitions into the flight log (x1/8 frames). */
  private recordMutations(): void {
    const n = Math.min(64, this.ring.counters.published - this.mutMark);
    if (n <= 0) return;
    for (let k = 0; k < n; k++) {
      const idx = (this.mutMark + k) % this.ring.capacity;
      const seqNew = this.ring.slotSeq(idx);
      if (seqNew === 0) continue;
      const base = idx * 32 + 16;
      const b0 = this.ring.bytes[base] | (this.ring.bytes[base + 1] << 8) | (this.ring.bytes[base + 2] << 16) | (this.ring.bytes[base + 3] << 24);
      const b1 = this.ring.bytes[base + 4] | (this.ring.bytes[base + 5] << 8) | (this.ring.bytes[base + 6] << 16) | (this.ring.bytes[base + 7] << 24);
      this.flight.record(
        this.ring.slotTsNs(idx), idx * 32, seqNew - 2, seqNew,
        this.beforeU32[0], this.beforeU32[1],
        b0, b1,
      );
      this.beforeU32[0] = b0; this.beforeU32[1] = b1;
      void MSG_MUTATION;
    }
    this.mutMark += n;
  }
  private mutMark = 0;

  /** Freeze the flight log into a replayer (time-travel mode). */
  freezeReplay(): TimeTravelReplayer {
    this.replayer = new TimeTravelReplayer(this.flight);
    return this.replayer;
  }

  /** One-Click Crash Export → returns the bundle bytes (fresh array, cold path). */
  exportCrashBundle(): Uint8Array {
    const streamLen = FlightRecorder.serializedSize(this.flight.recordCount);
    const out = new Uint8Array(streamLen + SBURST_TRAILER_SIZE);
    const n = this.flight.toCrashBundle(out);
    if (n < 0) throw new Error('E_EXPORT: bundle serialization failed');
    return out;
  }

  // --------------------------------------------------------------- driving --

  /** Deterministic virtual run: advance `dtNs` of studio time at 240 Hz. */
  runVirtual(dtNs: number): number {
    const from = this.vNowNs;
    this.vNowNs += dtNs;
    return this.scheduler.advance(from, dtNs, this.clock);
  }

  /** Live wall drive from the browser rAF tick. */
  driveWall(): void {
    const nowMs = this.clock() / 1e6;
    if (this.lastWallMs === 0) this.lastWallMs = nowMs;
    const dtMs = nowMs - this.lastWallMs;
    this.lastWallMs = nowMs;
    const res = this.scheduler.driveWall(nowMs * 1e6, this.clock);
    if (res === 'ran' || res === 'skipped') {
      const dtNs = dtMs * 1e6;
      this.sim.tick(Math.min(dtNs, 1e9 / 240));
      const now = nowMs;
      this.wallMsgAcc = this.ring.counters.published;
      if (now - this.wallMsgMark > 250) {
        this.live.msgPerSec = ((this.wallMsgAcc - this.msgMarkPrev) * 4) / ((now - this.wallMsgMark) / 1000);
        this.msgMarkPrev = this.wallMsgAcc;
        this.wallMsgMark = now;
      }
      this.live.fps = 240;
    }
  }
  private msgMarkPrev = 0;

  /** Telemetry percentiles (p50/p99/max frame work ns) → caller out[3]. */
  framePercentiles(out: Float64Array): void {
    this.scheduler.percentiles(out);
    void P;
  }

  /** Schema-hash echo for status bar. */
  schemaHashShort(): string {
    return hex8(fnv1a32(this.schemaHash));
  }

  /** Scratch for panels (avoid per-frame allocation in panel code). */
  get scratch(): Uint32Array { return this.scratchU32; }

  budgetNs(): number { return FRAME_BUDGET_NS_240; }

  /** Telemetry block alias. */
  get telemetry(): TelemetryBlock { return this.ingestion.stats; }

  /** Slot-state histogram for the Ring Monitor legend (into caller arrays). */
  stateHistogram(out: Int32Array): void {
    // out: [FREE, WRITING, COMMITTED, READ, DROPPED]
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0; out[4] = this.ring.counters.dropped;
    const cap = this.ring.capacity;
    const scan = Math.min(cap, 65536);
    for (let i = 0; i < scan; i++) {
      const s = this.ring.slotState(i);
      if (s === SLOT_DROPPED) out[4]++;
      else if (s === SLOT_COMMITTED) out[2]++;
      else out[0]++;
    }
  }
}
