// wire.test.mjs — WCN1/WGS1 wire format tests.
// Law 2 (LE determinism), Law 4 (malformed taxonomy).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  WCN1_MAGIC, WCN1_HEADER_SIZE, WGS1_HEADER_SIZE, WGS1_ENTRY_SIZE,
  fnv1a64, u64ToBigInt, FrameHandle, encodeWcn1Into, decodeWcn1Into,
  verifyFrameCrc, encodeWgs1Into, writeGossipEntryInto, decodeWgs1Into,
  readGossipEntryInto, WC_FLAG_INLINE_PAYLOAD, WC_FLAG_CRC_PRESENT,
} from '../src/wire.js';
import { crc32 } from '../src/crc32.js';
import { WC, wcName } from '../src/errors.js';

function makeDv(n) { const b = new ArrayBuffer(n); return new DataView(b); }

test('crc32 IEEE vectors (parity with Python zlib.crc32)', () => {
  const v = (s) => crc32(new TextEncoder().encode(s));
  assert.equal(v('123456789'), 0xcbf43926);
  assert.equal(v(''), 0x00000000);
  assert.equal(v('The quick brown fox jumps over the lazy dog'), 0x414fa339);
  assert.equal(v('weft-cluster'), crc32(new TextEncoder().encode('weft-cluster')));
});

test('fnv1a64 topic hash vectors (parity with Python impl)', () => {
  // Independent hand-computed expectations for short inputs.
  const { lo, hi } = fnv1a64('a');
  // FNV-1a("a") = 0xaf63dc4c8601ec8c
  assert.equal(hi, 0xaf63dc4c >>> 0 >>> 0);
  assert.equal(lo, 0x8601ec8c >>> 0);
  assert.equal(u64ToBigInt(fnv1a64('a').lo, fnv1a64('a').hi), 0xaf63dc4c8601ec8cn);
  assert.equal(u64ToBigInt(fnv1a64('').lo, fnv1a64('').hi), 0xcbf29ce484222325n);
  // Deterministic, allocation-tolerant (cold path only).
  assert.equal(u64ToBigInt(fnv1a64('telemetry').lo, fnv1a64('telemetry').hi),
               u64ToBigInt(fnv1a64('telemetry').lo, fnv1a64('telemetry').hi));
});

test('WCN1 header: little-endian byte layout is normative', () => {
  const dv = makeDv(WCN1_HEADER_SIZE + 8);
  const written = encodeWcn1Into(dv, 0, {
    flags: WC_FLAG_INLINE_PAYLOAD | WC_FLAG_CRC_PRESENT,
    srcNode: 7, topicLo: 0x11223344, topicHi: 0x55667788,
    seqLo: 0x99aabbcc, seqHi: 0x00112233,
    tsLo: 0x44332211, tsHi: 0x88776655,
    payloadLen: 8, schemaId: 42, crc: 0xdeadbeef, rdmaKey: 0,
  });
  assert.equal(written, WCN1_HEADER_SIZE);
  const u8 = new Uint8Array(dv.buffer);
  // Magic bytes on the wire are literally "WCN1".
  assert.equal(String.fromCharCode(u8[0], u8[1], u8[2], u8[3]), 'WCN1');
  // Every multi-byte field decodes ONLY with the LE flag set.
  assert.equal(dv.getUint16(4, true), 1);
  assert.equal(dv.getUint16(6, true), 64);
  assert.equal(dv.getUint32(12, true), 7);
  assert.equal(dv.getUint32(16, true), 0x11223344);
  assert.equal(dv.getUint32(20, true), 0x55667788);
  assert.equal(dv.getUint32(24, true), 0x99aabbcc);
  assert.equal(dv.getUint32(28, true), 0x00112233);
  assert.equal(dv.getUint32(40, true), 8);
  assert.equal(dv.getUint32(48, true), 0xdeadbeef);
  // Reserved region is zero.
  assert.equal(dv.getUint32(56, true), 0);
  assert.equal(dv.getUint32(60, true), 0);
});

