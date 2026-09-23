// lib/src/hpl1_layout.dart — HPL1 layout constants + geometry, byte-exact
// parity with packages/heddle-core/src/layout.js (audited by test/static_audit.mjs).
//
// Law 2: every multi-byte accessor in this package passes Endian.little
// EXPLICITLY. u64 fields are lo/hi u32 pairs with lo at the field offset
// (lo-last publish ordering — readers gate on lo).
//
// Law 3: managed-side only — mirrors the normative spec
// docs/heddle2/HPL1-LAYOUT-V1.md; core/c/ is untouched.

import 'dart:typed_data';

/// HPL1 Law-4 error taxonomy (stable numeric codes, mirrors errors.js).
class Hpl1 {
  static const int ok = 0;
  static const int badMagic = 1;
  static const int badVersion = 2;
  static const int notLittleEndian = 3;
  static const int capacityMismatch = 4;
  static const int laneOutOfRange = 5;
  static const int tornSeqlock = 6;
  static const int epochChanged = 7;
  static const int planeDetached = 8;
  static const int contextLost = 9;
  static const int tabHidden = 10;
  static const int workerCrash = 11;
  static const int badRenderEngine = 12;
  static const int invalidSample = 13;
  static const int ringUnderrun = 14;

  static const List<String> names = [
    'HPL1_OK', 'HPL1_BAD_MAGIC', 'HPL1_BAD_VERSION', 'HPL1_NOT_LITTLE_ENDIAN',
    'HPL1_CAPACITY_MISMATCH', 'HPL1_LANE_OUT_OF_RANGE', 'HPL1_TORN_SEQLOCK',
    'HPL1_EPOCH_CHANGED', 'HPL1_PLANE_DETACHED', 'HPL1_CONTEXT_LOST',
    'HPL1_TAB_HIDDEN', 'HPL1_WORKER_CRASH', 'HPL1_BAD_RENDER_ENGINE',
    'HPL1_INVALID_SAMPLE', 'HPL1_RING_UNDERRUN',
  ];

  // ---- pinned layout constants (HPL1 v1) ----
  static const int headerSize = 128;
  static const int laneCtrlStride = 64;
  static const int magicU32 = 0x314C5048; // 'H','P','L','1' little-endian
  static const int version = 1;
  static const int flagLeRequired = 1;
  static const int flagEpochStable = 2;
  static const int laneFlagActive = 1;
  static const int laneFlagManual = 2;
  static const int maxLanes = 4096;

  // header field offsets
  static const int hdrMagic = 0x00;
  static const int hdrVersion = 0x04;
  static const int hdrFlags = 0x08;
  static const int hdrLaneCount = 0x0C;
  static const int hdrSamplesPerLane = 0x10;
  static const int hdrRingMask = 0x14;
  static const int hdrTickHz = 0x18;
  static const int hdrReserved0 = 0x1C;
  static const int hdrEpoch = 0x20;
  static const int hdrPublishSeq = 0x28;
  static const int hdrLastPublishNs = 0x30;
  static const int hdrFramesDropped = 0x38;
  static const int hdrGlobalMin = 0x40;
  static const int hdrGlobalMax = 0x48;
  static const int hdrGlobalAvg = 0x50;
  static const int hdrGlobalCurrent = 0x58;
  static const int hdrDirtyWords = 0x60;
  static const int hdrLaneCtrlStride = 0x64;
  static const int hdrRingBase = 0x68;
  static const int hdrTotalBytes = 0x6C;

  // lane control block offsets (relative to laneCtrlBase + lane*64)
  static const int laneSeq = 0x00;
  static const int laneCurrent = 0x08;
  static const int laneMin = 0x10;
  static const int laneMax = 0x18;
  static const int laneAvg = 0x20;
  static const int laneSamplesSeen = 0x28;
  static const int laneHead = 0x30;
  static const int laneFlags = 0x34;
  static const int lanePublishNs = 0x38;
  static const int laneDrops = 0x40;

  static int align8(int x) => (x + 7) & ~7;

  static bool isPow2(int x) => x >= 2 && (x & (x - 1)) == 0;

