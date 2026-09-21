// demos/trading-terminal-240fps/producer.mjs — deterministic synthetic market
// feed for the heddle-2.0 flight demo. 16 HPL1 lanes, 100,000 ticks/second
// aggregate, seeded LCG → identical series on every run (evidence is
// reproducible). ZERO allocation in the tick path (Law 1): values are written
// into a caller-owned scratch row.
//
// Lane map (multi-panel financial dashboard):
//   0..3   last-trade price streams  (oscilloscope, candlestick)
//   4..5   robotics telemetry waves  (oscilloscope)
//   6..8   bid level sizes L1..L3    (order book)
//   9..11  ask level sizes L1..L3    (order book)
//   12..13 bid/ask top-of-book price (order book mid)
//   14..15 sensor/audio channels     (audio meter)

import { HotPlaneProducer } from '../../packages/heddle-core/src/index.js';

export const LANE_COUNT = 16;
export const SAMPLES_PER_LANE = 256;

function lcg(seed) {
  let s = seed >>> 0;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s;
  };
}

export class MarketFeed {
  // prices: 4 symbols around base levels; book: sizes around depth levels
  constructor({ seed = 0x51D5EEDB } = {}) {
    this.rnd = lcg(seed);
    this.tickIndex = 0;
    this.row = new Float64Array(LANE_COUNT); // preallocated tick row (Law 1)
  }

  // ONE tick: update ONE lane (i % 16). Deterministic drift + micro noise.
  tick() {
    const i = this.tickIndex;
    const lane = i % LANE_COUNT;
    const t = Math.floor(i / LANE_COUNT);
    let v;
    if (lane <= 3) v = 100 + lane * 10 + Math.sin(t / 64) * 8 + (this.rnd() % 17) / 32;
    else if (lane <= 5) v = 50 + lane * 4 + Math.sin(t / 32 + lane) * 12;
    else if (lane <= 8) v = 900 + (8 - lane) * 220 + (this.rnd() % 64);
    else if (lane <= 11) v = 950 + (lane - 9) * 240 + (this.rnd() % 64);
    else if (lane <= 13) v = 100 + (lane - 12) * 0.5 + Math.cos(t / 90) * 0.8;
    else v = Math.abs(Math.sin(t / 12 + lane)) * 0.9; // 0..1 audio-ish
    this.row[lane] = v;
    this.tickIndex += 1;
    return lane;
  }

  // Burst helper: emit `count` ticks through the producer (ingestion stage).
  burst(producer, count, startNs, nsPerTick) {
    let lastNow = startNs;
    for (let i = 0; i < count; i++) {
      const lane = this.tick();
      const nowNs = startNs + Math.round((i + 1) * nsPerTick);
      producer.publishLane(lane, this.row[lane], nowNs);
      lastNow = nowNs;
    }
    return lastNow;
  }

  // Full-frame helper for the frame-budget stage: one value per lane.
  frameRow() {
    // advance 16 ticks (one full lane sweep) and publish as a frame batch
    for (let l = 0; l < LANE_COUNT; l++) this.tick();
    return this.row;
  }
}

export function createDemoPlane() {
  const producer = HotPlaneProducer.create({
    laneCount: LANE_COUNT, samplesPerLane: SAMPLES_PER_LANE, tickHz: 240,
  });
  return producer;
}
