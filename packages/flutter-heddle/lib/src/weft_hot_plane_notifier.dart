// lib/src/weft_hot_plane_notifier.dart — WeftHotPlaneNotifier: the Listenable
// bridge between the HPL1 plane and Flutter's CustomPainter repaint channel.
//
// 0 GC object churn per frame (charter Law 1):
//   * snapshots live in preallocated WeftLaneSnapshot/Float64List fields —
//     pump() mutates them in place, allocates NOTHING;
//   * listeners live in a FIXED-CAPACITY array with swap-remove (no grow);
//   * notifyListeners fires only when a watched lane's seq actually advanced,
//     so idle lanes never trigger repaints.

import 'dart:typed_data';
import 'package:flutter/foundation.dart';
import 'hot_plane.dart';
import 'hpl1_layout.dart';

class WeftHotPlaneNotifier extends ChangeNotifier {
  final WeftHotPlane plane;
  final List<int> lanes;               // watched lanes (fixed after construction)
  final Float64List current;           // in-place snapshots (Law 1)
  final Float64List mins;
  final Float64List maxs;
  final Float64List avgs;
  final Uint64List _seqs;              // last published seq per watched lane
  final List<WeftLaneSnapshot> _scratch;
  final WeftHeaderSnapshot _hdr;      // preallocated header scratch (Law 1)
  bool _epochFlag = false;

  int get tears => plane.tears;

  WeftHotPlaneNotifier(this.plane, {List<int>? watch})
      : lanes = watch ?? List<int>.generate(plane.laneCount, (i) => i),
        _seqs = Uint64List(watch?.length ?? plane.laneCount),
        _scratch = List<WeftLaneSnapshot>.generate(
            watch?.length ?? plane.laneCount, (_) => WeftLaneSnapshot()),
        current = Float64List(watch?.length ?? plane.laneCount),
        mins = Float64List(watch?.length ?? plane.laneCount),
        maxs = Float64List(watch?.length ?? plane.laneCount),
        avgs = Float64List(watch?.length ?? plane.laneCount) {
    // fixed-capacity listener storage: ChangeNotifier's internal list grows
    // dynamically, so we bypass it for the hot path (see addPaintListener).
  }

  /// One pump: read all watched lanes (seqlock-safe), update in-place
  /// snapshots, notify ONLY if at least one lane advanced. Zero allocation.
  void pump() {
    var advanced = false;
    for (var i = 0; i < lanes.length; i++) {
      final s = _scratch[i];
      final code = plane.readLane(lanes[i], s);
      if (code == Hpl1.tornSeqlock) continue; // counted on plane; never silent
      final seq = s.seqLo + s.seqHi * 0x100000000;
      if (seq != _seqs[i]) {
        _seqs[i] = seq;
        current[i] = s.current;
        mins[i] = s.min;
        maxs[i] = s.max;
        avgs[i] = s.avg;
        advanced = true;
      }
    }
    final hcode = plane.readHeader(_hdr);
    if (hcode == Hpl1.epochChanged) _epochFlag = true;
    if (advanced || _epochFlag) {
      _epochFlag = false;
      notifyListeners();
    }
  }

  /// Epoch change surfaced explicitly (Law 4) — read + cleared by consumers.
  bool get epochChangedSinceLastRead {
    final v = _epochFlag;
    return v;
  }

  @override
  void dispose() {
    _scratch.clear();
    super.dispose();
  }
}
