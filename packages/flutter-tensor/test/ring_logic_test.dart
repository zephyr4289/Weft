// ring_logic_test.dart — PURE-DART tests for ring_header.dart (WTR1 layout).
//
// ZERO framework dependencies (no flutter, no package:test, no dart:ffi):
// a standalone main() harness runnable on the plain VM via
//
//   dart run test/ring_logic_test.dart      (or)   dart test/ring_logic_test.dart
//
// Exit code 0 = all green, 1 = any failure (CI-gated; this sandbox has no
// Dart SDK, so test/static_audit.mjs mirrors the structural checks here).
//
// CRC known-answer vectors were NOT guessed: they were computed with
// node:zlib crc32 (Node 24, authoritative zlib CRC-32/IEEE) via
// `node -e` and the printed values embedded below — see worklog Task 12-b:
//   KAT1 crc32([0x57,0x45,0x46,0x54])            = 3421166146 (0xCBEADA42)
//   KAT2 crc32 over the fixed header region      = 1553622279 (0x5C9A6507)
//   KAT3 crc32(0..255 ascending)                 = 688229491  (0x29058C73)
// test/static_audit.mjs RE-DERIVES all three with zlib and diffs the
// embedded numbers, so the vectors cannot silently drift.
//
// Law 2 (strict LE): every multi-byte ByteData access below passes
// Endian.little explicitly. Law 4 (boundary validation): fail-closed
// corruption matrix with typed codes identical to the TS implementation.
library weft_flutter_tensor.test.ring_logic_test;

import 'dart:io' show exitCode;
import 'dart:typed_data';

import 'package:weft_flutter_tensor/src/ring_header.dart';

// -- CRC known-answer constants (derived from node:zlib, do not edit) --------
const int katCrcWeftBytes = 3421166146; // zlib.crc32("WEFT" bytes)
const int katCrcHeaderRegion = 1553622279; // zlib.crc32 over KAT2 header region
const int katCrcAscending256 = 688229491; // zlib.crc32(0..255)

var _passed = 0;
var _failed = 0;

void _check(bool cond, String name) {
  if (cond) {
    _passed++;
    print('PASS: $name');
  } else {
    _failed++;
    print('FAIL: $name');
  }
}

void _checkCode(Object? fn(), String code, String name) {
  try {
    fn();
    _check(false, '$name (no exception thrown)');
  } on LayoutException catch (e) {
    _check(e.code == code, '$name (got ${e.code})');
  }
}

// ---------------------------------------------------------------------------
// Header writer — builds the EXACT KAT2 header that test/static_audit.mjs
// rebuilds in Node when it re-derives katCrcHeaderRegion with zlib.crc32.
// ---------------------------------------------------------------------------

ByteData buildKat2Header() {
  final bd = ByteData(128);
  bd.setUint8(0, 0x57);
  bd.setUint8(1, 0x45);
  bd.setUint8(2, 0x46);
  bd.setUint8(3, 0x54); // "WEFT"
  bd.setUint16(4, 1, Endian.little); // layout_version
  bd.setUint16(6, 128, Endian.little); // header_size
  bd.setUint32(8, 4, Endian.little); // slot_count
  bd.setUint32(12, 128, Endian.little); // slot_stride
  bd.setUint8(16, 1); // dtype_code = kDLUInt
  bd.setUint8(17, 8); // dtype_bits
  bd.setUint16(18, 1, Endian.little); // lanes
  bd.setUint32(20, 1, Endian.little); // elem_size
  bd.setUint32(24, 2, Endian.little); // shape[0] = 2
  bd.setUint32(28, 3, Endian.little); // shape[1] = 3
  bd.setUint32(56, 3, Endian.little); // strides[0] = 3 (elements)
  bd.setUint32(60, 1, Endian.little); // strides[1] = 1
  bd.setUint32(88, 0x55667788, Endian.little); // schema_id lo
  bd.setUint32(92, 0x11223344, Endian.little); // schema_id hi
  // producer_seq [96,104) stays 0 (excluded from the CRC region).
  bd.setUint32(104, 120, Endian.little); // tick_hz
  bd.setUint32(108, 1, Endian.little); // flags = little_endian
  bd.setUint32(112, 0, Endian.little); // crc (excluded from CRC region)
  return bd;
}

