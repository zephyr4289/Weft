/**
 * Weft Studio — governed 240 Hz frame scheduler.
 *
 * Two driving modes share one budget ledger:
 *  - `advance(fromNs, dtNs, clock)` — deterministic virtual-cadence driver
 *    (managed suite + demo): executes EVERY due frame inline, measuring real
 *    work cost via `clock()`. When work fits the 4.166 ms budget the skip
 *    counter stays at 0 — that is the Stage-4 proof surface.
 *  - `driveWall(nowNs, clock)` — live rAF driver for the browser: the virtual
 *    due cursor advances one period per executed frame; when the host falls
 *    more than one period behind, the frame is SKIPPED (drop-not-queue),
 *    never queued.
 */

import { FRAME_BUDGET_NS_240 } from './types';

const STAT_RING = 256;
const PERIOD_240_NS = 1e9 / 240;

export class FrameScheduler {
  private readonly workRing = new Float64Array(STAT_RING);
  private workHead = 0;
  private framesRun = 0;
  private framesSkipped = 0;
  private maxWorkNs = 0;
  private stallFrames = 0;
  private vDueNs = 0;

  constructor(private readonly work: (frame: number) => void) {}

  /** Deterministic virtual advance over [fromNs, fromNs + dtNs). */
  advance(fromNs: number, dtNs: number, clock: () => number): number {
    const end = fromNs + dtNs;
    let ran = 0;
    let guard = 0;
    while (this.vDueNs < end && guard < 4096) {
      const t0 = clock();
      this.work(this.framesRun + ran);
      const t1 = clock();
      this.recordWork(t1 - t0);
      ran++;
      guard++;
      this.vDueNs += PERIOD_240_NS;
    }
    this.framesRun += ran;
    return ran;
  }

  /** Live wall-clock driver. Returns 'ran' | 'skipped' | 'idle'. */
  driveWall(nowNs: number, clock: () => number): 'ran' | 'skipped' | 'idle' {
    if (nowNs < this.vDueNs) return 'idle';
    // drop-not-queue: any due frames older than one period are skipped
    let behind = 0;
    while (this.vDueNs + PERIOD_240_NS <= nowNs && behind < 4096) {
      this.vDueNs += PERIOD_240_NS;
      behind++;
      this.framesSkipped++;
    }
    const t0 = clock();
    this.work(this.framesRun);
    const t1 = clock();
    this.recordWork(t1 - t0);
    this.vDueNs += PERIOD_240_NS;
    this.framesRun++;
    return 'ran';
  }

  private recordWork(ns: number): void {
    this.workRing[this.workHead] = ns;
    this.workHead = (this.workHead + 1) % STAT_RING;
    if (ns > this.maxWorkNs) this.maxWorkNs = ns;
    if (ns > FRAME_BUDGET_NS_240) this.stallFrames++;
  }

  get frameCount(): number { return this.framesRun; }
  get skipped(): number { return this.framesSkipped; }
  get stalls(): number { return this.stallFrames; }
  get dueNs(): number { return this.vDueNs; }

  /** p50/p99/max over the last 256 measured frames → caller-owned out[3]. */
  percentiles(out: Float64Array): void {
    const scratch: number[] = [];
    for (let i = 0; i < STAT_RING; i++) if (this.workRing[i] > 0) scratch.push(this.workRing[i]);
    scratch.sort((a, b) => a - b);
    const n = scratch.length;
    out[0] = n ? scratch[Math.floor(n * 0.5)] : 0;
    out[1] = n ? scratch[Math.min(n - 1, Math.floor(n * 0.99))] : 0;
    out[2] = this.maxWorkNs;
  }

  resetStats(): void {
    this.workRing.fill(0);
    this.workHead = 0;
    this.framesRun = 0;
    this.framesSkipped = 0;
    this.maxWorkNs = 0;
    this.stallFrames = 0;
    this.vDueNs = 0;
  }
}
