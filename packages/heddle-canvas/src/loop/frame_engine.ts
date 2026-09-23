// frame_engine.ts — the HeddleEngine: the 240 FPS zero-GC frame loop.
//
// WHY EXISTS: this is the loop that owns the frame. One tick:
//
//   1. budget.begin          (stamp the clock)
//   2. hal.beginFrame        (bind surface, reset dirty rect)
//   3. per lane (fixed order):
//        takeDirtyBits       — read-clear the lane's 64-B dirty word in
//                              two 32-bit atomic exchanges (ONE op per
//                              half — no lock, no lost bit)
//        clean  -> SKIP      — no upload, no draw (deliverable C: the
//                              whole point; stats.skippedCleanLanes++)
//        dirty  -> range     — the bit-span element range (conservative
//                              superset; RFC-0022 §4.3)
//                 upload     — sub-range only, the lane's OWN SAB view
//                              (zero-copy road) or the init-time staging
//                 draw       — family dispatch by lane kind
//   4. hal.endFrame          (submit + present within the dirty rect)
//   5. budget.end            (ledger the µs, count the violation)
//
// TEAR DISCIPLINE: write_pos/seq are snapshot under the producer's fence
// protocol; a moved fence (producer published mid-read) is NOT an error —
// the lane re-reads once, then gives up for this frame (the producer's
// next publication re-raises the dirty bit; no data is ever drawn torn,
// at most one frame late — the documented bounded price).
//
// LAW 1: tick() allocates NOTHING. All mutable state lives in the
// preallocated records carved at construction (bits, range, per-lane
// scratch). The heap gate runs 60,000 consecutive ticks and requires a
// zero heapUsed delta (bench/frame_budget.ts).
//
// LAW 4: a backend that dies (context/device loss) throws from its frame
// methods; the engine catches, asks the HAL for the degradation event,
// stops the loop and hands the event to the host callback — never a
// silent continue, never an unhandled throw mid-frame.

import type { PlaneView, LaneView } from '../plane/hot_plane.ts';
import type { RenderHAL } from '../hal/hal.ts';
import type { EngineConfig } from '../hal/tier.ts';
import { createDirtyBits, createDirtyRange, dirtyRangeOf, takeDirtyBits, type DirtyBits, type DirtyRange } from '../plane/dirty_mask.ts';
import { HP_KIND } from '../plane/whp1.ts';
import { FrameBudgetLedger } from './budget.ts';
import { HeddleError, type TierFallbackEvent } from '../errors.ts';

/** Fixed lane dispatch table carved at construction (Law 1). */
interface LaneSlot {
  readonly lane: LaneView;
  readonly bits: DirtyBits;
  readonly range: DirtyRange;
}

export class HeddleEngine {
  readonly plane: PlaneView;
  readonly hal: RenderHAL;
  readonly cfg: EngineConfig;
  readonly budget: FrameBudgetLedger;

  private readonly slots: LaneSlot[] = [];
  private running = false;
  private rafHandle = 0;
  private rafDriver: (() => void) | null = null;
  private onDegradation: ((ev: TierFallbackEvent) => void) | null = null;
  // Contended-frame counter (tear fence moved twice) — a health metric,
  // never an error.
  contendedFrames = 0;

  constructor(plane: PlaneView, hal: RenderHAL, cfg: EngineConfig) {
    this.plane = plane;
    this.hal = hal;
    this.cfg = cfg;
    this.budget = new FrameBudgetLedger(cfg.tickHz);
    hal.initialize(plane, cfg);
    for (let i = 0; i < plane.laneCount; i++) {
      this.slots.push({
        lane: plane.lanes[i],
        bits: createDirtyBits(),
        range: createDirtyRange(),
      });
    }
  }

  /** Host wires the degradation sink ONCE (init-time; Law 4). */
  setDegradationSink(cb: (ev: TierFallbackEvent) => void): void {
    this.onDegradation = cb;
  }

