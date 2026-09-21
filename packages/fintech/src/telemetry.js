// telemetry.js — steady-state market-data telemetry with ZERO allocation.
//
// Law 2 (zero-GC steady state): every counter lives in a preallocated
// Float64Array/Uint32Array slot. Per-message updates are in-place arithmetic
// only — no objects, no closures, no array growth, no strings.
//
// Exposed metrics (the WeftOrderBook telemetry bar binds to these slots):
//   msgsTotal, bytesTotal, rateEwma (msgs/sec), latEwma (ns/message),
//   latency histogram (32 log2 buckets) with cold-path percentile readout.

export const HIST_BUCKETS = 32;

export class MarketTelemetry {
  constructor() {
    this.slots = new Float64Array(8);
    // [0] msgsTotal [1] bytesTotal [2] rateEwma [3] latEwmaNs
    // [4] lastTs [5] lastByteOff [6] windowStartTs [7] reserved
    this.hist = new Uint32Array(HIST_BUCKETS);
    this.histTotal = 0;
  }

  get msgsTotal() { return this.slots[0]; }
  get bytesTotal() { return this.slots[1]; }
  get rateEwma() { return this.slots[2]; }
  get latEwmaNs() { return this.slots[3]; }

  // Per-message hook — the hot path. Integer/float in-place math only.
  record(bytes, nowNs) {
    const s = this.slots;
    s[0] += 1;
    s[1] += bytes;
    const dt = nowNs - s[6];
    if (dt > 0) {
      const inst = 1e9 / dt;
      s[2] += (inst - s[2]) * 0.001; // EWMA over ~1000 messages
    }
    s[6] = nowNs;
  }

  recordLatency(ns) {
    const b = ns > 0 ? 31 - Math.clz32(ns) : 0; // log2 bucket, no Math.log
    this.hist[b <= 0 ? 0 : b >= HIST_BUCKETS ? HIST_BUCKETS - 1 : b] += 1;
    this.histTotal++;
    this.slots[3] += (ns - this.slots[3]) * 0.001;
  }

  // Cold path: percentile readout [p50, p99, max] bucket boundaries in ns.
  percentiles() {
    const out = [0, 0, 0];
    if (this.histTotal === 0) return out;
    let acc = 0;
    let p50 = -1, p99 = -1, max = -1;
    for (let b = 0; b < HIST_BUCKETS; b++) {
      acc += this.hist[b];
      if (acc >= this.histTotal * 0.5 && p50 < 0) p50 = b;
      if (acc >= this.histTotal * 0.99 && p99 < 0) p99 = b;
      if (this.hist[b] > 0) max = b;
    }
    out[0] = p50 < 0 ? 0 : 2 ** p50;
    out[1] = p99 < 0 ? 0 : 2 ** p99;
    out[2] = max < 0 ? 0 : 2 ** (max + 1) - 1;
    return out;
  }
}
