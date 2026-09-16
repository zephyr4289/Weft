// writer.mjs — TS-side VerifiedWeft producer (xlang gate, direction 2).
//
// Writes a C-consumable record file: VWV1 header, then `frames` records of
// [envelope v1 | payload | HMAC-SHA256 tag]. Payloads come from the shared
// 04-LITMUS §0.1 mix32 generator; tags are computed by the pure-TS kernel
// module (core/ts/verified.ts). The C consumer validates everything.
import { writeFileSync } from 'node:fs';
import {
  VerifiedWeftSigner,
  deriveKey,
  weftEnvelopeEncodeV1,
  VW_ENVELOPE_LEN,
  HMAC_TAG_LEN,
} from '../../core/ts/verified.ts';

const mix32 = (x) => {
  x = x >>> 0;
  x = (Math.imul(x ^ (x >>> 16), 0x7feb352d)) >>> 0;
  x = (Math.imul(x ^ (x >>> 15), 0x846ca68b)) >>> 0;
  x = (x ^ (x >>> 16)) >>> 0;
  return x;
};

const [out, framesArg, plenArg, secret] = process.argv.slice(2);
if (!out || !framesArg || !plenArg || !secret) {
  console.error('usage: node writer.mjs <out.bin> <frames> <payload_len> <secret>');
  process.exit(2);
}
const frames = Number(framesArg);
const plen = Number(plenArg);
const recLen = VW_ENVELOPE_LEN + plen + HMAC_TAG_LEN;

const key = deriveKey(new TextEncoder().encode(secret));
const signer = new VerifiedWeftSigner(key);

const header = Buffer.alloc(24);
header.writeBigUInt64LE(0x57565731n, 0); // "VWV1"
header.writeBigUInt64LE(BigInt(frames), 8);
header.writeBigUInt64LE(BigInt(plen), 16);

const body = Buffer.alloc(frames * recLen);
const env = new Uint8Array(VW_ENVELOPE_LEN);
const payload = new Uint8Array(plen);
const tag = new Uint8Array(HMAC_TAG_LEN);
const bodyU8 = new Uint8Array(body.buffer, body.byteOffset, body.byteLength);

for (let i = 0; i < frames; i++) {
  for (let j = 0; j < plen; j += 4) {
    const w = mix32((i * 0x9e3779b9 + j) >>> 0);
    if (j + 4 <= plen) {
      payload[j] = w & 0xff;
      payload[j + 1] = (w >>> 8) & 0xff;
      payload[j + 2] = (w >>> 16) & 0xff;
      payload[j + 3] = (w >>> 24) & 0xff;
    } else {
      for (let k = 0; k < plen - j; k++) payload[j + k] = (w >>> (8 * k)) & 0xff;
    }
  }
  weftEnvelopeEncodeV1(env, i, plen);
  signer.update(env, 0, VW_ENVELOPE_LEN);
  signer.update(payload);
  signer.finalize(tag);

  const off = i * recLen;
  bodyU8.set(env, off);
  bodyU8.set(payload, off + VW_ENVELOPE_LEN);
  bodyU8.set(tag, off + VW_ENVELOPE_LEN + plen);
}

writeFileSync(out, Buffer.concat([header, body]));
console.log(`writer: ${frames} authenticated records (${recLen} B each) -> ${out}`);
