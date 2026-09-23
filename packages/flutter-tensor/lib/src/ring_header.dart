// ring_header.dart — WTR1 (Weft Tensor Ring v1) normative layout constants,
// fail-closed header parse/validate, slot-header read, CRC-32/IEEE.
//
// Spec: docs/weft-tensor/LAYOUT-V1.md (NORMATIVE).
// TypeScript reference: packages/weft-tensor/src/layout.js — the constants
// below are byte-exact parity with that file, enforced MECHANICALLY by
// test/static_audit.mjs (check: constants parity diff).
//
// PURE DART: this file intentionally imports neither `package:flutter` nor
// `dart:ffi`, so it runs on the plain VM (fixture tooling, CI shards, tests).
//
// Law 2 (strict LE + IEEE 754 parity): every multi-byte ByteData access in
// this file passes Endian.little EXPLICITLY. Implicit-endian access is a
// CI-failing crime in this repo (test/static_audit.mjs scans every call site).
//
// Law 4 (boundary schema validation): parse/validate is fail-closed with
// typed error codes IDENTICAL to the TypeScript implementation
// (WTR1_BAD_MAGIC, WTR1_BAD_VERSION, ... WTR1_SHORT_RING).
library weft_flutter_tensor.src.ring_header;

import 'dart:typed_data';

// ---------------------------------------------------------------------------
// Wire constants — MUST stay in lockstep with packages/weft-tensor/src/layout.js
// (test/static_audit.mjs diffs them; names are the lowerCamel mirrors of the
// TS export names, declaration form `const int <name> = <value>;` is part of
// the audit contract).
// ---------------------------------------------------------------------------

/// ASCII magic of the ring header: "WEFT".
const List<int> ringMagicBytes = <int>[0x57, 0x45, 0x46, 0x54];

/// ASCII magic of a slot header: "WFRM".
const List<int> slotMagicBytes = <int>[0x57, 0x46, 0x52, 0x4d];

const int layoutVersion = 1;
const int ringHeaderSize = 128;
const int slotHeaderSize = 64;

// Ring header offsets.
const int offMagic = 0;
const int offLayoutVersion = 4;
const int offHeaderSize = 6;
const int offSlotCount = 8;
const int offSlotStride = 12;
const int offDtypeCode = 16;
const int offDtypeBits = 17;
const int offLanes = 18;
const int offElemSize = 20;
const int offShape = 24; // u32 x8
const int offStrides = 56; // u32 x8, in ELEMENTS (DLPack convention)
const int offSchemaId = 88; // u64 (read/written as lo/hi u32 pair)
const int offProducerSeq = 96; // u64 — the publish word (lo/hi u32 pair)
const int offTickHz = 104;
const int offFlags = 108;
const int offHeaderCrc = 112; // CRC-32/IEEE over bytes [0,96) ++ [104,112)

// Ring flags.
const int ringFlagLittleEndian = 1;
const int ringFlagSharedMemory = 2;

// Slot header offsets (relative to slot base).
const int soffMagic = 0;
const int soffPayloadLen = 4;
const int soffSeq = 8; // u64 (lo/hi u32 pair)
const int soffTimestampNs = 16; // u64 (lo/hi u32 pair)
const int soffDurationUs = 24;
const int soffSlotFlags = 28;
const int soffFourcc = 32;
const int soffRank = 36;
const int soffPlanes = 37;
const int soffPlaneOffset = 40; // u32 x3
const int soffPlaneSize = 52; // u32 x3

/// Slot header flags: bit0 = COMMITTED (written last before publish).
const int slotFlagCommitted = 1;

/// Max rank this layout supports (shape[8]/strides[8]).
const int maxRank = 8;

/// producer_seq bound: the hi word must stay below [seqHiLimit] so that
/// seq < 2^53 — the exact-integer domain shared with the managed TS path
/// (Pillar 1 Lo/Hi discipline). Dart VM ints are 64-bit, so the managed
/// path here is exact for the full u64, but we keep the SAME bound so all
/// Weft implementations reject the same rings (Law 2 parity, fail-closed).
const int seqHiLimit = 0x200000;

