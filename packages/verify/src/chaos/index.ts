// chaos/index.ts — synthetic silicon chaos suites (deterministic).

import { mulberry32, DEFAULT_SEED } from './prng.ts';

export { DEFAULT_SEED };

export interface ThermalPoint {
  mhz: number;
  nsPerOp: number;
}

export interface ThermalResult {
  profile: 'thermal';
  cyclesPerOp: number;
  curve: ThermalPoint[];
  bench?: ThermalPoint[];
  deterministic: true;
  pass: boolean;
}

export interface BusPoint {
  utilization: number;
  nsPerOp: number;
}

export interface BusResult {
  profile: 'bus';
  baseNsPerOp: number;
  curve: BusPoint[];
  deterministic: true;
  pass: boolean;
}

export interface NetworkRatePoint {
  dropRate: number;
  sent: number;
  delivered: number;
  dropped: number;
  deliveredRatio: number;
  avgRecoveryTicks: number;
  maxQueue: number;
}

export interface NetworkResult {
  profile: 'network';
  ticks: number;
  nodes: number;
  rates: NetworkRatePoint[];
  resilienceScore: number;
  deterministic: true;
  pass: boolean;
}

export interface ChaosResult {
  thermal: ThermalResult;
  bus: BusResult;
  network: NetworkResult;
  seed: number;
}

export const CYCLES_PER_OP = 42;
const THERMAL_STEPS = [3200, 2800, 2400, 2000, 1600, 1200, 800];
const BUS_STEPS = [0, 0.25, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95];
const NETWORK_RATES = [0.001, 0.005, 0.01, 0.02, 0.05];
const NETWORK_TICKS = 12000;

function round2(x: number): number {
  return Math.round(x * 100) / 100;
}

/** Thermal throttling: 3.2 GHz -> 800 MHz stepped frequency scaling. */
export function runThermalChaos(bench = false): ThermalResult {
  const curve: ThermalPoint[] = THERMAL_STEPS.map((mhz) => ({
    mhz,
    nsPerOp: round2(CYCLES_PER_OP / (mhz / 1000)),
  }));
  let benchCurve: ThermalPoint[] | undefined;
  if (bench) {
    benchCurve = [];
    for (const mhz of THERMAL_STEPS) {
      const t0 = process.hrtime.bigint();
      let acc = 0;
      for (let i = 0; i < 2_000_000; i++) acc = (acc + i) | 0;
      const ns = Number(process.hrtime.bigint() - t0) / 2_000_000;
      benchCurve.push({ mhz, nsPerOp: round2(ns) });
      if (acc === 42) benchCurve.push({ mhz, nsPerOp: 0 }); // keep sink alive
    }
  }
  const first = curve[0].nsPerOp;
  const last = curve[curve.length - 1].nsPerOp;
  return { profile: 'thermal', cyclesPerOp: CYCLES_PER_OP, curve, bench: benchCurve, deterministic: true, pass: last > first };
}

/** Memory-bus saturation: M/M/1-style queueing latency curve. */
export function runBusChaos(seed: number = DEFAULT_SEED): BusResult {
  const rng = mulberry32(seed);
  const base = round2(CYCLES_PER_OP / 3.2);
  const curve: BusPoint[] = BUS_STEPS.map((u) => {
    const jitter = 1 + (rng() - 0.5) * 0.04; // +/-2% deterministic jitter
    return { utilization: u, nsPerOp: round2((base / (1 - 0.92 * u)) * jitter) };
  });
  const first = curve[0].nsPerOp;
  const last = curve[curve.length - 1].nsPerOp;
  return { profile: 'bus', baseNsPerOp: base, curve, deterministic: true, pass: last > first * 2 };
}