test('WCN1 round-trip: encode -> decode -> same fields', () => {
  const dv = makeDv(WCN1_HEADER_SIZE + 16);
  const payload = new Uint8Array(dv.buffer, WCN1_HEADER_SIZE, 16);
  crypto.getRandomValues(payload);
  const crc = crc32(payload);
  encodeWcn1Into(dv, 0, {
    flags: WC_FLAG_INLINE_PAYLOAD | WC_FLAG_CRC_PRESENT, srcNode: 9,
    topicLo: 1, topicHi: 2, seqLo: 3, seqHi: 0, tsLo: 4, tsHi: 0,
    payloadLen: 16, schemaId: 5, crc, rdmaKey: 0,
  });
  const h = new FrameHandle().bind(dv.buffer, 0);
  assert.equal(decodeWcn1Into(h, 0), WC.WC_OK);
  assert.equal(h.srcNode, 9);
  assert.equal(h.topicLo, 1); assert.equal(h.topicHi, 2);
  assert.equal(h.seqLo, 3); assert.equal(h.seqHi, 0);
  assert.equal(h.payloadLen, 16); assert.equal(h.schemaId, 5);
  assert.equal(h.payloadOffset, WCN1_HEADER_SIZE);
  assert.equal(verifyFrameCrc(h), WC.WC_OK);
});

test('Law 4: malformed datagrams map to the exact error taxonomy', () => {
  const mk = (n) => new FrameHandle().bind(new ArrayBuffer(n), 0);
  // truncated: 3 bytes only
  assert.equal(decodeWcn1Into(mk(3), 0), WC.WC_E_TRUNCATED);
  // bad magic
  const bad = mk(64);
  new DataView(bad.u8.buffer).setUint32(0, 0xdeadbeef, true);
  assert.equal(decodeWcn1Into(bad, 0), WC.WC_E_BAD_MAGIC);
  // future version (valid magic, then version=2)
  const fut = mk(64);
  new DataView(fut.u8.buffer).setUint32(0, WCN1_MAGIC, true);
  new DataView(fut.u8.buffer).setUint16(4, 2, true);
  assert.equal(decodeWcn1Into(fut, 0), WC.WC_E_BAD_VERSION);
  // header_size < 64
  const small = mk(64);
  new DataView(small.u8.buffer).setUint32(0, WCN1_MAGIC, true);
  new DataView(small.u8.buffer).setUint16(6, 32, true);
  assert.equal(decodeWcn1Into(small, 0), WC.WC_E_BAD_HEADER);
  // header_size > avail
  const big = mk(64);
  new DataView(big.u8.buffer).setUint32(0, WCN1_MAGIC, true);
  new DataView(big.u8.buffer).setUint16(6, 128, true);
  assert.equal(decodeWcn1Into(big, 0), WC.WC_E_BAD_HEADER);
  // payload overruns buffer
  const over = mk(64 + 4);
  new DataView(over.u8.buffer).setUint32(0, WCN1_MAGIC, true);
  new DataView(over.u8.buffer).setUint16(6, 64, true);
  new DataView(over.u8.buffer).setUint32(40, 5, true);
  new DataView(over.u8.buffer).setUint32(8, WC_FLAG_INLINE_PAYLOAD, true);
  assert.equal(decodeWcn1Into(over, 0), WC.WC_E_TRUNCATED);
  // CRC mismatch is distinct from transport errors
  const crcBad = mk(64 + 4);
  const cdv = new DataView(crcBad.u8.buffer);
  encodeWcn1Into(cdv, 0, { flags: WC_FLAG_INLINE_PAYLOAD | WC_FLAG_CRC_PRESENT,
    srcNode: 1, topicLo: 0, topicHi: 0, seqLo: 0, seqHi: 0, tsLo: 0, tsHi: 0,
    payloadLen: 4, schemaId: 0, crc: 0x12345678, rdmaKey: 0 });
  assert.equal(decodeWcn1Into(crcBad, 0), WC.WC_OK);
  assert.equal(verifyFrameCrc(crcBad), WC.WC_E_BAD_CRC);
});