/// Writes a committed slot header (mirrors TS/ring.js finishCommit order).
void writeSlot(
  ByteData bd,
  int slotBase, {
  required int payloadLen,
  required int seq,
  int timestampNs = 0,
  int durationUs = 0,
  int flags = slotFlagCommitted,
  int fourcc = 0x41424752, // "RGBA"
  int rank = 2,
  int planes = 1,
}) {
  bd.setUint8(slotBase + soffMagic + 0, 0x57);
  bd.setUint8(slotBase + soffMagic + 1, 0x46);
  bd.setUint8(slotBase + soffMagic + 2, 0x52);
  bd.setUint8(slotBase + soffMagic + 3, 0x4d); // "WFRM"
  bd.setUint32(slotBase + soffSlotFlags, 0, Endian.little);
  bd.setUint32(slotBase + soffPayloadLen, payloadLen, Endian.little);
  bd.setUint32(slotBase + soffSeq, seq & 0xffffffff, Endian.little);
  bd.setUint32(slotBase + soffSeq + 4, (seq >>> 32) & 0xffffffff, Endian.little);
  bd.setUint32(slotBase + soffTimestampNs, timestampNs & 0xffffffff, Endian.little);
  bd.setUint32(
      slotBase + soffTimestampNs + 4, (timestampNs >>> 32) & 0xffffffff, Endian.little);
  bd.setUint32(slotBase + soffDurationUs, durationUs, Endian.little);
  bd.setUint32(slotBase + soffFourcc, fourcc, Endian.little);
  bd.setUint8(slotBase + soffRank, rank);
  bd.setUint8(slotBase + soffPlanes, planes);
  bd.setUint32(slotBase + soffSlotFlags, flags, Endian.little);
}

