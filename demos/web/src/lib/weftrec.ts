// weftrec.ts — bit-exact .weftrec v1 parser + writer for the browser demo
//
// WHY EXISTS: FORMATS.md §1 is the normative capture/replay contract. The
// previous Playback tab shipped six hardcoded fixture frames — it never
// parsed a byte. This module implements the REAL format:
//
//   File header (32 B): magic "WREC" | format_version u16 | header_size u16
//     | flags u32 | envelope_version u32 | frame_count u32
//     | crc32(bytes 0..20) u32 | reserved 8 B
//   Frame record: rec_len u32 | envelope_header 16 B (verbatim)
//     | payload N B (N = envelope.payload_len) | crc32(bytes 4..20+N) u32
//
// CRC-32/zlib: reflected poly 0xEDB88320, init 0xFFFFFFFF, final xor
// 0xFFFFFFFF (FORMATS.md §1.4 — identical to zlib's crc32()).
//
// Classification (mirrors tools/weft-playback):
//   foreign  — envelope magic != "WEFT": the record is SKIPPED by rec_len
//              (L8 skip-unknown forward compatibility), never decoded.
//   stale    — envelope seq equals the previously claimed seq: the recorder
//              is a protocol reader; stale returns are part of the honest
//              capture and are recorded, not filtered.
//   corrupt  — record CRC mismatch (tolerated + reported, never thrown).

export const WREC_MAGIC = 0x43455257; // "WREC" LE u32
export const WEFT_MAGIC = 0x54464557; // "WEFT" LE u32

/// CRC-32/zlib table (reflected 0xEDB88320), built once.
const CRC_TABLE: Uint32Array = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  return t;
})();

