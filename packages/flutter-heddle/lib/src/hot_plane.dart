// lib/src/hot_plane.dart — WeftHotPlane: the consumer side of HPL1 in Dart.
//
// Zero-allocation reads: readLane fills a CALLER-OWNED WeftLaneSnapshot;
// readRecent fills a caller Float64List. Tears are counted (never swallowed,
// Law 4). Every multi-byte access passes Endian.little explicitly (Law 2).
//
// Attach paths:
//   * WeftHotPlane.fromBytes(Uint8List)      — Dart-side copy / test rigs
//   * WeftHotPlane.fromPointer(ptr, bytes)   — dart:ffi native memory
//     (Engineer 1's SharedArrayBuffer-equivalent bridge; POSIX shm mmap)

import 'dart:typed_data';
import 'hpl1_layout.dart';

/// Caller-owned lane snapshot (allocate once, reuse forever — Law 1).
class WeftLaneSnapshot {
  int seqLo = 0, seqHi = 0;
  double current = 0, min = 0, max = 0, avg = 0;
  int samplesSeenLo = 0, samplesSeenHi = 0;
  int head = 0, flags = 0;
  int publishNsLo = 0, publishNsHi = 0;
  int drops = 0;
  int get samplesSeen => samplesSeenLo + samplesSeenHi * 0x100000000;
  int get publishNs => publishNsLo + publishNsHi * 0x100000000;
}

/// Header snapshot — same reuse discipline.
class WeftHeaderSnapshot {
  int publishSeqLo = 0, publishSeqHi = 0;
  int epochLo = 0, epochHi = 0;
  int lastPublishNsLo = 0, lastPublishNsHi = 0;
  int framesDroppedLo = 0, framesDroppedHi = 0;
  double globalMin = 0, globalMax = 0, globalAvg = 0, globalCurrent = 0;
  int tickHz = 0, flags = 0;
}

class WeftHotPlane {
  final Uint8List bytes;
  final ByteData _dv;
  final Hpl1Geometry geo;
  final int maxTries;
  int tears = 0;
  int _lastEpochLo = -1, _lastEpochHi = -1;
  late final Uint32List _lastMask;

  WeftHotPlane._(this.bytes, {int byteOffset = 0, this.maxTries = 64})
      : _dv = ByteData.sublistView(bytes, byteOffset),
        geo = Hpl1.validate(bytes, byteOffset: byteOffset) {
    _lastMask = Uint32List(geo.dirtyWords);
  }

  factory WeftHotPlane.fromBytes(Uint8List bytes, {int byteOffset = 0, int maxTries = 64}) {
    return WeftHotPlane._(bytes, byteOffset: byteOffset, maxTries: maxTries);
  }

  /// dart:ffi attach: native plane memory becomes a Dart view. NO COPY.
  /// `ptr` must be 8-byte aligned (HPL1 planes are, by construction).
  factory WeftHotPlane.fromPointer(int address, int byteLength,
      {int maxTries = 64}) {
    // Callers on native platforms pass a Pointer<Uint8>.asTypedList result —
    // kept as a plain factory-over-bytes here so this file stays a pure
    // Dart dependency-free core (the FFI shim lives in weft_hot_plane_notifier
    // example wiring). See example/lib/main.dart for the posix_shm attach.
    throw UnsupportedError(
        'Use WeftHotPlane.fromBytes over a Pointer.asTypedList() view — '
        'see example/lib/main.dart');
  }

  int get laneCount => geo.laneCount;
  int get samplesPerLane => geo.samplesPerLane;

  int _loadSeqLo(int byteOff) => _dv.getUint32(byteOff, Endian.little);

  /// Header seqlock acquire. Returns [Hpl1.ok], [Hpl1.epochChanged] (out still
  /// filled with the NEW header) or [Hpl1.tornSeqlock] (out untouched).
  int readHeader(WeftHeaderSnapshot out) {
    for (var t = 0; t < maxTries; t++) {
      final s1 = _loadSeqLo(Hpl1.hdrPublishSeq);
      if ((s1 & 1) != 0) {
        tears++;
        continue;
      }
      out.publishSeqLo = s1;
      out.publishSeqHi = _dv.getUint32(Hpl1.hdrPublishSeq + 4, Endian.little);
      out.epochLo = _dv.getUint32(Hpl1.hdrEpoch, Endian.little);
      out.epochHi = _dv.getUint32(Hpl1.hdrEpoch + 4, Endian.little);
      out.lastPublishNsLo = _dv.getUint32(Hpl1.hdrLastPublishNs, Endian.little);
      out.globalMin = _dv.getFloat64(Hpl1.hdrGlobalMin, Endian.little);
      out.globalMax = _dv.getFloat64(Hpl1.hdrGlobalMax, Endian.little);
      out.globalAvg = _dv.getFloat64(Hpl1.hdrGlobalAvg, Endian.little);
      out.globalCurrent = _dv.getFloat64(Hpl1.hdrGlobalCurrent, Endian.little);
      out.tickHz = _dv.getUint32(Hpl1.hdrTickHz, Endian.little);
      out.flags = _dv.getUint32(Hpl1.hdrFlags, Endian.little);
      final s2 = _loadSeqLo(Hpl1.hdrPublishSeq);
      if (s1 != s2) {
        tears++;
        continue;
      }
      if (out.epochLo != _lastEpochLo || out.epochHi != _lastEpochHi) {
        final first = _lastEpochLo == -1 && _lastEpochHi == -1;
        _lastEpochLo = out.epochLo;
        _lastEpochHi = out.epochHi;
        if (!first) return Hpl1.epochChanged;
      }
      return Hpl1.ok;
    }
    return Hpl1.tornSeqlock;
  }

