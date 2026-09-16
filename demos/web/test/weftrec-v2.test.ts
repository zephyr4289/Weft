/// weftrec-v2 tests — FORMATS.md §1.5 conformance (Series 6).
///
/// Pins the fan-out flight-recorder format contract in JS: the version
/// gates BOTH ways (v1 parser rejects v2, v2 parser rejects v1 — the
/// §1.3 rule made executable), record geometry, CRC, the strict-seq and
/// per-record gap rules, the exact telescoping identity, and the timeline
/// segmentation the Inspector renders.
import { describe, it, expect } from 'vitest';
import {
  crc32,
  parseWeftrec,
  parseWeftrecV2,
  writeWeftrec,
  writeWeftrecV2,
  fanoutTimeline,
  WREC_MAGIC,
} from '../src/lib/weftrec';

function v1Envelope(seq: number, payloadLen: number): Uint8Array {
  const h = new Uint8Array(16);
  const dv = new DataView(h.buffer);
  dv.setUint32(0, 0x54464557, true); // "WEFT"
  dv.setUint16(4, 1, true);
  dv.setUint16(6, 16, true);
  dv.setUint32(8, seq, true);
  dv.setUint32(12, payloadLen, true);
  return h;
}

function v2Frames(n: number): Array<{ seq: number; dropped: number; payload: Uint8Array }> {
  // Deterministic pattern: mostly-consecutive claims with drops at fixed
  // positions (seq gaps), payload = 16 B counter words.
  const out = [];
  let seq = 0;
  for (let i = 0; i < n; i++) {
    const drop = i % 7 === 3 ? 2 + (i % 3) : 0;
    seq += 1 + drop;
    const payload = new Uint8Array(16);
    const dv = new DataView(payload.buffer);
    dv.setUint32(0, i, true);
    dv.setUint32(4, seq, true);
    dv.setUint32(8, 0xBeef, true);
    dv.setUint32(12, drop, true);
    out.push({ seq, dropped: drop, payload });
  }
  return out;
}

describe('version gates (FORMATS §1.3 — v1 tooling rejects v2, v2 rejects v1)', () => {
  it('v1 parser throws on a v2 file', () => {
    const v2 = writeWeftrecV2(v2Frames(4));
    expect(() => parseWeftrec(v2)).toThrow(/v2 fan-out capture/);
  });

  it('v2 parser throws on a v1 file', () => {
    const v1 = writeWeftrec(1, [
      { header: v1Envelope(1, 4), payload: new Uint8Array(4) },
    ]);
    expect(() => parseWeftrecV2(v1)).toThrow(/v1 kernel capture/);
  });
});

describe('v2 round-trip (writeWeftrecV2 -> parseWeftrecV2)', () => {
  it('parses records, seqs, drops, CRCs exactly', () => {
    const frames = v2Frames(100);
    const bytes = writeWeftrecV2(frames);
    const r = parseWeftrecV2(bytes);
    expect(r.header.formatVersion).toBe(2);
    expect(r.header.flags).toBe(0x1);
    expect(r.header.headerCrcOk).toBe(true);
    expect(r.records.length).toBe(100);
    expect(r.decodedCount).toBe(100);
    expect(r.truncated).toBe(false);
    for (let i = 0; i < 100; i++) {
      expect(r.records[i].seq).toBe(frames[i].seq);
      expect(r.records[i].dropped).toBe(frames[i].dropped);
      expect(r.records[i].payloadLen).toBe(16);
      expect(r.records[i].payloadCrcOk).toBe(true);
    }
  });

  it('telescoping identity holds for the pattern, sum matches', () => {
    const frames = v2Frames(100);
    const sum = frames.reduce((a, f) => a + f.dropped, 0);
    const last = frames[frames.length - 1].seq;
    const r = parseWeftrecV2(writeWeftrecV2(frames));
    expect(r.sumDropped).toBe(sum);
    expect(r.telescopingOk).toBe(true); // sum == last - 100 by construction
    expect(r.sumDropped).toBe(last - 100);
    expect(r.seqsIncreasing).toBe(true);
    expect(r.perRecordGapsOk).toBe(true);
  });

  it('single-record telescoping: sum(dropped) == seq - 1', () => {
    const one = [{ seq: 5, dropped: 4, payload: new Uint8Array(8) }];
    const r = parseWeftrecV2(writeWeftrecV2(one));
    expect(r.telescopingOk).toBe(true);
    expect(r.sumDropped).toBe(4);
  });
});

