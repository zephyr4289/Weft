/**
 * Stage 5 — Time-Travel determinism & crash-bundle integrity.
 * - scrub to 1,000 pseudo-random record indices across two independent
 *   passes: per-index state hashes must be identical (deterministic replay)
 * - checkpoint-jump state must equal full-replay state
 * - SREC1 stream round-trip: serialize → parse → serialize = byte-identical
 * - SBURST crash bundle: trailer CRC verifies; corrupted header fails closed
 */

import { FlightRecorder, TimeTravelReplayer, SBURST_TRAILER_SIZE } from '../../../packages/studio/src/engine/flightrec.ts';
import { crc32 } from '../../../packages/studio/src/engine/types.ts';
import { writeFileSync } from 'node:fs';

const results = { stage: 5, checks: [], ok: false };
let failures = 0;
function check(name, ok, detail = '') {
  results.checks.push({ name, ok, detail: String(detail).slice(0, 120) });
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

// deterministic pseudo-random stream of mutation events
let seed = 0x5eed7a1e >>> 0;
const rnd = () => {
  seed = (Math.imul(seed ^ (seed >>> 15), 1 | seed) + 0x9e3779b9) >>> 0;
  return seed / 4294967296;
};

const N = 50_000;
const rec = new FlightRecorder('stage5hash', 0);
for (let i = 0; i < N; i++) {
  const addr = Math.floor(rnd() * 1_048_576) * 32;
  const beforeLo = Math.floor(rnd() * 4294967296) >>> 0;
  const beforeHi = Math.floor(rnd() * 4294967296) >>> 0;
  const afterLo = Math.floor(rnd() * 4294967296) >>> 0;
  const afterHi = Math.floor(rnd() * 4294967296) >>> 0;
  rec.record(i * 1_000_000, addr, i * 2, i * 2 + 2, beforeLo, beforeHi, afterLo, afterHi);
}
check(`${N} records captured`, rec.recordCount === N, `${rec.recordCount}`);

const rp = new TimeTravelReplayer(rec);
const s1 = new Uint32Array(16);
const s2 = new Uint32Array(16);

// two independent scrub passes over 1,000 pseudo-random indices
seed = 0x1337c0de >>> 0;
const idxs: number[] = [];
for (let i = 0; i < 1000; i++) idxs.push(Math.floor(rnd() * (N + 1)));
const pass1: string[] = idxs.map((i) => { rp.scrubTo(i, s1); return TimeTravelReplayer.stateHash(s1); });
const pass2: string[] = idxs.map((i) => { rp.scrubTo(i, s2); return TimeTravelReplayer.stateHash(s2); });
const mismatch = pass1.filter((h, i) => h !== pass2[i]).length;
check('scrub determinism across two passes (1,000 indices)', mismatch === 0, `${mismatch} mismatches`);
check('state is non-trivial (distinct hashes)', new Set(pass1).size > 500,
  `${new Set(pass1).size} distinct states over 1,000 indices`);

// checkpoint-jump == full replay from zero
const i = 4_500; // inside checkpoint 1's fold range
rp.scrubTo(i, s1);
const full = new Uint32Array(16);
for (let k = 0; k < i; k++) {
  const lane = (rec.addrAt(k) % 8) * 2;
  full[lane] ^= rec.beforeLoAt(k); full[lane + 1] ^= rec.beforeHiAt(k);
  full[lane] ^= rec.afterLoAt(k); full[lane + 1] ^= rec.afterHiAt(k);
}
check('checkpoint jump equals full replay', TimeTravelReplayer.stateHash(s1) === TimeTravelReplayer.stateHash(full));

// SREC1 round-trip
const streamLen = FlightRecorder.serializedSize(N);
const bytes = new Uint8Array(streamLen);
const n = rec.toBytes(bytes);
check('serialize fills the stream buffer', n === streamLen, `${n} vs ${streamLen}`);
const parsed = FlightRecorder.fromBytes(bytes);
check('parse succeeds', parsed.error === null && parsed.rec.recordCount === N,
  parsed.error ?? `${parsed.rec?.recordCount}`);
const bytes2 = new Uint8Array(streamLen);
parsed.rec.toBytes(bytes2);
let identical = bytes.length === bytes2.length;
for (let i = 0; identical && i < bytes.length; i++) identical = bytes[i] === bytes2[i];
check('serialize(parse(serialize)) byte-identical', identical);

// SBURST crash bundle
const bundle = new Uint8Array(streamLen + SBURST_TRAILER_SIZE);
const bn = rec.toCrashBundle(bundle);
check('crash bundle serialized', bn === bundle.length, `${bn} bytes`);
const dv = new DataView(bundle.buffer);
const trailerCrc = dv.getUint32(streamLen + 28, true);
check('bundle trailer CRC verifies', trailerCrc === crc32(bundle, 0, streamLen + 28));

// corrupted header must fail closed
const corrupt = bytes.slice();
corrupt[17] ^= 0xff; // flip a header byte inside CRC coverage
const bad = FlightRecorder.fromBytes(corrupt);
check('corrupted stream rejected (fail-closed)', bad.error !== null, bad.error ?? 'no error!?');

results.metrics = { records: N, distinct_states: new Set(pass1).size, bundle_bytes: bundle.length };
results.ok = failures === 0;
writeFileSync(new URL('../../../evidence/pillar7/stage-5-timetravel.json', import.meta.url), JSON.stringify(results, null, 2));
console.log(results.ok ? 'STAGE 5: PASS' : `STAGE 5: FAIL (${failures})`);
process.exit(results.ok ? 0 : 2);
