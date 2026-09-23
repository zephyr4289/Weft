// crosslang_produce.mjs — TS producer: write a deterministic WTR1 ring to
// argv[2] (10 frames, f32 [2,3], payload (seq*10+i)*0.25, ts=seq*1e6).
// Consumed by python/tests/crosslang_consume.py in CI shard stage 4.
import { writeFileSync } from 'node:fs';
import { WeftTensorRing, DLPackCode } from '../src/index.js';

const out = process.argv[2];
if (!out) { console.error('usage: node crosslang_produce.mjs <out-path>'); process.exit(2); }

const ring = WeftTensorRing.create({
  slotCount: 4, payloadCap: 24, dtype: { code: DLPackCode.FLOAT, bits: 32 },
  shape: [2, 3], schemaId: 0xFE77000000000001n, tickHz: 120, fourcc: 'F32 ',
});
const src = new Float32Array(6);
for (let seq = 1; seq <= 10; seq++) {
  for (let i = 0; i < 6; i++) src[i] = (seq * 10 + i) * 0.25;
  ring.commit(src, { tsLo: (seq * 1_000_000) % 4294967296, tsHi: Math.floor((seq * 1_000_000) / 4294967296) });
}
writeFileSync(out, Buffer.from(ring.buffer));
console.log(JSON.stringify({
  producer: 'typescript', path: out, byteLength: ring.byteLength,
  producerSeq: ring.producerSeq, slotStride: ring.slotStride,
  schemaId: ring.layout.schemaId.toString(16),
}));
