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
  if (formatVersion !== WREC_FORMAT_VERSION_V1) {
    // Version gate (FORMATS.md §1.3): v1 tooling MUST reject v2 files —
    // a v2 fan-out capture is not a kernel capture and must not be
    // misparsed as one (parseWeftrecV2 owns it, and vice versa).
    throw new Error(
      `parseWeftrec: format_version ${formatVersion} != 1 — ` +
      (formatVersion === 2 ? 'this is a v2 fan-out capture (use parseWeftrecV2)'
                           : 'unknown future version'));
  }
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

// ---------------------------------------------------------------------------
// .weftrec v2 — fan-out ring flight-recorder captures (FORMATS.md §1.5)
// Series 6: the JS parse the lead's roadmap named — the browser side of the
// daemon (tools/weft-fanout-rec daemon --shm ...), so an Inspector can load
// a captured session and show the fan-out timeline with drop accounting.
// ---------------------------------------------------------------------------

export const WREC_FORMAT_VERSION_V1 = 1;
export const WREC_FORMAT_VERSION_V2 = 2;
export const WREC_FLAG_FANOUT = 0x1;

/** One v2 fan-out claim record (the only v2 record kind so far). */
export interface WeftrecV2Record {
  index: number;
  /** Ring frame seq of this claim (strictly increasing across records). */
  seq: number;
  /** Frames completed without the recorder observing them (per-claim
   *  RFC-0004 accounting — the honest capture: drops visible). */
  dropped: number;
  payloadLen: number;
  payloadCrcOk: boolean;
}

export interface WeftrecV2ParseResult {
  header: WeftrecFileHeader;
  records: WeftrecV2Record[];
  decodedCount: number;
  truncated: boolean;
  /** Per-record telescoping check: dropped_i == seq_i - seq_{i-1} - 1. */
  perRecordGapsOk: boolean;
  /** Seqs strictly increasing. */
  seqsIncreasing: boolean;
  /** The exact telescoping identity: sum(dropped) == last_seq - records. */
  telescopingOk: boolean;
  /** sum(dropped) — the headline number the Inspector badge shows. */
  sumDropped: number;
}

function u64le(b: Uint8Array, off: number): number {
  // Safe up to 2^53 — ring seqs are u64 but every real session is far below;
  // the JS number road is declared (no BigInt on the hot parse path).
  let v = 0;
  for (let i = 7; i >= 0; i--) v = v * 256 + b[off + i];
  return v;
}

/// Parse a .weftrec v2 byte stream (fan-out flight recorder). Throws on a
/// v1 file (version gate — FORMATS.md §1.3: v1 tooling rejects v2 and vice
/// versa; here the v2 parser owns v2 only).
export function parseWeftrecV2(bytes: Uint8Array): WeftrecV2ParseResult {
  const empty: WeftrecV2ParseResult = {
    header: {
      formatVersion: 0, headerSize: 0, flags: 0, envelopeVersion: 0,
      frameCount: 0, headerCrcOk: false, headerCrc: 0,
    },
    records: [], decodedCount: 0, truncated: true,
    perRecordGapsOk: true, seqsIncreasing: true, telescopingOk: false, sumDropped: 0,
  };
  if (bytes.length < 32 || u32le(bytes, 0) !== WREC_MAGIC) return empty;
  const formatVersion = u16le(bytes, 4);
  if (formatVersion !== WREC_FORMAT_VERSION_V2) {
    throw new Error(
      `parseWeftrecV2: format_version ${formatVersion} != 2 — ` +
      (formatVersion === 1 ? 'this is a v1 kernel capture (use parseWeftrec)'
                           : 'unknown future version'));
  }
  const headerSize = u16le(bytes, 6);
  const flags = u32le(bytes, 8);
  const envelopeVersion = u32le(bytes, 12);
  const frameCount = u32le(bytes, 16);
  const headerCrc = u32le(bytes, 20);
  const headerCrcOk = crc32(bytes, 0, 20) === headerCrc;
  const header: WeftrecFileHeader = {
    formatVersion, headerSize, flags, envelopeVersion, frameCount, headerCrcOk, headerCrc,
  };

  let off = headerSize >= 32 ? headerSize : 32;
  const records: WeftrecV2Record[] = [];
  let lastSeq = 0;
  let sumDropped = 0;
  let gapsOk = true;
  let increasing = true;
  let index = 0;
  const hasCount = frameCount !== 0;

  while (off + 4 <= bytes.length && (!hasCount || index < frameCount)) {
    const recLen = u32le(bytes, off);
    // v2 record: rec_len u32 | kind u16 | reserved u16 | seq u64 | dropped
    // u64 | payload_len u32 | reserved u32 | payload N | crc32 u32
    if (recLen < 36 || (recLen % 4) !== 0 || off + recLen > bytes.length) {
      return { ...empty, header, records, decodedCount: index, truncated: true,
               perRecordGapsOk: gapsOk, seqsIncreasing: increasing, sumDropped };
    }
    const kind = u16le(bytes, off + 4);
    if (kind !== 1) {
      // Unknown kinds are a version violation in v2 (its sole kind); unlike
      // v1's L8 skip, the v2 reader stops — declared in FORMATS.md §1.5.
      return { ...empty, header, records, decodedCount: index, truncated: true,
               perRecordGapsOk: gapsOk, seqsIncreasing: increasing, sumDropped };
    }
    const seq = u64le(bytes, off + 8);
    const dropped = u64le(bytes, off + 16);
    const payloadLen = u32le(bytes, off + 24);
    if (36 + payloadLen !== recLen) {
      return { ...empty, header, records, decodedCount: index, truncated: true,
               perRecordGapsOk: gapsOk, seqsIncreasing: increasing, sumDropped };
    }
    const storedCrc = u32le(bytes, off + recLen - 4);
    const crcOk = crc32(bytes, off + 4, off + recLen - 4) === storedCrc;

    if (index > 0 && seq <= lastSeq) increasing = false;
    if (dropped !== seq - lastSeq - 1) gapsOk = false;

    records.push({ index, seq, dropped, payloadLen, payloadCrcOk: crcOk });
    sumDropped += dropped;
    lastSeq = seq;
    index++;
    off += recLen;
  }

  const truncated = hasCount && index < frameCount;
  // The exact telescoping identity (RFC 0004): sum(dropped) == lastSeq - n.
  const telescopingOk = records.length > 0 && sumDropped === lastSeq - records.length;
  return { header, records, decodedCount: index, truncated,
           perRecordGapsOk: gapsOk, seqsIncreasing: increasing,
           telescopingOk, sumDropped };
}