void main() {
  // -- CRC known-answer vectors (node:zlib-derived) --------------------------
  _check(
      crc32Bytes(const [0x57, 0x45, 0x46, 0x54]) == katCrcWeftBytes,
      'crc32 KAT1: "WEFT" bytes -> $katCrcWeftBytes (node:zlib)');
  final kat2 = buildKat2Header();
  _check(
      ringHeaderCrc(kat2) == katCrcHeaderRegion,
      'crc32 KAT2: WTR1 header region [0,96)+[104,112) -> '
      '$katCrcHeaderRegion (node:zlib)');
  final asc = List<int>.generate(256, (i) => i, growable: false);
  _check(crc32Bytes(asc) == katCrcAscending256,
      'crc32 KAT3: 0..255 -> $katCrcAscending256 (node:zlib)');

  // -- valid header parse -----------------------------------------------------
  final info = RingHeaderInfo.parse(kat2);
  _check(info.slotCount == 4, 'parse: slot_count == 4');
  _check(info.slotStride == 128, 'parse: slot_stride == 128');
  _check(info.headerSize == 128, 'parse: header_size == 128');
  _check(info.version == 1, 'parse: layout_version == 1');
  _check(info.dtypeCode == dlPackUInt && info.dtypeBits == 8,
      'parse: dtype == u8 (DLPack verbatim)');
  _check(info.dtypeLanes == 1 && info.elemSize == 1, 'parse: lanes/elem_size');
  _check(info.rank == 2, 'parse: rank == 2');
  _check(info.shape[0] == 2 && info.shape[1] == 3 && info.shape[2] == 0,
      'parse: shape [2,3] 0-padded');
  _check(info.strides[0] == 3 && info.strides[1] == 1, 'parse: strides [3,1] (elements)');
  _check(info.schemaIdLo == 0x55667788 && info.schemaIdHi == 0x11223344,
      'parse: schema_id lo/hi');
  _check(info.schemaId == 0x1122334455667788, 'parse: schema_id composed');
  _check(info.tickHz == 120, 'parse: tick_hz == 120');
  _check(info.flags == ringFlagLittleEndian, 'parse: flags bit0 (LE) set');
  _check(info.payloadCap == 64, 'parse: payload_cap == stride - 64');
  _check(info.byteLength == 128, 'parse: byteLength passthrough');

  // -- corruption matrix (Law 4, TS-parity decision order) --------------------
  ByteData corrupt(void Function(ByteData) mutate) {
    final bd = buildKat2Header();
    mutate(bd);
    return bd;
  }

  _checkCode(
      () => RingHeaderInfo.parse(corrupt((bd) => bd.setUint8(0, 0x58))),
      'WTR1_BAD_MAGIC', 'matrix: bad magic');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint16(4, 2, Endian.little))),
      'WTR1_BAD_VERSION', 'matrix: bad layout_version');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint16(6, 64, Endian.little))),
      'WTR1_BAD_HEADER_SIZE', 'matrix: bad header_size');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint32(8, 1, Endian.little))),
      'WTR1_BAD_SLOT_COUNT', 'matrix: slot_count < 2');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint32(12, 100, Endian.little))),
      'WTR1_BAD_SLOT_STRIDE', 'matrix: slot_stride not 64-aligned');
  _checkCode(
      () => RingHeaderInfo.parse(corrupt((bd) {
            bd.setUint8(17, 7); // bits = 7 -> invalid dtype
          })),
      'WTR1_BAD_DTYPE', 'matrix: bad dtype');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint32(20, 2, Endian.little))),
      'WTR1_BAD_ELEM_SIZE', 'matrix: elem_size mismatch');
  _checkCode(
      () => RingHeaderInfo.parse(corrupt((bd) {
            for (var d = 0; d < 8; d++) {
              bd.setUint32(24 + 4 * d, 0, Endian.little);
            }
          })),
      'WTR1_BAD_RANK', 'matrix: all-zero shape');
  _checkCode(
      () => RingHeaderInfo.parse(corrupt((bd) {
            bd.setUint32(56, 1, Endian.little); // strides [1,3]: not row-major sane
          })),
      'WTR1_BAD_STRIDES', 'matrix: insane strides');
  _checkCode(
      () => RingHeaderInfo.parse(corrupt((bd) {
            // producer_seq hi word — outside the CRC region, still fail-closed.
            bd.setUint32(100, seqHiLimit, Endian.little);
          })),
      'WTR1_SEQ_OVERFLOW', 'matrix: producer_seq hi >= 2^21');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint32(108, 2, Endian.little))),
      'WTR1_NOT_LITTLE_ENDIAN', 'matrix: flags bit0 clear');
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint32(104, 121, Endian.little))),
      'WTR1_BAD_CRC', 'matrix: crc mismatch (tick_hz mutated)');
  _checkCode(
      () => RingHeaderInfo.parse(ByteData.sublistView(buildKat2Header(), 0, 64)),
      'WTR1_SHORT', 'matrix: buffer shorter than header');
  _checkCode(
      () => RingHeaderInfo.parse(buildKat2Header(), byteLength: 500),
      'WTR1_SHORT_RING', 'matrix: buffer shorter than header + slots');

  // lanes != 1 fails closed (same typed code as TS).
  _checkCode(() => RingHeaderInfo.parse(corrupt((bd) => bd.setUint16(18, 2, Endian.little))),
      'WTR1_BAD_DTYPE', 'matrix: lanes != 1');

  // -- slot header read --------------------------------------------------------
  final ringBd = ByteData(ringHeaderSize + 2 * 128);
  // Reuse the KAT2 static config but shrink to 2 slots for the slot tests.
  final slotRing = ByteData(ringHeaderSize + 2 * 128);
  final katBytes = buildKat2Header();
  for (var i = 0; i < ringHeaderSize; i++) {
    slotRing.setUint8(i, katBytes.getUint8(i));
  }
  slotRing.setUint32(offSlotCount, 2, Endian.little);
  slotRing.setUint32(offHeaderCrc, ringHeaderCrc(slotRing), Endian.little);
  final slotInfo = RingHeaderInfo.parse(slotRing);
  _check(slotInfo.slotCount == 2, 'slot ring: re-CRCed 2-slot header parses');

  const seqBig = 0x100000005; // exercises the lo/hi split (4294967301)
  writeSlot(ringBd, ringHeaderSize,
      payloadLen: 32, seq: seqBig, timestampNs: 0x1BC0000000000000 + 7, durationUs: 8333);
  final meta = SlotHeaderInfo();
  _check(readSlotHeader(ringBd, ringHeaderSize, meta), 'slot: committed slot reads OK');
  _check(meta.seq == seqBig && meta.seqLo == 5 && meta.seqHi == 1,
      'slot: 64-bit seq lo/hi composition');
  _check(meta.payloadLen == 32, 'slot: payload_len');
  _check(meta.timestampNs == 0x1BC0000000000000 + 7, 'slot: timestamp ns composition');
  _check(meta.durationUs == 8333, 'slot: duration_us');
  _check(meta.fourcc == 0x41424752, 'slot: fourcc u32 ("RGBA")');
  _check(meta.rank == 2 && meta.planes == 1, 'slot: rank/planes');
  _check(meta.committed, 'slot: committed bit visible');

  writeSlot(ringBd, ringHeaderSize + 128, payloadLen: 1, seq: 2, flags: 0);
  _check(!readSlotHeader(ringBd, ringHeaderSize + 128, meta),
      'slot: uncommitted (torn marker) -> false');

  final noMagic = ByteData(64);
  _check(!readSlotHeader(noMagic, 0, meta), 'slot: zeroed slot (no magic) -> false');

  // -- fourcc + dtype helpers ---------------------------------------------------
  _check(fourccFromString('RGBA') == 0x41424752, 'fourcc: "RGBA" -> 0x41424752 (LE)');
  _check(fourccToString(0x41424752) == 'RGBA', 'fourcc: 0x41424752 -> "RGBA"');
  _check(dtypeIsValid(dlPackUInt, 8) && dtypeIsValid(dlPackFloat, 32),
      'dtype: u8/f32 valid');
  _check(!dtypeIsValid(dlPackInt, 7) && !dtypeIsValid(dlPackBFloat, 16),
      'dtype: invalid pairs rejected');

  // -- summary ------------------------------------------------------------------
  print('');
  print('ring_logic_test: $_passed passed, $_failed failed');
  if (_failed > 0) exitCode = 1;
}
