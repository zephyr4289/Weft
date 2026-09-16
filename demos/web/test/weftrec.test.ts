/// weftrec round-trip tests — FORMATS.md §1 conformance.
///
/// These pin the bit-exact format contract: header CRC over bytes 0..20,
/// per-record CRC over envelope+payload, rec_len demarcation, L8 skip-unknown
/// (foreign records are classified, never decoded), stale-return honesty,
/// crash-tolerant scan when frame_count == 0 or the file is truncated.
import { describe, it, expect } from 'vitest';
import {
  crc32,
  parseWeftrec,
  writeWeftrec,
  WEFT_MAGIC,
  WREC_MAGIC,
} from '../src/lib/weftrec';

function envelope(seq: number, payloadLen: number, magic = WEFT_MAGIC): Uint8Array {
  const h = new Uint8Array(16);
  const dv = new DataView(h.buffer);
  dv.setUint32(0, magic, true);
  dv.setUint16(4, 1, true); // version
  dv.setUint16(6, 16, true); // header_size
  dv.setUint32(8, seq, true);
  dv.setUint32(12, payloadLen, true);
  return h;
}

describe('crc32 (FORMATS §1.4 parameters)', () => {
  it('matches the zlib canonical vectors', () => {
    // Established zlib CRC-32 vectors.
    expect(crc32(new Uint8Array(0))).toBe(0x00000000);
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
    expect(crc32(new TextEncoder().encode('The quick brown fox jumps over the lazy dog'))).toBe(0x414fa339);
  });
});

describe('writeWeftrec → parseWeftrec round-trip', () => {
  it('round-trips frames with verified CRCs', () => {
    const frames = [1, 2, 3].map((seq) => ({
      header: envelope(seq, 8),
      payload: new Uint8Array([seq, seq, seq, seq, 0xaa, 0xbb, 0xcc, 0xdd]),
    }));
    const bytes = writeWeftrec(1, frames);
    // File header checks.
    const dv = new DataView(bytes.buffer);
    expect(dv.getUint32(0, true)).toBe(WREC_MAGIC);
    expect(dv.getUint16(4, true)).toBe(1); // format_version
    expect(dv.getUint16(6, true)).toBe(32); // header_size
    expect(dv.getUint32(16, true)).toBe(3); // frame_count
    expect(dv.getUint32(20, true)).toBe(crc32(bytes, 0, 20)); // header CRC

    const parsed = parseWeftrec(bytes);
    expect(parsed.header.headerCrcOk).toBe(true);
    expect(parsed.truncated).toBe(false);
    expect(parsed.decodedCount).toBe(3);
    expect(parsed.records.map((r) => r.seq)).toEqual([1, 2, 3]);
    expect(parsed.records.every((r) => r.kind === 'fresh' && r.payloadCrcOk)).toBe(true);
  });

  it('classifies stale returns honestly (recorder is a protocol reader)', () => {
    const frames = [
      { header: envelope(5, 4), payload: new Uint8Array([1, 2, 3, 4]) },
      { header: envelope(5, 4), payload: new Uint8Array([1, 2, 3, 4]) }, // stale claim
      { header: envelope(6, 4), payload: new Uint8Array([9, 9, 9, 9]) },
    ];
    const parsed = parseWeftrec(writeWeftrec(1, frames));
    expect(parsed.records.map((r) => r.kind)).toEqual(['fresh', 'stale', 'fresh']);
    expect(parsed.decodedCount).toBe(3); // stale is still a valid record
  });

  it('skips foreign records by rec_len without decoding (L8)', () => {
    const foreign = new Uint8Array(32);
    const fdv = new DataView(foreign.buffer);
    fdv.setUint32(0, 32, true); // rec_len
    fdv.setUint32(4, 0xdeadbeef, true); // foreign magic
    fdv.setUint32(28, 0, true); // crc (never checked for foreign)
    const bytes = writeWeftrec(1, [
      { header: envelope(1, 4), payload: new Uint8Array([1, 2, 3, 4]) },
    ]);
    const merged = new Uint8Array(bytes.length + 32);
    merged.set(bytes, 0);
    merged.set(foreign, bytes.length);

    const parsed = parseWeftrec(merged);
    // frame_count says 1 → parser stops after the fresh record; the foreign
    // tail is future data (not truncated content).
    expect(parsed.records.length).toBe(1);
    expect(parsed.records[0].kind).toBe('fresh');
    expect(parsed.truncated).toBe(false);
  });

  it('flags corrupt records on CRC mismatch without throwing', () => {
    const frames = [{ header: envelope(7, 4), payload: new Uint8Array([1, 2, 3, 4]) }];
    const bytes = writeWeftrec(1, frames);
    bytes[bytes.length - 1] ^= 0xff; // flip a CRC bit
    const parsed = parseWeftrec(bytes);
    expect(parsed.records[0].kind).toBe('corrupt');
    expect(parsed.records[0].payloadCrcOk).toBe(false);
  });

  it('scan-to-EOF when frame_count == 0 (crash-tolerant)', () => {
    const bytes = writeWeftrec(1, [
      { header: envelope(1, 4), payload: new Uint8Array([1, 2, 3, 4]) },
      { header: envelope(2, 4), payload: new Uint8Array([5, 6, 7, 8]) },
    ]);
    new DataView(bytes.buffer).setUint32(16, 0, true); // crash before close
    const parsed = parseWeftrec(bytes);
    expect(parsed.header.frameCount).toBe(0);
    expect(parsed.records.length).toBe(2);
    expect(parsed.decodedCount).toBe(2);
  });

  it('reports truncation honestly when records are cut short', () => {
    const bytes = writeWeftrec(1, [
      { header: envelope(1, 4), payload: new Uint8Array([1, 2, 3, 4]) },
      { header: envelope(2, 4), payload: new Uint8Array([5, 6, 7, 8]) },
    ]);
    const cut = bytes.slice(0, bytes.length - 6); // tear the last record
    const parsed = parseWeftrec(cut);
    expect(parsed.truncated).toBe(true);
    expect(parsed.records.length).toBe(1);
  });

  it('rejects non-WREC files without throwing', () => {
    const parsed = parseWeftrec(new Uint8Array([1, 2, 3]));
    expect(parsed.truncated).toBe(true);
    expect(parsed.records.length).toBe(0);
  });
});