/// Serialize v2 records into a .weftrec v2 byte stream — the writer half of
/// the round-trip contract (tests + future in-browser capture from a SAB
/// ring via weft_fanout_attach-style readers).
export function writeWeftrecV2(
  frames: Array<{ seq: number; dropped: number; payload: Uint8Array }>,
): Uint8Array {
  let total = 32;
  for (const f of frames) total += 32 + f.payload.length + 4;
  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  out[0] = 0x57; out[1] = 0x52; out[2] = 0x45; out[3] = 0x43; // "WREC"
  dv.setUint16(4, WREC_FORMAT_VERSION_V2, true);
  dv.setUint16(6, 32, true);
  dv.setUint32(8, WREC_FLAG_FANOUT, true);
  dv.setUint32(12, 0, true); // envelope_version: fan-out frames carry none
  dv.setUint32(16, frames.length, true);
  dv.setUint32(20, crc32(out, 0, 20), true);
  let off = 32;
  for (const f of frames) {
    const recLen = 32 + f.payload.length + 4;
    dv.setUint32(off, recLen, true);
    dv.setUint16(off + 4, 1, true); // kind = fanout claim
    dv.setUint16(off + 6, 0, true);
    dv.setBigUint64(off + 8, BigInt(f.seq), true);
    dv.setBigUint64(off + 16, BigInt(f.dropped), true);
    dv.setUint32(off + 24, f.payload.length, true);
    dv.setUint32(off + 28, 0, true);
    out.set(f.payload, off + 32);
    dv.setUint32(off + recLen - 4, crc32(out, off + 4, off + 32 + f.payload.length), true);
    off += recLen;
  }
  return out;
}

/** A renderable timeline segment: one claimed frame or one drop gap. */
export interface FanoutTimelineSegment {
  kind: 'claim' | 'drop';
  seq: number;         // first seq of the segment
  count: number;       // claims in run (kind=claim) or dropped frames (drop)
}

/// Collapse parsed records into claim-runs and drop-gaps — the shape the
/// Inspector timeline renders (a gap segment exists only where dropped > 0).
export function fanoutTimeline(records: WeftrecV2Record[]): FanoutTimelineSegment[] {
  const segs: FanoutTimelineSegment[] = [];
  let prevSeq = 0;
  for (const r of records) {
    if (r.dropped > 0) segs.push({ kind: 'drop', seq: r.seq - r.dropped, count: r.dropped });
    if (segs.length > 0 && segs[segs.length - 1].kind === 'claim' &&
        segs[segs.length - 1].seq + segs[segs.length - 1].count === r.seq) {
      segs[segs.length - 1].count++;
    } else {
      segs.push({ kind: 'claim', seq: r.seq, count: 1 });
    }
    prevSeq = r.seq;
  }
  void prevSeq;
  return segs;
}
