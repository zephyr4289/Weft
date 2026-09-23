// budget.ts — the 240 FPS frame-budget ledger.
//
// WHY EXISTS: "240 FPS locked" is a CLAIM until it is a measured
// distribution. The ledger records per-frame ENGINE execution time (the
// CPU-side work between beginFrame and endFrame — the part this code
// owns) into a preallocated ring and counts violations of the frame
// budget (4,166.67 µs at 240 Hz). The bench and the browser rig read the
// percentiles; D-42 quotes them with the honesty labels (MEASURED on
// NullHAL/software ICDs; the hardware present-rate lock stays a
// hardware-tier claim, labeled).
//
// LAW 1: the frame path writes two numbers into a preallocated
// Float64Array and increments counters — nothing else. Percentile
// computation SORTS A COPY and is therefore diagnostic-only (init or
// post-run paths), never per frame.

export class FrameBudgetLedger {
  readonly targetMicros: number;
  private readonly ring: Float64Array;
  private readonly capacity: number;
  private head = 0;
  private count = 0;
  private violations = 0;
  private worstMicros = 0;
  private t0 = 0;
  private readonly clock: () => number;

  constructor(tickHz: number, windowSize = 256, clock: () => number = () => performance.now() * 1000) {
    if (tickHz <= 0) throw new RangeError(`tickHz must be > 0, got ${tickHz}`);
    this.targetMicros = 1_000_000 / tickHz;
    this.capacity = windowSize;
    this.ring = new Float64Array(windowSize);
    this.clock = clock; // injectable: deterministic tests, real clock in prod
  }

  /** Frame path begin — samples the clock (µs). */
  begin(): void {
    this.t0 = this.clock();
  }

  /** Frame path end — samples the clock, records the elapsed µs. */
  end(): void {
    const dt = this.clock() - this.t0;
    this.ring[this.head] = dt;
    this.head = (this.head + 1) % this.capacity;
    if (this.count < this.capacity) this.count++;
    if (dt > this.targetMicros) this.violations++;
    if (dt > this.worstMicros) this.worstMicros = dt;
  }

  /** Frames recorded so far. */
  get frames(): number {
    return this.count;
  }

  /** Budget violations over the ledger's whole life. */
  get budgetViolations(): number {
    return this.violations;
  }

  /** Worst single frame (µs) — the spike the HUD shows red. */
  get worst(): number {
    return this.worstMicros;
  }

  /**
   * Percentiles over the current window — DIAGNOSTIC PATH ONLY (sorts a
   * copy). Returns p50/p95/p99 in µs (zeros when empty).
   */
  percentiles(): { p50: number; p95: number; p99: number } {
    if (this.count === 0) return { p50: 0, p95: 0, p99: 0 };
    const copy = this.ring.subarray(0, this.count).slice();
    copy.sort();
    const at = (q: number): number => copy[Math.min(copy.length - 1, Math.floor(q * copy.length))];
    return { p50: at(0.5), p95: at(0.95), p99: at(0.99) };
  }

  /**
   * A JSON-ready snapshot for evidence logs — allocates by design; call
   * from benches/rigs, never from the frame loop.
   */
  snapshot(extra: Record<string, unknown> = {}): Record<string, unknown> {
    const p = this.percentiles();
    return {
      target_micros: Math.round(this.targetMicros * 100) / 100,
      frames: this.count,
      violations: this.violations,
      p50_micros: Math.round(p.p50 * 100) / 100,
      p95_micros: Math.round(p.p95 * 100) / 100,
      p99_micros: Math.round(p.p99 * 100) / 100,
      worst_micros: Math.round(this.worstMicros * 100) / 100,
      ...extra,
    };
  }
}
