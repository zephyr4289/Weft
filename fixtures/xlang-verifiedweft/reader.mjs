// reader.mjs — TS-side VerifiedWeft consumer (xlang gate, direction 1 + 3).
//
// Reads a C-produced (or tampered) record file, derives the auth key from
// the secret, verifies EVERY record (tag + envelope geometry + payload
// bit-exactness against the shared 04-LITMUS §0.1 mix32 generator), and
// exits non-zero on any mismatch.
import { readFileSync } from 'node:fs';
import { verifiedWeftRecordDecodeVerify, deriveKey, ctEq } from '../../core/ts/verified.ts';

const mix32 = (x) => {
  x = x >>> 0;
  x = (Math.imul(x ^ (x >>> 16), 0x7feb352d)) >>> 0;
  x = (Math.imul(x ^ (x >>> 15), 0x846ca68b)) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x;
};

const [file, secret] = process.argv.slice(2);
if (!file || !secret) {
  console.error('usage: node reader.mjs <file.bin> <secret>');
  process.exit(2);
}

const buf = readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
const magic = dv.getBigUint64(0, true);
const frames = Number(dv.getBigUint64(8, true));
const plen = Number(dv.getBigUint64(16, true));
if (magic !== 0x57565731n) {
  console.error(`bad magic: ${magic.toString(16)}`);
  process.exit(1);
}
const recLen = 16 + plen + 32;
const key = deriveKey(new TextEncoder().encode(secret));

let verified = 0;
for (let i = 0; i < frames; i++) {
  const off = 24 + i * recLen;
  const rec = buf.subarray(off, off + recLen);
  const r = verifiedWeftRecordDecodeVerify(key, rec);
  if (r.code !== 0) {
    console.error(`record ${i}: verify FAILED (code ${r.code})`);
    process.exit(1);
  }
  // Envelope seq must equal the record index.
  const seq = dv.getUint32(off + 8, true);
  if (seq !== i) {
    console.error(`record ${i}: seq mismatch (${seq})`);
    process.exit(1);
  }
  // Payload words must match the mix32 generator bit-exactly.
  for (let j = 0; j < plen; j += 4) {
    const w = mix32((i * 0x9e3779b9 + j) >>> 0);
    const take = Math.min(4, plen - j);
    const got = new DataView(r.payload.buffer, r.payload.byteOffset + j, take).getUint32(0, true) & (take === 4 ? 0xffffffff : (1 << (8 * take)) - 1);
    if ((w & (take === 4 ? 0xffffffff : (1 << (8 * take)) - 1)) !== got) {
      console.error(`record ${i}: payload word ${j} mismatch`);
      process.exit(1);
    }
  }
  verified++;
}

if (!ctEq(new Uint8Array(key), new Uint8Array(key))) process.exit(1); // sanity
console.log(`reader: ${verified}/${frames} records verified (tag + payload bit-exact)`);
process.exit(verified === frames ? 0 : 1);