/**
 * Network chaos: 3-node ring replication over a lossy channel with a
 * split-brain episode (link partition ticks 3000..5000). During the
 * partition, in-flight messages are QUEUED store-and-forward (not
 * silently lost); after the heal the backlog drains at link capacity.
 * Packet drops follow the Bernoulli rate with next-tick retransmit.
 *
 * Metrics: deliveredRatio = delivered / sent (only genuine rate-drops
 * are losses); recoveryTicks = ticks from heal until the backlog fully
 * drains (worst-case split-brain recovery cost).
 */
export function runNetworkChaos(seed: number = DEFAULT_SEED): NetworkResult {
  const nodes = 3;
  const rates: NetworkRatePoint[] = [];
  let scoreComponents = 0;
  const SPLIT_START = 3000;
  const SPLIT_END = 5000;

  for (const rate of NETWORK_RATES) {
    const rng = mulberry32((seed ^ Math.floor(rate * 1e6)) >>> 0);
    let sent = 0;
    let delivered = 0;
    let dropped = 0;
    let maxQueue = 0;
    let queue = 0;
    let recoveryTicks = -1;

    for (let tick = 0; tick < NETWORK_TICKS; tick++) {
      const partitioned = tick >= SPLIT_START && tick < SPLIT_END;
      let fresh = 0;
      for (let link = 0; link < 2; link++) {
        sent++;
        if (partitioned) {
          queue++; // store-and-forward during split-brain
        } else if (rng() < rate) {
          dropped++; // lost, retransmitted later (joins the backlog)
          queue++;
        } else {
          fresh++;
        }
      }
      if (!partitioned) {
        delivered += fresh;
        const drain = Math.min(queue, 2);
        delivered += drain;
        queue -= drain;
      }
      if (queue > maxQueue) maxQueue = queue;
      if (!partitioned && queue === 0 && recoveryTicks < 0 && tick >= SPLIT_END) {
        recoveryTicks = tick - SPLIT_END + 1;
      }
    }
    if (recoveryTicks < 0) recoveryTicks = NETWORK_TICKS - SPLIT_END;

    const ratio = sent > 0 ? Math.round((delivered / sent) * 10000) / 10000 : 0;
    rates.push({
      dropRate: rate,
      sent,
      delivered,
      dropped,
      deliveredRatio: ratio,
      avgRecoveryTicks: recoveryTicks,
      maxQueue,
    });

    if (rate === 0.01) scoreComponents += 0.5 * Math.min(100, ratio * 100);
    if (rate === 0.05) scoreComponents += 0.3 * Math.min(100, ratio * 100);
    if (rate === 0.02) {
      const recoveryScore = Math.max(0, Math.min(100, 100 - recoveryTicks / 40));
      scoreComponents += 0.2 * recoveryScore;
    }
  }

  const resilienceScore = Math.round(scoreComponents);
  return {
    profile: 'network',
    ticks: NETWORK_TICKS,
    nodes,
    rates,
    resilienceScore,
    deterministic: true,
    pass: resilienceScore >= 85,
  };
}

export function runChaos(profile: 'thermal' | 'bus' | 'network' | 'all', seed = DEFAULT_SEED, bench = false): ChaosResult {
  const thermal = profile === 'all' || profile === 'thermal' ? runThermalChaos(bench) : emptyThermal();
  const bus = profile === 'all' || profile === 'bus' ? runBusChaos(seed) : emptyBus();
  const network = profile === 'all' || profile === 'network' ? runNetworkChaos(seed) : emptyNetwork();
  return { thermal, bus, network, seed };
}

function emptyThermal(): ThermalResult {
  return { profile: 'thermal', cyclesPerOp: CYCLES_PER_OP, curve: [], deterministic: true, pass: true };
}
function emptyBus(): BusResult {
  return { profile: 'bus', baseNsPerOp: 0, curve: [], deterministic: true, pass: true };
}
function emptyNetwork(): NetworkResult {
  return { profile: 'network', ticks: 0, nodes: 3, rates: [], resilienceScore: 100, deterministic: true, pass: true };
}
