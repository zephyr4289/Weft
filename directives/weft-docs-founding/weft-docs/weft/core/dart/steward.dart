// steward.dart — Lifecycle manager for Wefts (Dart/Flutter)
//
// WHY EXISTS: Manages the lifetime of Weft instances — allocates, binds to
// scope, frees on scope exit. Per 02-KERNEL §7. StatefulWidget-scoped.
// SINGLE-ISOLATE: same constraints as weft.dart.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
// SINGLE-ISOLATE REFERENCE: no cross-thread ordering claims.

import 'weft.dart';

class Steward {
  final Map<int, Weft> _wefts = {};
  int _nextId = 1;
  bool _released = false;

  Weft weft(int payloadMax) {
    assert(!_released, 'Steward is released');
    final w = Weft(payloadMax);
    _wefts[_nextId++] = w;
    return w;
  }

  void releaseAll() {
    if (_released) return;
    _released = true;
    _wefts.clear(); // Dart GC handles the buffers
  }

  StewardStats stats() {
    var totalPub = 0, totalRead = 0;
    for (final w in _wefts.values) {
      totalPub += w.tPublishCount;
      totalRead += w.tClaimCount;
    }
    return StewardStats(_wefts.length, totalPub, totalRead);
  }

  List<String> dumpLeaks() {
    return _wefts.keys.map((id) => 'Weft $id still bound').toList();
  }
}

class StewardStats {
  final int weftCount;
  final int totalPublishes;
  final int totalReads;
  StewardStats(this.weftCount, this.totalPublishes, this.totalReads);
}
