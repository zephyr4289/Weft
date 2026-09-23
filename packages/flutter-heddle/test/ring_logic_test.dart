// test/ring_logic_test.dart — dependency-free pure-Dart harness (dart run).
// NO flutter/test/ffi imports: geometry, seqlock protocol, corruption matrix,
// and fixture-manifest parity run on any Dart SDK (CI flutter lane or local).
import 'dart:io';
import 'dart:typed_data';

import 'package:weft_flutter/src/hpl1_layout.dart';
import 'package:weft_flutter/src/hot_plane.dart';

var _passed = 0;
var _failed = 0;

void check(bool cond, String label) {
  if (cond) {
    _passed++;
    stdout.writeln('  ok  $label');
  } else {
    _failed++;
    stdout.writeln('FAIL  $label');
  }
}

void main() {
  // ---- geometry derivation (matches heddle-core layout.js + fixtures) ----
  final g4 = Hpl1.deriveGeometry(4, 8);
  check(g4.dirtyWords == 1, '4x8 dirtyWords == 1');
  check(g4.laneCtrlBase == 136, '4x8 laneCtrlBase == 136');
  check(g4.ringBase == 392, '4x8 ringBase == 392');
  check(g4.totalBytes == 648, '4x8 totalBytes == 648');
  final g1 = Hpl1.deriveGeometry(1, 2);
  check(g1.totalBytes == 216, '1x2 totalBytes == 216');
  check(g33().dirtyWords == 2, '33 lanes → 2 dirty words');
  check(Hpl1.align8(129) == 136, 'align8(129) == 136');
  check(!Hpl1.isPow2(3) && Hpl1.isPow2(256), 'isPow2 gate');

  // ---- build a synthetic plane (producer semantics, HPL1 §4.1) ----
  final bytes = Uint8List(g4.totalBytes);
  final dv = ByteData.sublistView(bytes);
  dv.setUint32(Hpl1.hdrMagic, Hpl1.magicU32, Endian.little);
  dv.setUint32(Hpl1.hdrVersion, Hpl1.version, Endian.little);
  dv.setUint32(Hpl1.hdrFlags, Hpl1.flagLeRequired, Endian.little);
  dv.setUint32(Hpl1.hdrLaneCount, 4, Endian.little);
  dv.setUint32(Hpl1.hdrSamplesPerLane, 8, Endian.little);
  dv.setUint32(Hpl1.hdrRingMask, 7, Endian.little);
  dv.setUint32(Hpl1.hdrDirtyWords, g4.dirtyWords, Endian.little);
  dv.setUint32(Hpl1.hdrLaneCtrlStride, Hpl1.laneCtrlStride, Endian.little);
  dv.setUint32(Hpl1.hdrRingBase, g4.ringBase, Endian.little);
  dv.setUint32(Hpl1.hdrTotalBytes, g4.totalBytes, Endian.little);
  for (var lane = 0; lane < 4; lane++) {
    final ctrl = g4.laneCtrl(lane);
    for (var k = 0; k < 8; k++) {
      final v = (10 + lane + k).toDouble();
      dv.setUint32(ctrl + Hpl1.laneSeq, (k * 2 + 1), Endian.little); // odd
      dv.setFloat64(ctrl + Hpl1.laneCurrent, v, Endian.little);
      dv.setUint32(ctrl + Hpl1.laneHead, k + 1, Endian.little);
      dv.setUint32(ctrl + Hpl1.laneSeq, (k * 2 + 2), Endian.little); // even
      dv.setFloat64(g4.laneRing(lane) + k * 8, v, Endian.little);
    }
  }

  final plane = WeftHotPlane.fromBytes(bytes);
  check(plane.laneCount == 4 && plane.samplesPerLane == 8, 'plane validated');
  // producer-set dirty bits for all 4 lanes (HPL1 §3: producer ORs, holds)
  dv.setUint32(Hpl1.headerSize, 0x0F, Endian.little);

  // ---- seqlock acquire: values + in-place reuse ----
  final out = WeftLaneSnapshot();
  check(plane.readLane(1, out) == Hpl1.ok, 'readLane ok');
  check(out.current == 12.0, 'lane1 current == 12 (10+1+1)');
  final same = out;
  plane.readLane(1, out);
  check(identical(same, out), 'snapshot mutated in place (no churn)');

  // ---- corruption matrix (typed codes) ----
  final corrupt = (int code, void Function(ByteData) mutate) {
    final copy = Uint8List.fromList(bytes);
    mutate(ByteData.sublistView(copy));
    try {
      WeftHotPlane.fromBytes(copy);
      return false;
    } on Hpl1Exception catch (e) {
      return e.code == code;
    }
  };
  check(corrupt(Hpl1.badMagic, (d) => d.setUint32(Hpl1.hdrMagic, 0xdeadbeef, Endian.little)),
      'bad magic → HPL1_BAD_MAGIC');
  check(corrupt(Hpl1.badVersion, (d) => d.setUint32(Hpl1.hdrVersion, 2, Endian.little)),
      'bad version → HPL1_BAD_VERSION');
  check(corrupt(Hpl1.notLittleEndian, (d) => d.setUint32(Hpl1.hdrFlags, 0, Endian.little)),
      'LE flag clear → HPL1_NOT_LITTLE_ENDIAN');
  check(corrupt(Hpl1.capacityMismatch, (d) => d.setUint32(Hpl1.hdrSamplesPerLane, 7, Endian.little)),
      'non-pow2 → HPL1_CAPACITY_MISMATCH');
  check(corrupt(Hpl1.capacityMismatch, (d) => d.setUint32(Hpl1.hdrRingMask, 3, Endian.little)),
      'ringMask drift → HPL1_CAPACITY_MISMATCH');
  check(corrupt(Hpl1.capacityMismatch, (d) => d.setUint32(Hpl1.hdrTotalBytes, 4, Endian.little)),
      'totalBytes drift → HPL1_CAPACITY_MISMATCH');

  // ---- torn seqlock: odd seq never returns a value ----
  final torn = Uint8List.fromList(bytes);
  final tU32 = Uint32List.sublistView(torn);
  tU32[g4.laneCtrl(0) >> 2] |= 1;
  final tPlane = WeftHotPlane.fromBytes(torn);
  final tOut = WeftLaneSnapshot();
  final before = tOut.current;
  check(tPlane.readLane(0, tOut) == Hpl1.tornSeqlock, 'odd seq → torn code');
  check(tOut.current == before && tPlane.tears > 0, 'value NOT returned, tear counted');

  // ---- readRecent: newest-first + wrap ----
  final win = Float64List(8);
  final n = plane.readRecent(1, 8, win);
  check(n == 8, 'readRecent full window');
  check(win[0] == 18.0 && win[7] == 11.0, 'newest-first ring order (lane1 = 11..18)');

  // ---- dirty mask: producer-set bits observed, transitions tracked ----
  final changed = Uint32List(64);
  check(plane.scanDirty(changed) == 4, '4 lanes newly dirty');
  check(plane.scanDirty(changed) == 0, 'no transitions without new publish');
  check(plane.dirtyCount == 4, 'dirtyCount = set bits');

  // ---- header snapshot + epoch change (Law 4) ----
  final hdr = WeftHeaderSnapshot();
  check(plane.readHeader(hdr) == Hpl1.ok, 'header ok (epoch primed)');
  dv.setUint32(Hpl1.hdrEpoch, 2, Endian.little); // producer restart
  check(plane.readHeader(hdr) == Hpl1.epochChanged, 'epoch change explicit');
  check(plane.readHeader(hdr) == Hpl1.ok, 'epoch stable after');

  // ---- lane out of range is a caller error ----
  try {
    plane.readLane(9, out);
    check(false, 'lane 9 of 4 throws');
  } on Hpl1Exception catch (e) {
    check(e.code == Hpl1.laneOutOfRange, 'lane 9 of 4 → HPL1_LANE_OUT_OF_RANGE');
  }

  stdout.writeln('ring_logic_test: $_passed passed, $_failed failed');
  exit(_failed == 0 ? 0 : 1);
}

Hpl1Geometry g33() => Hpl1.deriveGeometry(33, 4);
