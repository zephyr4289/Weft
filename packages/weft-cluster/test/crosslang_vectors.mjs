// crosslang_vectors.mjs — generate frozen cross-language fixtures:
//   fixtures/cluster/wcn1_vectors.json
// TS encodes reference datagrams + malformed cases; Python must decode the
// same bytes to the same fields / error codes (and vice versa via round-trip).
import fs from 'node:fs';
import { crc32 } from '../src/crc32.js';
import { fnv1a64, encodeWcn1Into, WC_FLAG_INLINE_PAYLOAD, WC_FLAG_CRC_PRESENT } from '../src/wire.js';

const enc = new TextEncoder();

const vectors = { algorithm: 'WCN1-v1', spec: 'docs/weft-cluster/WIRE-V1.md', cases: [] };

// 1. full-field datagram
{
  const topic = 'telemetry';
  const th = fnv1a64(topic);
  const payload = enc.encode('hello-weft-cluster');
  const buf = new ArrayBuffer(64 + payload.length);
  const dv = new DataView(buf);
  encodeWcn1Into(dv, 0, {
    flags: WC_FLAG_INLINE_PAYLOAD | WC_FLAG_CRC_PRESENT,
    srcNode: 42, topicLo: th.lo, topicHi: th.hi,
    seqLo: 0xcafebabe, seqHi: 0x00000001,
    tsLo: 0x11223344, tsHi: 0x00ffeedd,
    payloadLen: payload.length, schemaId: 7, crc: crc32(payload), rdmaKey: 0,
  });
  new Uint8Array(buf, 64).set(payload);
  vectors.cases.push({
    name: 'full-fields',
    hex: Buffer.from(buf).toString('hex'),
    expect: {
      version: 1, headerSize: 64, flags: 5, srcNode: 42,
      topicHash: { lo: th.lo >>> 0, hi: th.hi >>> 0 },
      seq: { lo: 0xcafebabe, hi: 1 }, ts: { lo: 0x11223344, hi: 0x00ffeedd },
      payloadLen: payload.length, schemaId: 7, crc: crc32(payload),
      payloadHex: Buffer.from(payload).toString('hex'),
    },
  });
}

// 2. FNV / CRC reference vectors
vectors.fnv = { 'a': 'af63dc4c8601ec8c', '': 'cbf29ce484222325',
  'telemetry': (() => { const { hi, lo } = fnv1a64('telemetry');
    return hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0'); })() };
vectors.crc32 = { '123456789': crc32(enc.encode('123456789')),
  'weft-cluster': crc32(enc.encode('weft-cluster')) };

// 3. malformed cases -> expected stable error code (Law 4 parity)
{
  const cases = [];
  // bad magic
  const b1 = new ArrayBuffer(70);
  new DataView(b1).setUint32(0, 0xdeadbeef, true);
  cases.push({ name: 'bad-magic', hex: Buffer.from(b1).toString('hex'), code: 1 });
  // truncated (3 bytes)
  cases.push({ name: 'truncated', hex: '5700', code: 3 });
  // future version
  const b3 = new ArrayBuffer(70);
  const d3 = new DataView(b3);
  d3.setUint32(0, 0x314e4357, true); d3.setUint16(4, 2, true);
  cases.push({ name: 'future-version', hex: Buffer.from(b3).toString('hex'), code: 2 });
  // header_size < 64
  const b4 = new ArrayBuffer(70);
  const d4 = new DataView(b4);
  d4.setUint32(0, 0x314e4357, true); d4.setUint16(6, 32, true);
  cases.push({ name: 'small-header', hex: Buffer.from(b4).toString('hex'), code: 4 });
  // inline payload overrun
  const b5 = new ArrayBuffer(70);
  const d5 = new DataView(b5);
  d5.setUint32(0, 0x314e4357, true); d5.setUint16(6, 64, true);
  d5.setUint32(8, WC_FLAG_INLINE_PAYLOAD, true); d5.setUint32(40, 99, true);
  cases.push({ name: 'payload-overrun', hex: Buffer.from(b5).toString('hex'), code: 3 });
  // CRC mismatch
  const b6 = new ArrayBuffer(68);
  const d6 = new DataView(b6);
  encodeWcn1Into(d6, 0, { flags: WC_FLAG_INLINE_PAYLOAD | WC_FLAG_CRC_PRESENT,
    srcNode: 1, topicLo: 0, topicHi: 0, seqLo: 0, seqHi: 0, tsLo: 0, tsHi: 0,
    payloadLen: 4, schemaId: 0, crc: 0x12345678, rdmaKey: 0 });
  cases.push({ name: 'bad-crc', hex: Buffer.from(b6).toString('hex'), code: 5 });
  vectors.malformed = cases;
}

const out = new URL('../../../fixtures/cluster/wcn1_vectors.json', import.meta.url);
fs.writeFileSync(out, JSON.stringify(vectors, null, 2) + '\n');
console.log(`wrote ${out.pathname} (${vectors.cases.length} case, ${vectors.malformed.length} malformed)`);
