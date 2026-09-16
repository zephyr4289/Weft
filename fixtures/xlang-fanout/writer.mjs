// writer.mjs — TS-side ring producer for the cross-language interop gate.
//
// Publishes `frames` deterministic pattern frames through the SHIPPED
// @weft/core broadcaster (the protocol ops — begin/publish — run in the
// production class), filling the slot bytes through a private Uint32Array
// view over the same SharedArrayBuffer memory. The pattern words are the
// same generator the C side validates against (04-LITMUS §0.1 mixer):
//
//     word(seq, w) = mix32(seq * 2654435761 + w)        [u32 arithmetic]
//
// The ring bytes + a one-line meta file are written for the C consumer
// (core/c/fanout-runner validate-ring).

import { writeFileSync } from 'node:fs';
import { WeftFanoutBroadcaster, mix32 } from '../../packages/core/dist/index.js';

const payloadFloats = 64; // 256-byte payloads — must stay % 4 == 0 in bytes
const slotCount = 4;
const frames = Number(process.argv[4] ?? 5000);

const ringPath = process.argv[2] ?? 'ring-ts.bin';
const metaPath = process.argv[3] ?? 'ring-ts.meta';

const tword = (seq, w) => mix32((Math.imul(seq, 2654435761) + w) >>> 0);

const b = new WeftFanoutBroadcaster(payloadFloats, slotCount);
const payloadBase = 16 + 8 * slotCount;
const u32 = new Uint32Array(b.sab, payloadBase, slotCount * payloadFloats);

for (let seq = 1; seq <= frames; seq++) {
  b.begin(); // protocol op in the shipped class (invalidate-before-fill)
  const off = ((seq - 1) % slotCount) * payloadFloats;
  for (let w = 0; w < payloadFloats; w++) u32[off + w] = tword(seq, w);
  b.publish();
}

const d = b.debugStats();
if (Number(d.latestSeq) !== frames || Number(d.publishes) !== frames) {
  console.error(`writer: FAILED self-check latest=${d.latestSeq} publishes=${d.publishes}`);
  process.exit(1);
}

writeFileSync(ringPath, new Uint8Array(b.sab));
writeFileSync(metaPath, `${payloadFloats * 4} ${slotCount} ${frames}\n`);
console.log(
  `writer: frames=${frames} slots=${slotCount} floats=${payloadFloats} ` +
    `bytes=${b.sab.byteLength} latest=${d.latestSeq} publishes=${d.publishes}`
);