// DLPack DLDataTypeCode — used VERBATIM on the wire so the Python DLPack
// bridge maps dtype with zero translation (no table, no drift — Law 2/4).
const int dlPackInt = 0;
const int dlPackUInt = 1;
const int dlPackFloat = 2;
const int dlPackBFloat = 3;
const int dlPackComplex = 4;
const int dlPackBool = 5;

/// (code, bits) pairs accepted by V1, encoded as (code << 8) | bits —
/// parity with layout.js VALID_DTYPES.
const Set<int> _validDtypes = <int>{
  0x008, 0x010, 0x020, 0x040, // i8/i16/i32/i64   (kDLInt)
  0x108, 0x110, 0x120, 0x140, // u8/u16/u32/u64   (kDLUInt)
  0x210, 0x220, 0x240, // f16/f32/f64      (kDLFloat)
  0x508, // bool8            (kDLBool)
};

/// True when the (code, bits) pair is representable in WTR1.
bool dtypeIsValid(int code, int bits) => _validDtypes.contains((code << 8) | bits);

// ---------------------------------------------------------------------------
// CRC-32/IEEE (zlib-compatible) over the static header config.
//
// The table is built ONCE at first use (module-lifetime allocation — the
// same discipline as layout.js; Law 1 concerns the steady-state frame path,
// not attach). Verified against node:zlib crc32 in test/ring_logic_test.dart
// (known-answer vectors derived from `node -e` printing zlib.crc32(...)).
// ---------------------------------------------------------------------------

const int _crcPoly = 0xEDB88320; // CRC-32/IEEE reflected polynomial

final Uint32List _crcTable = _buildCrcTable();

Uint32List _buildCrcTable() {
  final t = Uint32List(256);
  for (var n = 0; n < 256; n++) {
    var c = n;
    for (var k = 0; k < 8; k++) {
      c = (c & 1) != 0 ? (_crcPoly ^ (c >>> 1)) : (c >>> 1);
    }
    t[n] = c >>> 0;
  }
  return t;
}

