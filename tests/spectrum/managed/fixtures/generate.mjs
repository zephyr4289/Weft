#!/usr/bin/env node
// generate.mjs — SHP1 golden fixture generator (Pillar 5, weft-spectrum managed).
//
// Deterministic by construction: fixed field values, fixed CRC table, zero
// entropy sources (no Math.random / Date / pid). The shard re-runs this into
// a scratch dir and byte-compares against the committed fixtures — any drift
// fails CI (silent-green contract).
//
// Outputs (into --out, default = this directory):
//   hw-profile-flagship.bin   Tier 1 — M4-Max-class, 240 FPS
//   hw-profile-mid.bin        Tier 2 — AVX2 workstation, 120 FPS
//   hw-profile-budget.bin     Tier 3 — budget phone, severe thermal, hidden
//   hw-profile-torn.bin       budget bytes with payload corruption (CRC must bite)
//   expected_profile.json     canonical decoded values — parity source of truth
//   governor_vector.json      frozen cadence-governor input timeline + expected outputs

import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = (() => {
  const i = process.argv.indexOf('--out');
  if (i > 0 && process.argv[i + 1]) return process.argv[i + 1];
  return dirname(fileURLToPath(import.meta.url));
})();
mkdirSync(OUT, { recursive: true });

// ---------------------------------------------------------------------------
// SHP1 constants (docs/spectrum/SPECTRUM-WIRE-V1.md — byte-frozen)
// ---------------------------------------------------------------------------
const RECORD_SIZE = 192;
const CRC_OFFSET = 188;
const FEATURE_BITS = {
  WASM_SIMD128: 0, SHARED_ARRAY_BUFFER: 1, WEBGPU: 2, WEBGL2: 3,
  AVX512: 4, AVX2: 5, SSE42: 6, NEON: 7, SVE2: 8, RVV: 9,
  METAL_3: 10, CUDA: 11, APPLE_MPS: 12, OPENVINO: 13,
  FASTRPC_DSP: 14, NEUROPILOT: 15, MULTILANE_DMA: 16, BIG_LITTLE: 17,
  THERMAL_SENSOR: 18, DLPACK_EXPORT: 19,
};

// CRC-32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF) — self-contained so
// every language implementation can be compared against identical arithmetic.
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) !== 0 ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c;
  }
  return t;
})();