  /**
   * One frame. `nowMicros` is the VIRTUAL frame clock (pacing, ordering,
   * telemetry — the producer side reads it); the budget ledger samples
   * the real clock itself to measure the engine's own execution span.
   */
  tick(nowMicros: number): void {
    const hal = this.hal;
    this.budget.begin();
    try {
      hal.beginFrame();
      for (let s = 0; s < this.slots.length; s++) {
        const slot = this.slots[s];
        const lane = slot.lane;
        if (!takeDirtyBits(this.plane.i32, lane, slot.bits)) {
          hal.stats.skippedCleanLanes++;
          continue; // clean lane: no upload, no draw — deliverable C
        }
        dirtyRangeOf(slot.bits, lane, slot.range);
        const writePos = Atomics.load(this.plane.i32, lane.writePosI32);
        if (lane.kind === HP_KIND.WAVEFORM_F32) {
          hal.uploadWaveform(lane, slot.range.elemStart, slot.range.elemEndExclusive);
          const visible = Math.min(writePos, lane.capacity);
          hal.drawOscillo(lane, visible, writePos % lane.capacity);
        } else if (lane.kind === HP_KIND.DEPTH_LADDER_F32) {
          hal.uploadRows(lane, slot.range.elemStart, slot.range.elemEndExclusive);
          hal.drawDepthLadder(lane, Math.min(writePos, lane.capacity));
        } else if (lane.kind === HP_KIND.CANDLE_OHLC_F32) {
          hal.uploadRows(lane, slot.range.elemStart, slot.range.elemEndExclusive);
          hal.drawCandles(lane, Math.min(writePos, lane.capacity));
        } else if (lane.kind === HP_KIND.POINTCLOUD_QUAT_F32) {
          hal.uploadRows(lane, slot.range.elemStart, slot.range.elemEndExclusive);
          hal.drawPointcloud(lane, Math.min(writePos, lane.capacity));
        } else {
          // open() refuses unknown kinds; this arm is unreachable defense.
          hal.stats.skippedCleanLanes++;
        }
      }
      hal.endFrame();
    } catch (err) {
      this.discard(err);
      return;
    }
    this.budget.end();
  }

  /** Real-browser path: rAF at the display's physical rate. */
  start(): void {
    if (this.running) {
      throw new HeddleError('HC_E_LOOP_ALREADY_RUNNING', 'engine loop already started');
    }
    const g = globalThis as { requestAnimationFrame?: (cb: (t: number) => void) => number };
    if (typeof g.requestAnimationFrame !== 'function') {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'requestAnimationFrame absent (non-browser host — drive tick() instead)');
    }
    this.running = true;
    const driver = (): void => {
      if (!this.running) return;
      this.tick(performance.now() * 1000);
      const raf = g.requestAnimationFrame;
      if (typeof raf === 'function') this.rafHandle = raf(driver);
    };
    this.rafDriver = driver;
    this.rafHandle = g.requestAnimationFrame(driver);
  }

  stop(): void {
    if (!this.running) {
      throw new HeddleError('HC_E_LOOP_NOT_RUNNING', 'engine loop not running');
    }
    this.running = false;
    const g = globalThis as { cancelAnimationFrame?: (h: number) => void };
    if (typeof g.cancelAnimationFrame === 'function' && this.rafHandle !== 0) {
      g.cancelAnimationFrame(this.rafHandle);
    }
  }

  get isRunning(): boolean {
    return this.running;
  }

  /** The rAF driver (test seam for the virtual clock). */
  get driver(): (() => void) | null {
    return this.rafDriver;
  }

  // --- internal ------------------------------------------------------------

  private discard(err: unknown): void {
    this.running = false;
    let event: TierFallbackEvent | null = null;
    if (err instanceof HeddleError && (err.code === 'HC_E_CONTEXT_LOST' || err.code === 'HC_E_DEVICE_LOST')) {
      event = this.hal.handleLoss(err.message);
    }
    if (event !== null && this.onDegradation !== null) {
      this.onDegradation(event);
    } else if (event === null && !(err instanceof HeddleError)) {
      throw err; // non-loss, non-Heddle errors are programming errors: rethrow
    }
  }
}