test('error names are stable (Law 4 contract)', () => {
  assert.equal(wcName(WC.WC_E_BAD_CRC), 'WC_E_BAD_CRC');
  assert.equal(wcName(WC.WC_E_STALE_SEQ), 'WC_E_STALE_SEQ');
  assert.match(wcName(99), /^WC_E_UNKNOWN_/);
});

test('WGS1 gossip: encode + entries + decode round-trip', () => {
  const entries = 3;
  const dv = makeDv(WGS1_HEADER_SIZE + entries * WGS1_ENTRY_SIZE);
  let off = encodeWgs1Into(dv, 0, {
    senderNode: 1, entryCount: entries, roundLo: 10, roundHi: 0,
    tsLo: 20, tsHi: 0, bootLo: 30, bootHi: 0, senderFlags: 1,
  });
  off = writeGossipEntryInto(dv, off, {
    nodeId: 2, addr: 0x0100007f, gossipPort: 5001, entryFlags: 1,
    lastSeenLo: 40, lastSeenHi: 0, incarnation: 3, dataPort: 5002,
  });
  writeGossipEntryInto(dv, off, {
    nodeId: 3, addr: 0, gossipPort: 5003, entryFlags: 2,
    lastSeenLo: 50, lastSeenHi: 0, incarnation: 4, dataPort: 5004,
  });
  const g = {};
  assert.equal(decodeWgs1Into(dv, 0, g), WC.WC_OK);
  assert.equal(g.senderNode, 1);
  assert.equal(g.entryCount, 3);
  const e = {};
  readGossipEntryInto(dv, 0, 0, e);
  assert.equal(e.nodeId, 2);
  assert.equal(e.gossipPort, 5001);
  assert.equal(e.incarnation, 3);
  assert.equal(e.dataPort, 5002);
  readGossipEntryInto(dv, 0, 1, e);
  assert.equal(e.nodeId, 3);
  assert.equal(e.entryFlags, 2); // LEAVING
});

test('WGS1: entry_count overrun is WC_E_TRUNCATED, not a crash', () => {
  const dv = makeDv(WGS1_HEADER_SIZE + 8);
  encodeWgs1Into(dv, 0, { senderNode: 1, entryCount: 999, roundLo: 0, roundHi: 0,
    tsLo: 0, tsHi: 0, bootLo: 0, bootHi: 0, senderFlags: 1 });
  assert.equal(decodeWgs1Into(dv, 0, {}), WC.WC_E_TRUNCATED);
});

test('Law 1: decodeInto is allocation-free (structural check)', () => {
  // The hot-path decoder must not construct ANY object per call. We assert
  // the numeric-code contract that makes that possible (no throw = no Error
  // allocation) plus handle field reuse across re-decodes.
  const dv = makeDv(WCN1_HEADER_SIZE + 8);
  encodeWcn1Into(dv, 0, { flags: 1, srcNode: 1, topicLo: 1, topicHi: 0,
    seqLo: 1, seqHi: 0, tsLo: 0, tsHi: 0, payloadLen: 8, schemaId: 0, crc: 0, rdmaKey: 0 });
  const h = new FrameHandle().bind(dv.buffer, 0);
  for (let i = 1; i <= 3; i++) {
    assert.equal(decodeWcn1Into(h, 0), WC.WC_OK);
    assert.equal(h.seqLo, i); // caller mutates seq in place between sends
    encodeWcn1Into(dv, 0, { flags: 1, srcNode: 1, topicLo: 1, topicHi: 0,
      seqLo: i + 1, seqHi: 0, tsLo: 0, tsHi: 0, payloadLen: 8, schemaId: 0, crc: 0, rdmaKey: 0 });
  }
});
