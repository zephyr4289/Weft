/**
 * Weft Studio — deterministic stream simulators (seeded, zero entropy).
 * These drive the studio when Engineer 2's live inspector seam is not
 * attached (STUDIO-SEAMS-V1 §6): a trading feed (up to 10,000,000 msg/s
 * nominal) and a 120 FPS robotics vision stream (point cloud + 6-DOF IMU).
 * All simulators allocate NOTHING in steady state.
 */

import { Ingestion, MSG_IMU_SAMPLE, MSG_FRAME_EVENT, splitTs } from './ingest';

/** mulberry32 — deterministic u32 PRNG, zero alloc. */
export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export interface SimStats {
  producedTicks: number;
  producedImu: number;
  producedFrames: number;
  nominalMsgPerSec: number;
}

const IMU_VALUES = 8; // 6-DOF + quat pad -> 8 f32 slots in 32B? payload is 16B: 4 f32

/**
 * Trading + robotics feed simulator. `tick()` advances the virtual clock by
 * `dtNs` and emits the corresponding message budget into the ingestion.
 * Burst-capable: nominalMsgPerSec up to 10_000_000 (message budget is
 * integral-tracked with a remainder carry — no drift).
 */
export class StreamSimulator {
  readonly rng: () => number;
  readonly imu = new Float32Array(6);
  readonly cloud = new Float32Array(4096 * 3); // point cloud XYZ
  private readonly body = new Uint8Array(16);
  private readonly bodyDv = new DataView(this.body.buffer);
  private readonly tsPair = new Int32Array(2);
  private tickCarry = 0;
  private frameCarry = 0;
  nowNs = 0;
  stats: SimStats = { producedTicks: 0, producedImu: 0, producedFrames: 0, nominalMsgPerSec: 0 };

  constructor(
    private readonly ingestion: Ingestion,
    seed = 0x57e57100,
    public nominalMsgPerSec = 1_000_000,
  ) {
    this.rng = mulberry32(seed);
    for (let i = 0; i < this.cloud.length; i++) this.cloud[i] = this.rng() * 2 - 1;
    this.stats.nominalMsgPerSec = nominalMsgPerSec;
  }

  setRate(msgPerSec: number): void {
    this.nominalMsgPerSec = msgPerSec;
    this.stats.nominalMsgPerSec = msgPerSec;
  }

  /** Advance virtual time; emit market ticks + robotics frames. */
  tick(dtNs: number): void {
    this.nowNs += dtNs;

    // --- trading stream (writer lane 0) ---
    this.tickCarry += (this.nominalMsgPerSec * dtNs) / 1e9;
    let n = Math.floor(this.tickCarry);
    this.tickCarry -= n;
    // hard per-tick cap keeps the frame budget honest at 240Hz preview cadence
    if (n > 120_000) n = 120_000;
    for (let i = 0; i < n; i++) {
      const priceFixed = 1_000_000_000 + Math.floor(this.rng() * 2_000_000_000);
      const qty = Math.floor(this.rng() * 5000);
      const flags = this.rng() < 0.02 ? 1 : 0;
      this.ingestion.ingestTick(0, this.nowNs, priceFixed, qty, flags);
    }
    this.stats.producedTicks += n;

    // --- robotics 120 FPS (writer lane 1) ---
    this.frameCarry += (120 * dtNs) / 1e9;
    const frames = Math.floor(this.frameCarry);
    this.frameCarry -= frames;
    for (let f = 0; f < frames; f++) {
      this.mutateCloud();
      this.mutateImu();
      // publish IMU sample: 4 f32 (roll, pitch, yaw, conf) in 16B payload
      this.bodyDv.setFloat32(0, this.imu[0], true);
      this.bodyDv.setFloat32(4, this.imu[1], true);
      this.bodyDv.setFloat32(8, this.imu[2], true);
      this.bodyDv.setFloat32(12, this.imu[5] * 0.01 + 0.5, true);
      this.ingestion.ingest(1, MSG_IMU_SAMPLE, this.nowNs, this.body);
      this.stats.producedImu++;
      // frame event carries the cloud revision in the payload (lo u32)
      splitTs(this.nowNs, this.tsPair);
      this.bodyDv.setUint32(0, this.cloudRev | 0, true);
      this.bodyDv.setUint32(4, this.tsPair[0], true);
      this.ingestion.ingest(1, MSG_FRAME_EVENT, this.nowNs, this.body);
      this.stats.producedFrames++;
    }
  }

  private cloudRev = 0;

  private mutateCloud(): void {
    const c = this.cloud;
    // deterministic drift — rotate 64 points per frame, no allocation
    for (let k = 0; k < 192; k++) {
      const i = (this.cloudRev * 192 + k) % c.length;
      c[i] = c[i] * 0.998 + (this.rng() * 2 - 1) * 0.002;
    }
    this.cloudRev++;
  }

  private mutateImu(): void {
    const imu = this.imu;
    for (let i = 0; i < 6; i++) {
      imu[i] = imu[i] * 0.995 + (this.rng() * 2 - 1) * 0.03;
    }
  }
}
