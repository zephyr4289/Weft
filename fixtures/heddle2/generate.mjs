// fixtures/heddle2/generate.mjs — HPL1 golden fixture generator.
//
// Wire-deterministic: seeded LCG, dyadic constants only, fixed arithmetic order.
// Double-run in-process must be byte-identical (CI heddle2 shard stage 2 re-checks
// this by regenerating to a temp dir and diffing sha256 against committed files).
//
// Layout math here mirrors packages/heddle-core/src/layout.js — the fixture test
// parses these .bin files THROUGH heddle-core, so any drift between the two is a
// test failure by construction.
//
// Usage: node fixtures/heddle2/generate.mjs [--out fixtures/heddle2]
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const HERE = dirname(fileURLToPath(import.meta.url));

// ---- layout constants (HPL1 §1) — MUST equal heddle-core layout.js ----
const HEADER_SIZE = 128;
const LANE_CTRL_STRIDE = 64;
const MAGIC_U32 = 0x314c5048; // 'H','P','L','1' little-endian
const VERSION = 1;
const FLAG_LE_REQUIRED = 1;
const FLAG_ACTIVE = 1; // laneFlags bit0
const align8 = (x) => (x + 7) & ~7;

export function deriveGeometry(laneCount, samplesPerLane) {
  const dirtyWords = Math.ceil(laneCount / 32);
  const laneCtrlBase = HEADER_SIZE + align8(4 * dirtyWords);
  const ringBase = align8(laneCtrlBase + LANE_CTRL_STRIDE * laneCount);
  const totalBytes = ringBase + laneCount * samplesPerLane * 8;
  return { dirtyWords, laneCtrlBase, ringBase, totalBytes };
}

// ---- deterministic series: LCG, values are integers / 64 (dyadic f64) ----
function lcg(seed) {
  let s = seed >>> 0;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s;
  };
}

function buildPlane(laneCount, samplesPerLane, seedBase, tickHz, epoch) {
  const geo = deriveGeometry(laneCount, samplesPerLane);
  const buf = new ArrayBuffer(geo.totalBytes);
  const dv = new DataView(buf);
  const u32 = new Uint32Array(buf);
  const f64Ring = new Float64Array(buf);

  const wrU32 = (o, v) => dv.setUint32(o, v >>> 0, true);
  const wrU64 = (o, v) => { // lo/hi split; caller stores lo LAST where ordering matters
    dv.setUint32(o + 4, Math.floor(v / 0x100000000) >>> 0, true);
    dv.setUint32(o, v >>> 0, true);
  };
  const wrF64 = (o, v) => dv.setFloat64(o, v, true);

  // ---- header (header seqlock left EVEN/stable at rest) ----
  wrU32(0x00, MAGIC_U32);
  wrU32(0x04, VERSION);
  wrU32(0x08, FLAG_LE_REQUIRED); // bit0 set; bit1 EPOCH_STABLE clear (fixture is at-rest)
  wrU32(0x0c, laneCount);
  wrU32(0x10, samplesPerLane);
  wrU32(0x14, samplesPerLane - 1);
  wrU32(0x18, tickHz);
  wrU32(0x1c, 0);
  wrU64(0x20, epoch);
  const totalPublishes = laneCount * samplesPerLane;
  wrU64(0x28, totalPublishes * 2); // even: header stable
  wrU64(0x30, 1_000_000 + totalPublishes * 7);
  wrU64(0x38, 0); // framesDropped
  // globals filled from lane stats below (after per-lane loops), under header lock
  wrU32(0x60, geo.dirtyWords);
  wrU32(0x64, LANE_CTRL_STRIDE);
  wrU32(0x68, geo.ringBase);
  wrU32(0x6c, geo.totalBytes);
  // 0x70..0x7f reserved zero (ArrayBuffer is zero-initialized)

  // ---- per-lane: deterministic series, publish one sample at a time ----
  const lanes = [];
  let gmin = Infinity, gmax = -Infinity, gsum = 0, gcount = 0;
  for (let lane = 0; lane < laneCount; lane++) {
    const rnd = lcg(seedBase + lane * 0x9e3779b9);
    const base = 32 + lane * 16; // lane base level (dyadic)
    const ctrl = geo.laneCtrlBase + lane * LANE_CTRL_STRIDE;
    const ring = geo.ringBase + lane * samplesPerLane * 8;
    let min = Infinity, max = -Infinity, sum = 0;
    let seq = 0;
    const values = new Array(samplesPerLane);
    for (let k = 0; k < samplesPerLane; k++) {
      const v = (base + (rnd() % 4096)) / 64; // dyadic
      values[k] = v;
      // producer publish: seq odd, write, seq even (in a single buffer, single thread)
      seq += 1;
      wrU32(ctrl + 0x00, seq >>> 0);           // lo (hi stays 0)
      wrF64(ctrl + 0x08, v);                   // current
      if (v < min) min = v; if (v > max) max = v;
      sum += v;
      wrF64(ctrl + 0x10, min);
      wrF64(ctrl + 0x18, max);
      wrF64(ctrl + 0x20, sum / (k + 1));
      wrU32(ctrl + 0x28, (k + 1) >>> 0);       // samplesSeen lo (hi 0)
      wrU32(ctrl + 0x30, (k + 1) & (samplesPerLane - 1)); // head (post-increment)
      wrU32(ctrl + 0x34, FLAG_ACTIVE);
      wrU64(ctrl + 0x38, 1_000_000 + lane * 1000 + k * 7);
      wrU32(ctrl + 0x40, 0); // laneDrops
      f64Ring[(ring >> 3) + (k & (samplesPerLane - 1))] = v; // ring slot
      seq += 1;
      wrU32(ctrl + 0x00, seq >>> 0);           // even: stable
      // global window bookkeeping
      if (v < gmin) gmin = v; if (v > gmax) gmax = v;
      gsum += v; gcount += 1;
      // header seqlock: odd → write globals → even
      wrU32(0x28, (totalPublishes * 2 + k * 2 + 1) >>> 0);
      wrF64(0x40, gmin); wrF64(0x48, gmax); wrF64(0x50, gsum / gcount);
      wrF64(0x58, v); // headline = most recent published value anywhere
      wrU32(0x28, (totalPublishes * 2 + k * 2 + 2) >>> 0);
    }
    lanes.push({ values, min, max, avg: sum / samplesPerLane });
    // dirty mask: all lanes touched → bit set (producer-set, held at rest)
    u32[(HEADER_SIZE >> 2) + (lane >> 5)] |= (1 << (lane & 31)) >>> 0;
  }
  return { buf, geo, lanes, globals: { min: gmin, max: gmax, avg: gsum / gcount } };
}