  /// Pure geometry derivation — single source of truth for offsets.
  static Hpl1Geometry deriveGeometry(int laneCount, int samplesPerLane) {
    final dirtyWords = (laneCount / 32).ceil();
    final laneCtrlBase = headerSize + align8(4 * dirtyWords);
    final ringBase = align8(laneCtrlBase + laneCtrlStride * laneCount);
    final totalBytes = ringBase + laneCount * samplesPerLane * 8;
    return Hpl1Geometry(
      dirtyWords: dirtyWords,
      laneCtrlBase: laneCtrlBase,
      ringBase: ringBase,
      totalBytes: totalBytes,
      laneCount: laneCount,
      samplesPerLane: samplesPerLane,
    );
  }

  /// Fail-closed validation of an existing plane (HPL1 §2 redundancy checks).
  static Hpl1Geometry validate(Uint8List bytes, {int byteOffset = 0}) {
    if (byteOffset % 8 != 0) {
      throw Hpl1Exception(planeDetached, 'byteOffset $byteOffset not 8-aligned');
    }
    if (bytes.length - byteOffset < headerSize) {
      throw Hpl1Exception(planeDetached, 'buffer too small');
    }
    final dv = ByteData.sublistView(bytes, byteOffset);
    final magic = dv.getUint32(hdrMagic, Endian.little);
    if (magic != magicU32) {
      throw Hpl1Exception(badMagic, 'magic 0x${magic.toRadixString(16)}');
    }
    final ver = dv.getUint32(hdrVersion, Endian.little);
    if (ver != version) throw Hpl1Exception(badVersion, 'version $ver');
    final flags = dv.getUint32(hdrFlags, Endian.little);
    if ((flags & flagLeRequired) == 0) {
      throw Hpl1Exception(notLittleEndian, 'LE_REQUIRED flag clear');
    }
    final laneCount = dv.getUint32(hdrLaneCount, Endian.little);
    final samplesPerLane = dv.getUint32(hdrSamplesPerLane, Endian.little);
    if (laneCount < 1 || laneCount > maxLanes) {
      throw Hpl1Exception(capacityMismatch, 'laneCount $laneCount out of range');
    }
    if (!isPow2(samplesPerLane)) {
      throw Hpl1Exception(capacityMismatch, 'samplesPerLane $samplesPerLane not pow2');
    }
    final ringMask = dv.getUint32(hdrRingMask, Endian.little);
    if (ringMask != samplesPerLane - 1) {
      throw Hpl1Exception(capacityMismatch, 'ringMask drift');
    }
    final geo = deriveGeometry(laneCount, samplesPerLane);
    final checks = <List<int>>[
      [dv.getUint32(hdrDirtyWords, Endian.little), geo.dirtyWords],
      [dv.getUint32(hdrLaneCtrlStride, Endian.little), laneCtrlStride],
      [dv.getUint32(hdrRingBase, Endian.little), geo.ringBase],
      [dv.getUint32(hdrTotalBytes, Endian.little), geo.totalBytes],
    ];
    for (final c in checks) {
      if (c[0] != c[1]) throw Hpl1Exception(capacityMismatch, 'derived vs stored drift');
    }
    if (bytes.length - byteOffset < geo.totalBytes) {
      throw Hpl1Exception(planeDetached, 'buffer smaller than plane');
    }
    return geo;
  }
}

/// Derived plane geometry (all offsets in bytes).
class Hpl1Geometry {
  final int dirtyWords;
  final int laneCtrlBase;
  final int ringBase;
  final int totalBytes;
  final int laneCount;
  final int samplesPerLane;
  const Hpl1Geometry({
    required this.dirtyWords,
    required this.laneCtrlBase,
    required this.ringBase,
    required this.totalBytes,
    required this.laneCount,
    required this.samplesPerLane,
  });
  int laneCtrl(int lane) => laneCtrlBase + lane * laneCtrlStride;
  int laneRing(int lane) => ringBase + lane * samplesPerLane * 8;
}

/// Typed HPL1 failure. `code` is the stable numeric taxonomy value.
class Hpl1Exception implements Exception {
  final int code;
  final String detail;
  Hpl1Exception(this.code, this.detail);
  @override
  String toString() => 'Hpl1Exception(${Hpl1.names[code]}): $detail';
}
