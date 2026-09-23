// crosslang_consume.mjs — TS consumer: attach the ring at argv[2] (produced by
// EITHER language) and validate it byte-exactly. Fail-closed, CI stage gate.
import { readFileSync } from 'node:fs';
import { WeftTensorRing } from '../src/index.js';

const path = process.argv[2];
if (!path) { console.error('usage: node crosslang_consume.mjs <ring-path>'); process.exit(2); }

const bin = readFileSync(path);
const ring = WeftTensorRing.attach(bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength));
if (ring.producerSeq !== 10) throw new Error(`producer_seq ${ring.producerSeq} != 10`);
if (ring.layout.schemaId !== 0xFE77000000000001) throw new Error('schema id mismatch');
if (ring.layout.tickHz !== 120) throw new Error('tick_hz mismatch');
for (let seq = 7; seq <= 10; seq++) {
  const v = ring.acquireFrame(seq);
  if (v === null) throw new Error(`frame ${seq} missing`);
  if (v.timestampNs !== seq * 1_000_000) throw new Error(`frame ${seq} ts ${v.timestampNs}`);
  if (v.fourcc !== 'F32 ') throw new Error(`frame ${seq} fourcc ${v.fourcc}`);
  for (let i = 0; i < 6; i++) {
    const want = (seq * 10 + i) * 0.25;
    if (v.getF32(i) !== want) throw new Error(`frame ${seq} elem ${i}: ${v.getF32(i)} != ${want}`);
  }
}
console.log(JSON.stringify({ consumer: 'typescript', path, framesValidated: [7, 8, 9, 10], ok: true }));