function sha256(buf) { return createHash('sha256').update(Buffer.from(buf)).digest('hex'); }

function manifestOf(name, plane, tickHz, epoch) {
  const dv = new DataView(plane.buf);
  const rdU32 = (o) => dv.getUint32(o, true);
  const rdF64 = (o) => dv.getFloat64(o, true);
  return {
    name,
    sha256: sha256(plane.buf),
    totalBytes: plane.buf.byteLength,
    geometry: plane.geo,
    header: {
      magicU32: rdU32(0x00), version: rdU32(0x04), flags: rdU32(0x08),
      laneCount: rdU32(0x0c), samplesPerLane: rdU32(0x10), ringMask: rdU32(0x14),
      tickHz, epoch,
      publishSeq: rdU32(0x28),
      lastPublishNs: rdU32(0x30),
      globalMin: rdF64(0x40), globalMax: rdF64(0x48),
      globalAvg: rdF64(0x50), globalCurrent: rdF64(0x58),
      dirtyWords: rdU32(0x60), laneCtrlStride: rdU32(0x64),
      ringBaseOffset: rdU32(0x68), totalBytesField: rdU32(0x6c),
    },
    lanes: plane.lanes.map((l, i) => {
      const ctrl = plane.geo.laneCtrlBase + i * 64;
      return {
        current: rdF64(ctrl + 0x08), min: rdF64(ctrl + 0x10),
        max: rdF64(ctrl + 0x18), avg: rdF64(ctrl + 0x20),
        samplesSeen: rdU32(ctrl + 0x28), head: rdU32(ctrl + 0x30),
        laneFlags: rdU32(ctrl + 0x34), lanePublishNs: rdU32(ctrl + 0x38),
        laneDrops: rdU32(ctrl + 0x40),
        values: l.values,
      };
    }),
  };
}

const FIXTURES = [
  { file: 'hpl1-basic-4x8', lanes: 4, samples: 8, seed: 0x57ef7c1d, tickHz: 240, epoch: 1 },
  { file: 'hpl1-edge-1x2', lanes: 1, samples: 2, seed: 0x0ddba11, tickHz: 240, epoch: 1 },
];

function generateAll(outDir) {
  const manifests = [];
  for (const f of FIXTURES) {
    const plane = buildPlane(f.lanes, f.samples, f.seed, f.tickHz, f.epoch);
    // in-process double-run determinism: rebuild and byte-compare
    const again = buildPlane(f.lanes, f.samples, f.seed, f.tickHz, f.epoch);
    const a = Buffer.from(plane.buf), b = Buffer.from(again.buf);
    if (!a.equals(b)) {
      console.error(`DETERMINISM FAILURE: ${f.file} double-run differs`);
      process.exit(2);
    }
    writeFileSync(join(outDir, `${f.file}.bin`), a);
    const manifest = manifestOf(f.file, plane, f.tickHz, f.epoch);
    writeFileSync(join(outDir, `${f.file}.json`), JSON.stringify(manifest, null, 2) + '\n');
    manifests.push(manifest);
    console.log(`${f.file}.bin  ${plane.buf.byteLength}B  sha256=${manifest.sha256}`);
  }
  return manifests;
}

const isMain = process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1];
if (isMain) {
  const argOut = process.argv.includes('--out')
    ? process.argv[process.argv.indexOf('--out') + 1]
    : HERE;
  if (argOut !== HERE) mkdirSync(argOut, { recursive: true });
  generateAll(argOut);
}
export { buildPlane, generateAll, FIXTURES, sha256 };