function crc32(bytes, len) {
  let c = -1;
  for (let i = 0; i < len; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

// ---------------------------------------------------------------------------
// Record writer
// ---------------------------------------------------------------------------
function buildRecord(p) {
  const b = new Uint8Array(RECORD_SIZE);
  const dv = new DataView(b.buffer);
  b[0] = 0x53; b[1] = 0x48; b[2] = 0x50; b[3] = 0x31; // "SHP1"
  dv.setUint16(4, 1, true);            // layout_version
  dv.setUint16(6, RECORD_SIZE, true);  // record_size
  let lo = 0, hi = 0;
  for (const name of p.features) {
    const bit = FEATURE_BITS[name];
    if (bit === undefined) throw new Error(`unknown feature: ${name}`);
    if (bit < 32) lo |= (1 >>> 0) << bit; else hi |= (1 >>> 0) << (bit - 32);
  }
  dv.setUint32(8, lo >>> 0, true);
  dv.setUint32(12, hi >>> 0, true);
  dv.setUint32(16, p.siliconTier, true);
  dv.setUint32(20, p.thermalState, true);
  dv.setUint32(24, p.perfCores, true);
  dv.setUint32(28, p.effCores, true);
  dv.setUint32(32, p.gpuFamily, true);
  dv.setUint32(36, p.cacheLineBytes, true);
  dv.setBigUint64(40, BigInt(p.cpuMaxClockKhz), true);
  dv.setBigUint64(48, BigInt(p.memoryTotalBytes), true);
  dv.setBigUint64(56, BigInt(p.memoryBudgetBytes), true);
  dv.setUint32(64, p.simdWidthBits, true);
  dv.setUint32(68, p.frameBudgetUs, true);
  dv.setBigUint64(72, BigInt(p.maxFrameRateMilliHz), true);
  dv.setUint32(80, p.batteryPermille, true);
  dv.setUint32(84, p.batteryCharging, true);
  dv.setUint32(88, p.visibility, true);
  dv.setUint32(92, p.dmaLaneCount, true);
  dv.setUint32(96, p.vendorId, true);
  dv.setUint32(100, p.deviceId, true);
  dv.setUint32(CRC_OFFSET, crc32(b, CRC_OFFSET), true);
  return b;
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------
const F = FEATURE_BITS;
const PROFILES = [
  {
    file: 'hw-profile-flagship.bin', name: 'flagship',
    features: ['WASM_SIMD128', 'SHARED_ARRAY_BUFFER', 'WEBGPU', 'WEBGL2', 'NEON', 'SVE2',
      'METAL_3', 'APPLE_MPS', 'MULTILANE_DMA', 'BIG_LITTLE', 'THERMAL_SENSOR', 'DLPACK_EXPORT'],
    siliconTier: 1, thermalState: 0, perfCores: 8, effCores: 4, gpuFamily: 1,
    cacheLineBytes: 128, cpuMaxClockKhz: 4500000, memoryTotalBytes: 68719476736,
    memoryBudgetBytes: 8589934592, simdWidthBits: 128, frameBudgetUs: 4166,
    maxFrameRateMilliHz: 240000, batteryPermille: 1000, batteryCharging: 1,
    visibility: 0, dmaLaneCount: 4, vendorId: 0x106b, deviceId: 0x0034,
  },
  {
    file: 'hw-profile-mid.bin', name: 'mid',
    features: ['WASM_SIMD128', 'SHARED_ARRAY_BUFFER', 'WEBGPU', 'WEBGL2', 'AVX2', 'SSE42',
      'CUDA', 'OPENVINO', 'THERMAL_SENSOR', 'DLPACK_EXPORT'],
    siliconTier: 2, thermalState: 1, perfCores: 6, effCores: 0, gpuFamily: 2,
    cacheLineBytes: 64, cpuMaxClockKhz: 3200000, memoryTotalBytes: 17179869184,
    memoryBudgetBytes: 2147483648, simdWidthBits: 256, frameBudgetUs: 8333,
    maxFrameRateMilliHz: 120000, batteryPermille: 0xffff, batteryCharging: 2,
    visibility: 0, dmaLaneCount: 2, vendorId: 0x8086, deviceId: 0x9a49,
  },
  {
    file: 'hw-profile-budget.bin', name: 'budget',
    features: ['WASM_SIMD128', 'SHARED_ARRAY_BUFFER', 'NEON', 'NEUROPILOT', 'FASTRPC_DSP',
      'BIG_LITTLE', 'THERMAL_SENSOR', 'DLPACK_EXPORT'],
    siliconTier: 3, thermalState: 3, perfCores: 2, effCores: 6, gpuFamily: 3,
    cacheLineBytes: 64, cpuMaxClockKhz: 2000000, memoryTotalBytes: 4294967296,
    memoryBudgetBytes: 536870912, simdWidthBits: 128, frameBudgetUs: 16666,
    maxFrameRateMilliHz: 60000, batteryPermille: 180, batteryCharging: 0,
    visibility: 1, dmaLaneCount: 1, vendorId: 0x5143, deviceId: 0x0607,
  },
];

// ---------------------------------------------------------------------------
// expected_profile.json — canonical decoded values (parity source of truth)
// ---------------------------------------------------------------------------
const expected = {};
for (const p of PROFILES) {
  const bytes = buildRecord(p);
  writeFileSync(join(OUT, p.file), bytes);
  expected[p.name] = {
    file: p.file,
    magic: 'SHP1', layoutVersion: 1, recordSize: RECORD_SIZE,
    features: p.features,
    featureFlagsLo: new DataView(bytes.buffer).getUint32(8, true) >>> 0,
    featureFlagsHi: new DataView(bytes.buffer).getUint32(12, true) >>> 0,
    siliconTier: p.siliconTier, thermalState: p.thermalState,
    perfCores: p.perfCores, effCores: p.effCores, gpuFamily: p.gpuFamily,
    cacheLineBytes: p.cacheLineBytes, cpuMaxClockKhz: p.cpuMaxClockKhz,
    memoryTotalBytes: p.memoryTotalBytes, memoryBudgetBytes: p.memoryBudgetBytes,
    simdWidthBits: p.simdWidthBits, frameBudgetUs: p.frameBudgetUs,
    maxFrameRateMilliHz: p.maxFrameRateMilliHz, batteryPermille: p.batteryPermille,
    batteryCharging: p.batteryCharging, visibility: p.visibility,
    dmaLaneCount: p.dmaLaneCount, vendorId: p.vendorId, deviceId: p.deviceId,
    crc32: new DataView(bytes.buffer).getUint32(CRC_OFFSET, true) >>> 0,
    error: null,
  };
}

// torn = budget bytes with payload corruption at offset 42 (inside u64 clock).
// CRC must mismatch -> E_CRC_MISMATCH (code 4), never a silent garbage decode.
{
  const bytes = buildRecord(PROFILES[2]);
  bytes[42] = (bytes[42] ^ 0x40) & 0xFF;
  const dv = new DataView(bytes.buffer);
  writeFileSync(join(OUT, 'hw-profile-torn.bin'), bytes);
  expected.torn = {
    file: 'hw-profile-torn.bin', error: 'E_CRC_MISMATCH', errorCode: 4,
    corruptedByteOffset: 42, crc32: dv.getUint32(CRC_OFFSET, true) >>> 0,
  };
}

writeFileSync(join(OUT, 'expected_profile.json'), JSON.stringify(expected, null, 2) + '\n');

// ---------------------------------------------------------------------------
// governor_vector.json — frozen cadence-governor timeline (all languages MUST
// produce the identical cap/rung/tierStage sequence; §4 of the wire spec).
// Ticks 0..129: nominal -> severe -> moderate -> recovery -> battery ->
// background -> visible again.
// ---------------------------------------------------------------------------
const NOM = { thermal: 0, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0 };
const ticks = [];
const push = (n, patch) => { for (let i = 0; i < n; i++) ticks.push({ ...NOM, ...patch }); };
push(10, {});                                        // 0–9   nominal -> 240
push(10, { thermal: 3 });                            // 10–19 severe: step at 19 -> 120
push(5, { thermal: 3 });                             // 20–24 severe streak resets at step
push(20, { thermal: 2 });                            // 25–44 moderate: 2x window -> step at 44 -> 60
push(55, {});                                        // 45–99 cool: recovery at 94 -> 120
push(5, { batteryPermille: 120, batteryCharging: 0 }); // 100–104 low battery -> cap 60
push(5, {});                                         // 105–109 battery ok -> 120
push(10, { visibility: 1 });                         // 110–119 hidden -> 30
push(10, {});                                        // 120–129 visible -> 120

// Frozen expected outputs: rung index into CADENCE_LADDER [240,120,60,30] plus
// tierStage. Reference semantics implemented here FIRST, then mirrored verbatim
// in TS / Python / Dart / Swift (audits cross-check the tables, tests cross-
// check the outputs).
const SUSTAINED = 10, RECOVERY = 50, BACKGROUND = 30, LOW_BATT = 150;
const LADDER = [240, 120, 60, 30];
let rung = 0, tierStage = 0, severeStreak = 0, moderateStreak = 0, coolStreak = 0;
const outTicks = [];
for (const t of ticks) {
  const visible = t.visibility === 0;
  if (!visible) {
    severeStreak = 0; moderateStreak = 0; coolStreak = 0;
  } else {
    if (t.thermal >= 3) { severeStreak++; moderateStreak = 0; coolStreak = 0; }
    else if (t.thermal === 2) { moderateStreak++; severeStreak = 0; coolStreak = 0; }
    else if (t.thermal <= 1) { coolStreak++; severeStreak = 0; moderateStreak = 0; }
    else { severeStreak = 0; moderateStreak = 0; coolStreak = 0; } // thermal=1 handled above; light is cool
    if (severeStreak >= SUSTAINED) { if (rung < LADDER.length - 1) rung++; severeStreak = 0; }
    if (moderateStreak >= 2 * SUSTAINED) { if (rung < LADDER.length - 1) rung++; moderateStreak = 0; }
    if (coolStreak >= RECOVERY) { if (rung > 0) rung--; coolStreak = 0; }
  }
  let cap = LADDER[rung];
  if (!visible) cap = BACKGROUND;                       // rule 1 (first match wins)
  else if (t.batteryPermille <= LOW_BATT && t.batteryCharging === 0) cap = Math.min(cap, 60);
  outTicks.push({ cap, rung, tierStage });
}
const stepAt = (cap) => { const a = [outTicks[0].cap]; for (let i = 1; i < outTicks.length; i++) if (outTicks[i].cap !== outTicks[i - 1].cap) a.push(outTicks[i].cap); return a; };

const governorVector = {
  constants: { CADENCE_LADDER: LADDER, SUSTAINED_TICKS: SUSTAINED, RECOVERY_TICKS: RECOVERY,
    BACKGROUND_CAP: BACKGROUND, LOW_BATTERY_PERMILLE: LOW_BATT, MAX_TIER_STAGES: 2 },
  profileMaxHz: 240, siliconTier: 1,
  ticks,
  expected: outTicks,
  expectedCapTransitions: stepAt(),
  tickCount: ticks.length,
};
writeFileSync(join(OUT, 'governor_vector.json'), JSON.stringify(governorVector, null, 1) + '\n');

// tier_vector.json — heap-pressure down-tier staging (rule 5) frozen separately.
const tierTicks = [{ heapPressure: 1 }, { heapPressure: 1 }, { heapPressure: 1 }, { heapPressure: 0 }, { heapPressure: 0 }];
let ts = 0;
const tierExpected = tierTicks.map((t) => {
  if (t.heapPressure && ts < 2) ts++;
  return { tierStage: ts, effectiveTier: Math.min(1 + ts, 3), budgetBytes: [8589934592, 4294967296, 2147483648][ts] };
});
writeFileSync(join(OUT, 'tier_vector.json'), JSON.stringify({
  siliconTier: 1, tierMaxHz: { '1': 240, '2': 120, '3': 60 },
  ticks: tierTicks, expected: tierExpected,
}, null, 1) + '\n');

console.log(`SHP1 fixtures -> ${OUT} (${PROFILES.length + 1} binaries, 3 manifests)`);