/// CRC-32/zlib over `bytes` (reflected 0xEDB88320, init/final 0xFFFFFFFF).
export function crc32(bytes: Uint8Array, start = 0, end = bytes.length): number {
  let crc = 0xffffffff;
  for (let i = start; i < end; i++) {
    crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export interface WeftrecFileHeader {
  formatVersion: number;
  headerSize: number;
  flags: number;
  envelopeVersion: number;
  frameCount: number; // 0 ⇒ scan-to-EOF (crash-tolerant path)
  headerCrcOk: boolean;
  headerCrc: number;
}

export type RecordKind = 'fresh' | 'stale' | 'foreign' | 'corrupt';

export interface WeftrecRecord {
  index: number;
  kind: RecordKind;
  recLen: number;
  // Envelope fields (decoded when the magic is WEFT; zeros for foreign).
  magic: number;
  seq: number;
  version: number;
  headerSize: number;
  payloadLen: number;
  payloadCrcOk: boolean;
  payloadCrc: number;
}

export interface WeftrecParseResult {
  header: WeftrecFileHeader;
  records: WeftrecRecord[];
  /** Records actually decoded (fresh + stale); foreign/corrupt are skipped. */
  decodedCount: number;
  /** True when the file ended before frame_count records (truncation). */
  truncated: boolean;
}

function u16le(b: Uint8Array, off: number): number {
  return b[off] | (b[off + 1] << 8);
}
function u32le(b: Uint8Array, off: number): number {
  return (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)) >>> 0;
}

/// Parse a .weftrec v1 byte stream. Never throws on malformed content —
/// malformed regions are classified (crash-tolerant scan path semantics).
export function parseWeftrec(bytes: Uint8Array): WeftrecParseResult {
  const records: WeftrecRecord[] = [];
  if (bytes.length < 32 || u32le(bytes, 0) !== WREC_MAGIC) {
    return {
      header: {
        formatVersion: 0, headerSize: 0, flags: 0, envelopeVersion: 0,
        frameCount: 0, headerCrcOk: false, headerCrc: 0,
      },
      records,
      decodedCount: 0,
      truncated: true,
    };
  }

  const formatVersion = u16le(bytes, 4);
  const headerSize = u16le(bytes, 6);
  const flags = u32le(bytes, 8);
  const envelopeVersion = u32le(bytes, 12);
  const frameCount = u32le(bytes, 16);
  const headerCrc = u32le(bytes, 20);
  const headerCrcOk = crc32(bytes, 0, 20) === headerCrc;

  const header: WeftrecFileHeader = {
    formatVersion, headerSize, flags, envelopeVersion,
    frameCount, headerCrcOk, headerCrc,
  };

  // The header CRC covers bytes 0..20 regardless of header_size (FORMATS §1.1);
  // trailing unknown header fields are skipped via header_size (L8 philosophy).
  let off = headerSize >= 32 ? headerSize : 32;
  let lastSeq = -1;
  let decoded = 0;
  let index = 0;

  // frame_count == 0 ⇒ scan to EOF (crash-tolerant). Otherwise stop at count.
  const hasCount = frameCount !== 0;

  while (off + 4 <= bytes.length && (!hasCount || index < frameCount)) {
    const recLen = u32le(bytes, off);
    if (recLen < 24 || off + recLen > bytes.length) {
      // Truncated/garbage record tail — report and stop (honest truncation).
      return { header, records, decodedCount: decoded, truncated: true };
    }

    // Envelope header: bytes off+4 .. off+20 (16 B, verbatim as claimed).
    const envOff = off + 4;
    const magic = u32le(bytes, envOff);
    const version = u16le(bytes, envOff + 4);
    const envHeaderSize = u16le(bytes, envOff + 6);
    const seq = u32le(bytes, envOff + 8);
    const payloadLen = u32le(bytes, envOff + 12);

    const isWeft = magic === WEFT_MAGIC;
    const saneHeader = isWeft && envHeaderSize >= 16 && envHeaderSize <= recLen - 8;
    // Payload begins at header_size within the envelope (03-ENVELOPE §2).
    const payStart = envOff + (saneHeader ? envHeaderSize : 16);
    const payEnd = payStart + (saneHeader ? payloadLen : 0);
    const storedCrc = u32le(bytes, off + recLen - 4);
    const crcOk =
      saneHeader && payEnd + 4 <= off + recLen
        ? crc32(bytes, envOff, payEnd) === storedCrc
        : false;

    let kind: RecordKind;
    if (!isWeft) kind = 'foreign';
    else if (!crcOk) kind = 'corrupt';
    else if (seq === lastSeq) kind = 'stale';
    else kind = 'fresh';

    if (isWeft && crcOk) {
      decoded++;
      lastSeq = seq;
    }

    records.push({
      index,
      kind,
      recLen,
      magic,
      seq,
      version,
      headerSize: envHeaderSize,
      payloadLen,
      payloadCrcOk: crcOk,
      payloadCrc: storedCrc,
    });

    index++;
    off += recLen;
  }

  const truncated = hasCount && index < frameCount;
  return { header, records, decodedCount: decoded, truncated };
}

/// Serialize claimed frames into a .weftrec v1 byte stream. `frames` are
/// (envelopeHeader16B, payloadBytes) pairs exactly as claimed from a Weft —
/// the writer is a protocol reader's counterpart (WO-P2 §0.2).
export function writeWeftrec(
  envelopeVersion: number,
  frames: Array<{ header: Uint8Array; payload: Uint8Array }>
): Uint8Array {
  // Size pass.
  let total = 32;
  for (const f of frames) total += 4 + 16 + f.payload.length + 4;

  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);

  out[0] = 0x57; out[1] = 0x52; out[2] = 0x45; out[3] = 0x43; // "WREC"
  dv.setUint16(4, 1, true); // format_version
  dv.setUint16(6, 32, true); // header_size
  dv.setUint32(8, 0, true); // flags
  dv.setUint32(12, envelopeVersion, true);
  dv.setUint32(16, frames.length, true); // frame_count (patched at close)
  dv.setUint32(20, crc32(out, 0, 20), true); // CRC over bytes 0..20
  // bytes 24..32 reserved = 0

  let off = 32;
  for (const f of frames) {
    const recLen = 4 + 16 + f.payload.length + 4;
    dv.setUint32(off, recLen, true);
    out.set(f.header.subarray(0, 16), off + 4);
    out.set(f.payload, off + 20);
    // CRC over record bytes 4..20+N (envelope_header + payload).
    dv.setUint32(off + recLen - 4, crc32(out, off + 4, off + 20 + f.payload.length), true);
    off += recLen;
  }
  return out;
}
