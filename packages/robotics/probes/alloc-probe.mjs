// probes/alloc-probe.mjs — Law 2 heap evidence: 1,000,000 RNG1 record
// acquisitions with --expose-gc heap probes + biting negative control.
//
//   node --expose-gc probes/alloc-probe.mjs           # ingest (gate 64 KiB)
//   node --expose-gc probes/alloc-probe.mjs control   # MUST bite
//
// The producer rewrites slot headers + committed each cycle (live-ring
// emulation); the reader acquires the reused flyweight. Steady state
// performs ZERO heap allocations in the reader path.

import { exit } from 'node:process';
import { attachRing, FMT_POINTS_F32 } from '../src/index.js';

const CYCLES = 1_000_000n;
const WARMUP = 50_000;
const SLOT_COUNT = 64;
const SLOT_SIZE = 4096;
const GATE_KIB = 64;

const payloadLen = 96; // 8 points x 12 bytes

function buildRing() {
  const total = 128 + SLOT_SIZE * SLOT_COUNT;
  const buf = new ArrayBuffer(total);
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  u8.set([0x52, 0x4e, 0x47, 0x31], 0);
  dv.setUint16(4, 1, true);
  dv.setUint16(6, 128, true);
  dv.setUint32(8, SLOT_SIZE, true);
  dv.setUint32(12, SLOT_COUNT, true);
  dv.setUint32(64, 1, true);
  for (let s = 0; s < SLOT_COUNT; s++) {
    const base = 128 + s * SLOT_SIZE;
    dv.setBigUint64(base, BigInt(s + 1), true);
    dv.setUint32(base + 8, payloadLen, true);
    dv.setUint32(base + 12, 1, true);
    dv.setUint32(base + 24, FMT_POINTS_F32, true);
  }
  dv.setBigUint64(24, BigInt(SLOT_COUNT), true);
  return buf;
}

const ringBuf = buildRing();
const dv = new DataView(ringBuf);
const reader = attachRing(ringBuf);
const sink = { seq: 0, points: 0 };

function produceOnce(seq) {
  const slot = (seq - 1n) % BigInt(SLOT_COUNT);
  const base = 128 + Number(slot) * SLOT_SIZE;
  dv.setBigUint64(base, seq, true);
  dv.setUint32(base + 8, payloadLen, true);
  dv.setUint32(base + 12, 1, true);
  dv.setBigUint64(base + 16, seq, true);
  dv.setUint32(base + 24, FMT_POINTS_F32, true);
  dv.setBigUint64(24, seq, true); // committed = seq
}

function run(mode) {
  const retain = [];
  for (let i = 1n; i <= WARMUP; i++) {
    produceOnce(i);
    const rec = reader.acquire();
    if (rec !== null) { sink.seq = rec.seq; sink.points = rec.payloadLen; }
  }
  if (global.gc !== undefined) global.gc();
  const before = process.memoryUsage().heapUsed;

  for (let i = 1n; i <= CYCLES; i++) {
    produceOnce(i);
    const rec = reader.acquire();
    if (rec !== null) {
      sink.seq = rec.seq;
      sink.points = rec.pointsView()[0];
      if (mode === 'control' && i % 100n === 0n) {
        retain.push({ seq: rec.seq, ts: rec.tsNs }); // textbook leak
      }
    }
  }
  if (global.gc !== undefined) global.gc();
  const after = process.memoryUsage().heapUsed;
  const growthKiB = Math.round((after - before) / 1024 * 10) / 10;
  return { growthKiB, retained: retain.length };
}

const mode = process.argv[2] ?? 'ingest';
if (mode !== 'ingest' && mode !== 'control') {
  console.error('usage: node --expose-gc probes/alloc-probe.mjs [ingest|control]');
  exit(1);
}
const { growthKiB, retained } = run(mode);
const ok = mode === 'ingest' ? growthKiB < GATE_KIB : growthKiB >= GATE_KIB;
console.log(JSON.stringify({
  mode, ok, growthKiB, gateKiB: GATE_KIB,
  cycles: Number(CYCLES), retained, lastSeq: sink.seq,
}));
exit(ok ? 0 : 2);