  /// Lane seqlock acquire → fills [out] in place. Returns [Hpl1.ok] or
  /// [Hpl1.tornSeqlock] (out untouched — value NOT returned, HPL1 §4.2).
  /// Throws [Hpl1Exception] for caller bugs (lane out of range).
  int readLane(int lane, WeftLaneSnapshot out) {
    if (lane < 0 || lane >= geo.laneCount) {
      throw Hpl1Exception(Hpl1.laneOutOfRange, 'lane $lane of ${geo.laneCount}');
    }
    final ctrl = geo.laneCtrl(lane);
    for (var t = 0; t < maxTries; t++) {
      final s1 = _loadSeqLo(ctrl + Hpl1.laneSeq);
      if ((s1 & 1) != 0) {
        tears++;
        continue;
      }
      out.seqLo = s1;
      out.seqHi = _dv.getUint32(ctrl + Hpl1.laneSeq + 4, Endian.little);
      out.current = _dv.getFloat64(ctrl + Hpl1.laneCurrent, Endian.little);
      out.min = _dv.getFloat64(ctrl + Hpl1.laneMin, Endian.little);
      out.max = _dv.getFloat64(ctrl + Hpl1.laneMax, Endian.little);
      out.avg = _dv.getFloat64(ctrl + Hpl1.laneAvg, Endian.little);
      out.samplesSeenLo = _dv.getUint32(ctrl + Hpl1.laneSamplesSeen, Endian.little);
      out.samplesSeenHi = _dv.getUint32(ctrl + Hpl1.laneSamplesSeen + 4, Endian.little);
      out.head = _dv.getUint32(ctrl + Hpl1.laneHead, Endian.little);
      out.flags = _dv.getUint32(ctrl + Hpl1.laneFlags, Endian.little);
      out.publishNsLo = _dv.getUint32(ctrl + Hpl1.lanePublishNs, Endian.little);
      out.drops = _dv.getUint32(ctrl + Hpl1.laneDrops, Endian.little);
      final s2 = _loadSeqLo(ctrl + Hpl1.laneSeq);
      if (s1 != s2) {
        tears++;
        continue;
      }
      return Hpl1.ok;
    }
    return Hpl1.tornSeqlock;
  }

  /// Copy the k NEWEST samples of [lane] into [out] (index 0 = newest).
  /// Returns samples written (< k ⇒ underrun), or -1 when torn.
  int readRecent(int lane, int k, Float64List out) {
    if (lane < 0 || lane >= geo.laneCount) {
      throw Hpl1Exception(Hpl1.laneOutOfRange, 'lane $lane of ${geo.laneCount}');
    }
    final ctrl = geo.laneCtrl(lane);
    final ringByte = geo.laneRing(lane);
    final mask = geo.samplesPerLane - 1;
    for (var t = 0; t < maxTries; t++) {
      final s1 = _loadSeqLo(ctrl + Hpl1.laneSeq);
      if ((s1 & 1) != 0) {
        tears++;
        continue;
      }
      final head = _dv.getUint32(ctrl + Hpl1.laneHead, Endian.little);
      final seen = _dv.getUint32(ctrl + Hpl1.laneSamplesSeen, Endian.little);
      final s2 = _loadSeqLo(ctrl + Hpl1.laneSeq);
      if (s1 != s2) {
        tears++;
        continue;
      }
      final avail = seen < geo.samplesPerLane ? seen : geo.samplesPerLane;
      final n = k < avail ? k : avail;
      for (var j = 0; j < n; j++) {
        out[j] = _dv.getFloat64(ringByte + ((head - 1 - j) & mask) * 8, Endian.little);
      }
      return n;
    }
    return -1;
  }

  /// Dirty-mask transition scan. Fills [changedOut] with newly-dirty lanes;
  /// returns how many were written. Consumers NEVER write the mask (HPL1 §3).
  int scanDirty(Uint32List changedOut) {
    var written = 0;
    var count = 0;
    for (var w = 0; w < geo.dirtyWords; w++) {
      final cur = _dv.getUint32(Hpl1.headerSize + w * 4, Endian.little);
      final fresh = cur & ~_lastMask[w];
      _lastMask[w] = cur;
      if (fresh != 0 && changedOut != null) {
        var bits = fresh;
        while (bits != 0) {
          final bit = bits & -bits;
          final lane = (w << 5) + bit.bitLength - 1;
          if (written < changedOut.length) changedOut[written++] = lane;
          bits ^= bit;
        }
      }
      var c = cur;
      while (c != 0) {
        c &= c - 1;
        count++;
      }
    }
    _dirtyCount = count;
    return written;
  }

  int _dirtyCount = 0;
  int get dirtyCount => _dirtyCount;
}