/// Generic CRC-32/IEEE over [bytes] (zlib-compatible).
int crc32Bytes(List<int> bytes) {
  final t = _crcTable;
  var c = 0xffffffff;
  for (var i = 0; i < bytes.length; i++) {
    c = t[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

/// CRC-32/IEEE over the static ring-header config: bytes [0,96) ++ [104,112).
/// Excludes the mutable producer_seq ([96,104)) and the CRC field itself —
/// identical to TS `ringHeaderCrc`.
int ringHeaderCrc(ByteData bd) {
  final t = _crcTable;
  var c = 0xffffffff;
  for (var i = 0; i < 96; i++) {
    c = t[(c ^ bd.getUint8(i)) & 0xff] ^ (c >>> 8);
  }
  for (var i = 104; i < 112; i++) {
    c = t[(c ^ bd.getUint8(i)) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

// ---------------------------------------------------------------------------
// fourcc helpers (LE ASCII in a u32) — parity with layout.js.
// ---------------------------------------------------------------------------

/// "RGBA" -> bytes 'R','G','B','A' -> LE u32 0x41424752.
int fourccFromString(String s) {
  final b0 = s.codeUnitAt(0) & 0xff;
  final b1 = s.codeUnitAt(1) & 0xff;
  final b2 = s.codeUnitAt(2) & 0xff;
  final b3 = s.codeUnitAt(3) & 0xff;
  return ((b3 << 24) | (b2 << 16) | (b1 << 8) | b0) >>> 0;
}

/// Inverse of [fourccFromString] (diagnostic use — allocates a String).
String fourccToString(int u32) => String.fromCharCode(
      u32 & 0xff,
      (u32 >>> 8) & 0xff,
      (u32 >>> 16) & 0xff,
      (u32 >>> 24) & 0xff,
    );

// ---------------------------------------------------------------------------
// Fail-closed errors — codes are STRING-IDENTICAL to the TS LayoutError codes
// so logs/CI gates can be diffed across languages (Law 4).
// ---------------------------------------------------------------------------

/// Thrown by [RingHeaderInfo.parse] for EVERY corruption class (fail-closed).
class LayoutException implements Exception {
  const LayoutException(this.code, this.message);

  /// Machine-readable code, e.g. "WTR1_BAD_MAGIC" (matches TS).
  final String code;

  /// Human explanation.
  final String message;

  @override
  String toString() => 'LayoutException[$code]: $message';
}

// ---------------------------------------------------------------------------
// Slot header read (consumer hot path).
// ---------------------------------------------------------------------------

/// Reused per-ring slot-header scratch (Law 1: the acquire path never
/// allocates — [readSlotHeader] fills a CALLER-PROVIDED instance).
class SlotHeaderInfo {
  int seq = 0;
  int seqLo = 0;
  int seqHi = 0;
  int payloadLen = 0;
  int timestampLo = 0;
  int timestampHi = 0;
  int durationUs = 0;
  int flags = 0;
  int fourcc = 0;
  int rank = 0;
  int planes = 0;

  /// Composed timestamp in ns. Exact for timestamps < 2^63 ns (year 2262);
  /// Dart VM ints are 64-bit two's complement so the bit pattern is exact.
  int get timestampNs => (timestampHi << 32) | timestampLo;

  /// True when the COMMITTED bit is set.
  bool get committed => (flags & slotFlagCommitted) != 0;
}

/// Read + validate one slot header into the CALLER-PROVIDED [out] so the hot
/// loop stays allocation-free. Returns false when the slot is not a valid
/// committed frame (torn write, wrong magic, uncommitted, out-of-bound seq) —
/// byte-identical decision table to TS `readSlotHeader` (Law 4).
bool readSlotHeader(ByteData bd, int slotBase, SlotHeaderInfo out) {
  if (bd.getUint8(slotBase + soffMagic + 0) != slotMagicBytes[0]) return false;
  if (bd.getUint8(slotBase + soffMagic + 1) != slotMagicBytes[1]) return false;
  if (bd.getUint8(slotBase + soffMagic + 2) != slotMagicBytes[2]) return false;
  if (bd.getUint8(slotBase + soffMagic + 3) != slotMagicBytes[3]) return false;
  final flags = bd.getUint32(slotBase + soffSlotFlags, Endian.little);
  if ((flags & slotFlagCommitted) == 0) return false; // torn / in-progress write
  final seqLo = bd.getUint32(slotBase + soffSeq, Endian.little);
  final seqHi = bd.getUint32(slotBase + soffSeq + 4, Endian.little);
  if (seqHi >= seqHiLimit) return false; // out of managed bound
  out.seq = (seqHi << 32) | seqLo;
  out.seqLo = seqLo;
  out.seqHi = seqHi;
  out.payloadLen = bd.getUint32(slotBase + soffPayloadLen, Endian.little);
  out.timestampLo = bd.getUint32(slotBase + soffTimestampNs, Endian.little);
  out.timestampHi = bd.getUint32(slotBase + soffTimestampNs + 4, Endian.little);
  out.durationUs = bd.getUint32(slotBase + soffDurationUs, Endian.little);
  out.flags = flags;
  out.fourcc = bd.getUint32(slotBase + soffFourcc, Endian.little);
  out.rank = bd.getUint8(slotBase + soffRank);
  out.planes = bd.getUint8(slotBase + soffPlanes);
  return true;
}

// ---------------------------------------------------------------------------
// Ring header parse/validate (attach-time boundary — Law 4).
// ---------------------------------------------------------------------------

/// Parsed, validated WTR1 ring header. Allocated ONCE at attach; consumed by
/// [WeftRing] (and readable by any tooling on the plain VM).
class RingHeaderInfo {
  const RingHeaderInfo({
    required this.version,
    required this.headerSize,
    required this.slotCount,
    required this.slotStride,
    required this.dtypeCode,
    required this.dtypeBits,
    required this.dtypeLanes,
    required this.elemSize,
    required this.shape,
    required this.strides,
    required this.rank,
    required this.schemaIdLo,
    required this.schemaIdHi,
    required this.tickHz,
    required this.flags,
    required this.payloadCap,
    required this.byteLength,
  });

  final int version;
  final int headerSize;
  final int slotCount;
  final int slotStride;

  /// DLPack DLDataTypeCode (verbatim wire value).
  final int dtypeCode;
  final int dtypeBits;
  final int dtypeLanes;

  /// Bytes per element: (dtypeBits / 8) * dtypeLanes.
  final int elemSize;

  /// shape[8], u32 per dimension, 0-padded past [rank].
  final List<int> shape;

  /// strides[8] in ELEMENTS (row-major), 0-padded past [rank].
  final List<int> strides;

  /// Dimensions used (1..8).
  final int rank;
  final int schemaIdLo;
  final int schemaIdHi;
  final int tickHz;
  final int flags;

  /// slot_stride - slot_header_size — live payload capacity per slot.
  final int payloadCap;
  final int byteLength;

  /// schema_id as a signed 64-bit composition. Bit-exact; for the full
  /// unsigned value interpret via [schemaIdLo]/[schemaIdHi].
  int get schemaId => (schemaIdHi << 32) | schemaIdLo;

  bool get isSharedMemory => (flags & ringFlagSharedMemory) != 0;

  /// Human-readable one-line identity (diagnostic — allocates).
  String describe() {
    final sb = StringBuffer('WeftRingHeader(v');
    sb..write(version)..write(', slots=')..write(slotCount);
    sb..write(', stride=')..write(slotStride);
    sb..write(', dtype=')..write(dtypeCode)..write('/')..write(dtypeBits);
    sb..write(', shape=[');
    for (var d = 0; d < rank; d++) {
      if (d > 0) sb.write('x');
      sb.write(shape[d]);
    }
    sb..write('], tickHz=')..write(tickHz)..write(')');
    return sb.toString();
  }

  /// Validate a WTR1 ring header at the start of [bd] (Law 4 boundary).
  /// Fail-closed: throws [LayoutException] with a code for EVERY corruption
  /// class, in the SAME decision order as TS `validateRingHeader` so
  /// corruption-matrix tests agree across languages:
  ///
  /// SHORT -> magic -> version -> header_size -> slot_count -> slot_stride
  /// -> dtype -> elem_size -> rank -> strides -> seq bound -> little-endian
  /// -> crc -> short ring.
  static RingHeaderInfo parse(ByteData bd, {int? byteLength}) {
    final bl = byteLength ?? bd.lengthInBytes;
    if (bl < ringHeaderSize) {
      throw LayoutException(
          'WTR1_SHORT', 'buffer is ${bl}B, ring header needs ${ringHeaderSize}B');
    }
    // Magic: byte-exact "WEFT" (Law 2 — never a guessed-endian u32 compare).
    for (var i = 0; i < 4; i++) {
      if (bd.getUint8(offMagic + i) != ringMagicBytes[i]) {
        throw const LayoutException('WTR1_BAD_MAGIC', 'ring magic is not "WEFT"');
      }
    }
    final version = bd.getUint16(offLayoutVersion, Endian.little);
    if (version != layoutVersion) {
      throw LayoutException('WTR1_BAD_VERSION', 'layout_version $version != $layoutVersion');
    }
    final headerSize = bd.getUint16(offHeaderSize, Endian.little);
    if (headerSize != ringHeaderSize) {
      throw LayoutException('WTR1_BAD_HEADER_SIZE', 'header_size $headerSize != $ringHeaderSize');
    }
    final slotCount = bd.getUint32(offSlotCount, Endian.little);
    if (slotCount < 2) {
      throw LayoutException('WTR1_BAD_SLOT_COUNT', 'slot_count $slotCount < 2');
    }
    final slotStride = bd.getUint32(offSlotStride, Endian.little);
    if ((slotStride & 63) != 0 || slotStride < slotHeaderSize) {
      throw LayoutException(
          'WTR1_BAD_SLOT_STRIDE', 'slot_stride $slotStride is not 64-aligned / < 64');
    }
    final code = bd.getUint8(offDtypeCode);
    final bits = bd.getUint8(offDtypeBits);
    final lanes = bd.getUint16(offLanes, Endian.little);
    if (!dtypeIsValid(code, bits) || lanes != 1) {
      throw LayoutException(
          'WTR1_BAD_DTYPE', 'dtype $code/${bits}bits x${lanes}lanes not supported in V1');
    }
    final elemSize = bd.getUint32(offElemSize, Endian.little);
    if (elemSize != (bits >>> 3) * lanes) {
      throw LayoutException(
          'WTR1_BAD_ELEM_SIZE', 'elem_size $elemSize != ${(bits >>> 3) * lanes}');
    }
    // shape/strides rank.
    final shape = List<int>.filled(maxRank, 0, growable: false);
    final strides = List<int>.filled(maxRank, 0, growable: false);
    var rank = 0;
    for (var d = 0; d < maxRank; d++) {
      final s = bd.getUint32(offShape + 4 * d, Endian.little);
      if (s != 0) rank = d + 1;
      shape[d] = s;
    }
    for (var d = 0; d < maxRank; d++) {
      strides[d] = bd.getUint32(offStrides + 4 * d, Endian.little);
    }
    if (rank == 0) {
      throw const LayoutException(
          'WTR1_BAD_RANK', 'shape is all-zero (rank-0 tensors not supported in V1)');
    }
    if (!_stridesSane(shape, strides, rank)) {
      throw const LayoutException(
          'WTR1_BAD_STRIDES', 'strides are not a sane row-major layout');
    }
    final seqHi = bd.getUint32(offProducerSeq + 4, Endian.little);
    if (seqHi >= seqHiLimit) {
      throw const LayoutException('WTR1_SEQ_OVERFLOW',
          'producer_seq exceeds 2^53 — unsupported by the managed path');
    }
    final flags = bd.getUint32(offFlags, Endian.little);
    if ((flags & ringFlagLittleEndian) == 0) {
      throw const LayoutException('WTR1_NOT_LITTLE_ENDIAN',
          'flags bit0 (little-endian) not set — refusing to guess byte order');
    }
    final wantCrc = bd.getUint32(offHeaderCrc, Endian.little);
    final gotCrc = ringHeaderCrc(bd);
    if (wantCrc != gotCrc) {
      throw LayoutException('WTR1_BAD_CRC',
          'header crc ${wantCrc.toRadixString(16)} != computed ${gotCrc.toRadixString(16)}');
    }
    final need = headerSize + slotCount * slotStride;
    if (bl < need) {
      throw LayoutException('WTR1_SHORT_RING',
          'buffer ${bl}B < header + $slotCount slots (${need}B)');
    }
    final schemaLo = bd.getUint32(offSchemaId, Endian.little);
    final schemaHi = bd.getUint32(offSchemaId + 4, Endian.little);
    return RingHeaderInfo(
      version: version,
      headerSize: headerSize,
      slotCount: slotCount,
      slotStride: slotStride,
      dtypeCode: code,
      dtypeBits: bits,
      dtypeLanes: lanes,
      elemSize: elemSize,
      shape: shape,
      strides: strides,
      rank: rank,
      schemaIdLo: schemaLo,
      schemaIdHi: schemaHi,
      tickHz: bd.getUint32(offTickHz, Endian.little),
      flags: flags,
      payloadCap: slotStride - slotHeaderSize,
      byteLength: bl,
    );
  }

  /// Row-major stride sanity: strides[i] >= strides[i+1] * shape[i+1]
  /// (elements) and the innermost stride >= 1 — parity with layout.js.
  static bool _stridesSane(List<int> shape, List<int> strides, int rank) {
    for (var i = 0; i < rank - 1; i++) {
      if (strides[i] < strides[i + 1] * shape[i + 1]) return false;
    }
    if (rank > 0 && strides[rank - 1] < 1) return false;
    return true;
  }
}
