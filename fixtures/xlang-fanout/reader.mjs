// reader.mjs — TS-side ring consumer for the cross-language interop gate.
//
// Attaches the SHIPPED @weft/core WeftFanoutReader to ring bytes produced by
// the C runner (dump-ring): bytes -> SharedArrayBuffer -> reader (the same
// attach path a worker thread uses). Validates the handoff accounting
// (fresh final frame, dropped = frames - 1) and every payload word's BIT
// PATTERN against the shared generator (f32 values are compared through
// their u32 bits — NaN payloads never compare equal by value, so bits are
// the only honest comparator).

import { readFileSync } from 'node:fs';
import { WeftFanoutReader, mix32 } from '../../packages/core/dist/index.js';

const ringPath = process.argv[2] ?? 'ring-c.bin';
const metaPath = process.argv[3] ?? 'ring-c.meta';

const meta = readFileSync(metaPath, 'utf8').trim().split(/\s+/).map(Number);
if (meta.length !== 3 || meta.some((n) => !Number.isFinite(n))) {
  console.error(`reader: bad meta file: ${metaPath}`);
  process.exit(2);
}
const [payloadBytes, slotCount, frames] = meta;
if (payloadBytes % 4 !== 0) {
  console.error('reader: payloadBytes % 4 != 0');
  process.exit(2);
}
const payloadFloats = payloadBytes / 4;

const fileBytes = readFileSync(ringPath);
const expectBytes = 16 + 8 * slotCount + slotCount * payloadBytes;
if (fileBytes.byteLength !== expectBytes) {
  console.error(`reader: geometry mismatch — expected ${expectBytes} bytes, got ${fileBytes.byteLength}`);
  process.exit(2);
}

// Bytes -> SAB: the reader attaches to a SharedArrayBuffer, exactly as it
// would in a worker thread receiving a posted SAB.
const sab = new SharedArrayBuffer(expectBytes);
new Uint8Array(sab).set(fileBytes);
const r = new WeftFanoutReader(sab, payloadFloats, slotCount);

const c = r.claim();
let failures = 0;
if (!c.fresh || c.seq !== frames || c.dropped !== frames - 1) {
  console.error(`reader: FAIL accounting — fresh=${c.fresh} seq=${c.seq} dropped=${c.dropped}`);
  failures++;
}

const tword = (seq, w) => mix32((Math.imul(seq, 2654435761) + w) >>> 0);
const cvt = new ArrayBuffer(4);
const f32 = new Float32Array(cvt);
const u32 = new Uint32Array(cvt);
const view = r.view();
let wordOk = true;
for (let w = 0; w < payloadFloats; w++) {
  f32[0] = view[w]; // f32 -> u32 bits through the shared conversion buffer
  if (u32[0] !== tword(frames, w)) {
    console.error(`reader: FAIL word ${w} torn across ports (bits ${u32[0]} != ${tword(frames, w)})`);
    wordOk = false;
    failures++;
    break;
  }
}

console.log(
  `reader: fresh seq=${c.seq} dropped=${c.dropped} words=${payloadFloats} ` +
    `bits=${wordOk ? 'OK' : 'TORN'} verdict=${failures === 0 ? 'PASS' : 'FAIL'}`
);
process.exit(failures === 0 ? 0 : 1);
