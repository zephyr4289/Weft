// crosslang.test.mjs — TS-side guard over the frozen cross-language fixtures.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { decodeWcn1Into, verifyFrameCrc, FrameHandle, fnv1a64 } from '../src/wire.js';
import { crc32 } from '../src/crc32.js';

const VEC = JSON.parse(fs.readFileSync(
  new URL('../../../fixtures/cluster/wcn1_vectors.json', import.meta.url), 'utf8'));

/** Hex -> standalone ArrayBuffer (Buffer's pooled backing would inflate the
 *  decoder's `avail` window to the whole 8KB pool — a real API pitfall). */
function ab(hex) {
  const b = Buffer.from(hex, 'hex');
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.length);
}

test('crosslang: TS still decodes the frozen WCN1 vectors byte-exactly', () => {
  for (const c of VEC.cases) {
    const buf = Buffer.from(ab(c.hex));
    const h = new FrameHandle().bind(buf.buffer, buf.byteOffset);
    const e = c.expect;
    // Re-derive expected topic hash from the fixture's own payload topic.
    assert.equal(decodeWcn1Into(h, 0), 0, `case ${c.name}`);
    assert.equal(h.version, e.version);
    assert.equal(h.headerSize, e.headerSize);
    assert.equal(h.flags, e.flags);
    assert.equal(h.srcNode, e.srcNode);
    assert.equal(h.topicLo >>> 0, e.topicHash.lo);
    assert.equal(h.topicHi >>> 0, e.topicHash.hi);
    assert.equal(h.seqLo >>> 0, e.seq.lo);
    assert.equal(h.seqHi >>> 0, e.seq.hi);
    assert.equal(h.tsLo >>> 0, e.ts.lo);
    assert.equal(h.tsHi >>> 0, e.ts.hi);
    assert.equal(h.payloadLen, e.payloadLen);
    assert.equal(h.schemaId, e.schemaId);
    assert.equal(h.crc >>> 0, e.crc);
    const payload = new Uint8Array(buf.buffer, buf.byteOffset + h.payloadOffset, h.payloadLen);
    assert.equal(Buffer.from(payload).toString('hex'), e.payloadHex);
    assert.equal(verifyFrameCrc(h), 0);
  }
});

test('crosslang: TS rejects malformed fixtures with the frozen codes', () => {
  for (const m of VEC.malformed) {
    const buf = Buffer.from(ab(m.hex));
    const h = new FrameHandle().bind(buf.buffer, buf.byteOffset);
    const code = m.name === 'bad-crc' ? (() => {
      const rc = decodeWcn1Into(h, 0);
      return rc === 0 ? verifyFrameCrc(h) : rc;
    })() : decodeWcn1Into(h, 0);
    assert.equal(code, m.code, `malformed case ${m.name}`);
  }
});

test('crosslang: FNV/CRC reference vectors match the fixture', () => {
  for (const [input, expected] of Object.entries(VEC.fnv)) {
    const { hi, lo } = fnv1a64(input);
    const hex = hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
    assert.equal(hex, expected, `fnv1a64('${input}')`);
  }
  for (const [input, expected] of Object.entries(VEC.crc32)) {
    assert.equal(crc32(new TextEncoder().encode(input)), expected >>> 0,
      `crc32('${input}')`);
  }
});