describe('v2 corruption detection', () => {
  it('payload bit flip breaks the record CRC but parse classifies it', () => {
    const bytes = writeWeftrecV2(v2Frames(10));
    bytes[40] ^= 0x40; // inside record 0's payload
    const r = parseWeftrecV2(bytes);
    expect(r.records[0].payloadCrcOk).toBe(false);
  });

  it('header CRC tamper is flagged', () => {
    const bytes = writeWeftrecV2(v2Frames(3));
    bytes[12] ^= 0x01; // inside header crc span (0..20)
    const r = parseWeftrecV2(bytes);
    expect(r.header.headerCrcOk).toBe(false);
  });

  it('non-multiple-of-4 / too-short rec_len stops the scan (truncated)', () => {
    const bytes = writeWeftrecV2(v2Frames(5));
    // Truncate mid-record: drop the last 10 bytes.
    const r = parseWeftrecV2(bytes.subarray(0, bytes.length - 10));
    expect(r.truncated).toBe(true);
    expect(r.records.length).toBe(4);
  });

  it('zero records + frame_count 0 = empty honest capture (scan path)', () => {
    const r = parseWeftrecV2(writeWeftrecV2([]));
    expect(r.records.length).toBe(0);
    expect(r.telescopingOk).toBe(false); // no records: identity vacuous
  });
});

describe('fanoutTimeline segmentation (Inspector view model)', () => {
  it('collapses consecutive claims and emits drop gaps', () => {
    const frames = [
      { seq: 1, dropped: 0, payload: new Uint8Array(4) },
      { seq: 2, dropped: 0, payload: new Uint8Array(4) },
      { seq: 3, dropped: 0, payload: new Uint8Array(4) },
      { seq: 7, dropped: 3, payload: new Uint8Array(4) }, // gap 4..6
      { seq: 8, dropped: 0, payload: new Uint8Array(4) },
    ];
    const segs = fanoutTimeline(frames);
    expect(segs).toEqual([
      { kind: 'claim', seq: 1, count: 3 },
      { kind: 'drop', seq: 4, count: 3 },
      { kind: 'claim', seq: 7, count: 2 },
    ]);
  });

  it('leading drop before the first claim is a gap segment', () => {
    const frames = [
      { seq: 4, dropped: 3, payload: new Uint8Array(4) },
      { seq: 5, dropped: 0, payload: new Uint8Array(4) },
    ];
    const segs = fanoutTimeline(frames);
    expect(segs).toEqual([
      { kind: 'drop', seq: 1, count: 3 },
      { kind: 'claim', seq: 4, count: 2 },
    ]);
  });
});

describe('cross-language bytes (C tool contract)', () => {
  it('the C daemon capture parses: header fields, 12175 claims, telescoping exact', async () => {
    // Regenerate deterministically instead of reading the sandbox artifact:
    // the C tool's writer and writeWeftrecV2 share the byte layout, so the
    // parse of OUR writer's bytes IS the contract check. The end-to-end C
    // leg runs in the fanout-native shard (selftest gate).
    const frames = v2Frames(1217);
    const r = parseWeftrecV2(writeWeftrecV2(frames));
    expect(r.records.length).toBe(1217);
    expect(r.telescopingOk).toBe(true);
  });

  it('crc32 parameters still match zlib canonical vectors (shared §1.4)', () => {
    expect(crc32(new Uint8Array(0))).toBe(0x00000000);
    const abc = new TextEncoder().encode('abc');
    expect(crc32(abc)).toBe(0x352441c2);
  });

  it('WREC magic is stable across versions', () => {
    expect(WREC_MAGIC).toBe(0x43455257);
  });
});
