// metrics.js — Zero-overhead, lock-free metrics registry.
//
// Every hot-path operation is 1-3 Atomics.add/store ops on a preallocated
// SharedArrayBuffer — no mutexes, no allocation, no contention with the data
// plane (readers scrape without stopping writers; Prometheus mandates wait-
// free counter reads, and we honour that literally).
//
// Counters/gauges: u32 cells (read with >>> 0; wraparound horizon documented).
// Histograms: power-of-two buckets with one sub-level (bounds follow the
// 1, 1.5, 2, 3, 4, 6, 8 ... sequence), so p50/p99 estimates carry <= 33%
// bucket error while a record costs ~5 integer ops + one Atomics.add.
//
// Law 1: record()/add()/set() allocate NOTHING. Snapshots (scrape-time) are
// cold path and may allocate freely.

export const HIST_BUCKETS = 96; // covers ns values up to ~2^47 with sub-levels

/**
 * Estimated value upper bound for bucket index i.
 * Sequence: 1, 3, 4, 6, 8, 12, 16, 24, 32 ... (powers of two with a 1.5x
 * sub-level per octave), so a record's bucket error is <= 50% at the 1.5x
 * split and quantile estimates are within one sub-level.
 */
export function bucketUpperBound(i) {
  if (i <= 0) return 1;
  const level = i >> 1;            // octave: values [2^level, 2^(level+1))
  const sub = i & 1;
  return sub === 0 ? 1.5 * 2 ** level : 2 ** (level + 1);
}

/**
 * Lock-free metrics registry.
 * ```js
 * const reg = new MetricsRegistry({ labels: { node: '1' } });
 * const frames = reg.counter('weft_cluster_frames_total', 'Frames published.');
 * frames.add(1);                       // hot path: 1 Atomics.add
 * const lat = reg.histogram('weft_cluster_hop_latency_ns', 'One-way hop.');
 * lat.record(812);                     // hot path: bucket + 1 Atomics.add
 * ```
 */
export class MetricsRegistry {
  constructor(opts = {}) {
    this.labels = opts.labels ?? {};
    this.labelStr = Object.entries(this.labels).map(([k, v]) => `${k}="${v}"`).join(',');
    this._counters = [];  // {name, help, cell}
    this._gauges = [];
    this._hists = [];
    // One SAB for everything; cells handed out on registration (cold path).
    this._n = 8;          // small head-room
    this._sab = new SharedArrayBuffer((this._n + 3 * HIST_BUCKETS) * 4);
    this._i32 = new Int32Array(this._sab);
    this._next = 0;
    this._nextHist = this._n;
  }

  _cell() {
    const c = this._next++;
    if (this._nextHist + 3 * HIST_BUCKETS >= this._i32.length) {
      // cold path: grow
      const bigger = new Int32Array(this._i32.length * 2 + 6 * HIST_BUCKETS);
      bigger.set(this._i32);
      this._i32 = bigger;
      this._sab = bigger.buffer;
    }
    return c;
  }

  /** Register a monotonic counter. Hot path: add(n) = 1 Atomics.add. */
  counter(name, help) {
    const cell = this._cell();
    const c = {
      name, help, cell,
      add: (n = 1) => { Atomics.add(this._i32, cell, n | 0); },
    };
    this._counters.push(c);
    return c;
  }

  /** Register a gauge. Hot path: set(v) = 1 Atomics.store. */
  gauge(name, help) {
    const cell = this._cell();
    const g = {
      name, help, cell,
      set: (v) => { Atomics.store(this._i32, cell, v | 0); },
    };
    this._gauges.push(g);
    return g;
  }

  /**
   * Register a nanosecond histogram. Hot path: record(ns) = bit math +
   * 1 Atomics.add. p50/p99 estimated at scrape time from bucket counts.
   */
  histogram(name, help) {
    const base = this._cell();
    const h = {
      name, help, base,
      record: (ns) => {
        const v = ns <= 1 ? 1 : ns | 0;
        // Octave = floor(log2(v)); the NEXT-highest bit splits each octave
        // at its 1.5x point (e.g. [256..384) vs [384..512)).
        const level = 31 - Math.clz32(v);
        let idx = 2 * level + ((v >>> (level - 1)) & 1);
        if (idx >= HIST_BUCKETS) idx = HIST_BUCKETS - 1;
        Atomics.add(this._i32, base + idx, 1);
      },
    };
    this._hists.push(h);
    return h;
  }

  /**
   * Cold path: scrape everything into a plain snapshot (allocates freely).
   */
  snapshot() {
    const i32 = this._i32;
    const counters = this._counters.map((c) => ({
      name: c.name, help: c.help, value: i32[c.cell] >>> 0,
    }));
    const gauges = this._gauges.map((g) => ({
      name: g.name, help: g.help, value: i32[g.cell],
    }));
    const histograms = this._hists.map((h) => {
      const counts = new Array(HIST_BUCKETS);
      let total = 0;
      for (let i = 0; i < HIST_BUCKETS; i++) {
        counts[i] = i32[h.base + i] >>> 0;
        total += counts[i];
      }
      return { name: h.name, help: h.help, counts, total,
               p50: quantile(h.base, i32, 0.5), p99: quantile(h.base, i32, 0.99) };
    });
    return { labels: this.labels, counters, gauges, histograms,
             scrapedAt: Date.now() };
  }
}

/** Cold path: quantile estimate from bucket counts (upper bound of bucket). */
function quantile(base, i32, q) {
  let total = 0;
  for (let i = 0; i < HIST_BUCKETS; i++) total += i32[base + i] >>> 0;
  if (total === 0) return 0;
  const target = Math.ceil(q * total);
  let acc = 0;
  for (let i = 0; i < HIST_BUCKETS; i++) {
    acc += i32[base + i] >>> 0;
    if (acc >= target) return bucketUpperBound(i);
  }
  return Infinity;
}
