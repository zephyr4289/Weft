// fanout_cross_isolate.dart — RFC-0004 fan-out, PRODUCTION cross-isolate
// channel (Flutter / Dart FFI).
//
// WHY EXISTS: the Dart driver port (core/dart/fanout.dart, mirrored at
// lib/src/reference/fanout.dart) is a SINGLE-ISOLATE reference — isolates
// share no memory and Dart has no atomics, so a same-isolate schedule is
// the honest boundary of that file. Cross-isolate fan-out on Flutter is
// real, though: the C ring (core/c/fanout.{h,c}) lives in process-global
// native memory, and dart:ffi handles may cross isolates BY ADDRESS (the
// D-14 FFI lifetime discipline). Until now that path existed only inside
// the test battery (fanout_ffi_test.dart DFT). This module promotes it to
// a production API: one writer isolate (the owner of WeftFanoutFFI) and N
// long-lived reader isolates, each owning exactly one reader handle,
// claim-looping over real OS threads with full protocol accounting.
//
// CONTRACT (single-writer, per-port — unchanged):
//   - begin/fill/publish stay on the isolate that owns the broadcaster.
//   - each reader handle is used by EXACTLY ONE isolate at a time; the
//     owning isolate of the ring keeps LIFETIME control (destroy happens
//     here, after the reader isolate has stopped — the isolate only
//     claims, it never frees; nothing can be freed twice).
//   - readers are bounded and paced (Law 1: no spin): every round claims
//     once per configured cadence and parks paceMs between rounds.
//
// INTEGRITY: every fresh claim may be byte-validated against the canonical
// payload pattern (04-LITMUS §0.1, the pat(seq, i) family — the same
// generator the xlang fixtures and every port's torture use). Violations
// are counted and reported, never silent (Law 1), and never abort the
// reader — the latest-wins stream continues.
//
// ACCOUNTING: reads/fresh/drops/skipped/torn come from the C reader's
// authoritative counters (weft_fanout_reader_stats) at every snapshot;
// the claim loop only adds the violation log (the correctness surface).
//
// STATUS: CI-PROVEN on host (fanout_ffi_test.dart exercises this module
// over real isolates); on-device verification pending (same declaration
// as every flutter_weft FFI change).

import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import 'bindings.dart';
import 'fanout_ffi.dart';

/// Reader-isolate configuration. All fields are primitives — the config
/// crosses isolates by value.
class CrossIsolateReaderConfig {
  /// Claim every Nth pacing round (1 = every round; 4 = quarter-rate
  /// consumer, the flight-recorder cadence).
  final int tickEvery;

  /// Park between pacing rounds (Law 1: no spin). 0 = tight bounded loop
  /// (torture shape); real background consumers should pace (1-8 ms).
  final int paceMs;

  /// Byte-validate every fresh claim against the canonical pat(seq, i)
  /// pattern (04-LITMUS §0.1) at this byte stride. 0 disables validation.
  final int verifyStride;

  /// Starvation guard: the reader stops (with a violation) after this many
  /// claims — bounded, never an unbounded loop.
  final int maxClaims;

  const CrossIsolateReaderConfig({
    this.tickEvery = 1,
    this.paceMs = 0,
    this.verifyStride = 8,
    this.maxClaims = 50000000,
  });
}

/// Immutable snapshot of a reader isolate's protocol accounting. The
/// counters are the C reader's authoritative numbers (AXIOM T: advisory);
/// the [violations] list is the correctness surface.
class CrossIsolateReaderStats {
  final int reads;
  final int fresh;
  final int drops;
  final int skippedMidOverwrite;
  final int tornExhausted;
  final int tornAccepted;
  final int lastSeq;
  final List<String> violations;

  const CrossIsolateReaderStats({
    required this.reads,
    required this.fresh,
    required this.drops,
    required this.skippedMidOverwrite,
    required this.tornExhausted,
    required this.tornAccepted,
    required this.lastSeq,
    required this.violations,
  });

  bool get ok => violations.isEmpty;

  Map<String, Object> toMap() => {
        'reads': reads,
        'fresh': fresh,
        'drops': drops,
        'skippedMidOverwrite': skippedMidOverwrite,
        'tornExhausted': tornExhausted,
        'tornAccepted': tornAccepted,
        'lastSeq': lastSeq,
        'violations': violations,
      };

  static CrossIsolateReaderStats fromMap(Map<Object?, Object?> m) =>
      CrossIsolateReaderStats(
        reads: m['reads'] as int? ?? 0,
        fresh: m['fresh'] as int? ?? 0,
        drops: m['drops'] as int? ?? 0,
        skippedMidOverwrite: m['skippedMidOverwrite'] as int? ?? 0,
        tornExhausted: m['tornExhausted'] as int? ?? 0,
        tornAccepted: m['tornAccepted'] as int? ?? 0,
        lastSeq: m['lastSeq'] as int? ?? 0,
        violations:
            (m['violations'] as List<Object?>?)?.cast<String>() ?? const [],
      );
}

/// One spawned reader isolate. Lifetime stays with the session (destroy
/// happens in the owning isolate AFTER this isolate has stopped).
class ReaderIsolate {
  final int id;
  final Isolate isolate;
  final SendPort _control;
  final CrossIsolateFanoutSession _session;
  Future<CrossIsolateReaderStats>? _stopped;

  ReaderIsolate._(
      this.id, this.isolate, this._control, this._session);

  /// Request a mid-flight stats snapshot; resolves when the isolate
  /// replies. Useful for eval rigs and observability dashboards.
  Future<CrossIsolateReaderStats> requestStats() {
    final c = Completer<CrossIsolateReaderStats>();
    _session._pendingStats[id] = c;
    _control.send({'cmd': 'stats'});
    return c.future;
  }

  /// Ask the isolate to drain and exit. Resolves with the FINAL stats
  /// snapshot after the isolate confirms ('stopped'). The reader handle
  /// itself is destroyed by the session afterwards (lifetime discipline).
  Future<CrossIsolateReaderStats> stop() {
    return _stopped ??= () {
      final c = Completer<CrossIsolateReaderStats>();
      _session._pendingStops[id] = c;
      _control.send({'cmd': 'stop'});
      return c.future;
    }();
  }
}

/// A cross-isolate fan-out session: one writer (this isolate, owning the
/// [WeftFanoutFFI] ring) + N spawned reader isolates. Spawn readers, drive
/// the writer, then [stopAll] (drain -> final stats -> destroy handles in
/// the owning isolate -> join isolates).
class CrossIsolateFanoutSession {
  final WeftFanoutFFI broadcaster;

  /// Optional explicit library path for reader isolates to reopen. null =
  /// each isolate resolves via [WeftNativeBindings.openLibrary] (platform
  /// defaults). Tests pass the search-resolved path explicitly.
  final String? soPath;

  final ReceivePort _events = ReceivePort();
  final Map<int, Completer<CrossIsolateReaderStats>> _pendingStops = {};
  final Map<int, Completer<CrossIsolateReaderStats>> _pendingStats = {};
  final Map<int, ReaderIsolate> _readers = {};
  final Map<int, WeftFanoutReaderFFI> _handles = {};
  final Map<int, Completer<ReaderIsolate>> _ready = {};
  final Map<int, SendPort> _controls = {};
  final Map<int, CrossIsolateReaderStats> _lastStats = {};
  int _nextId = 0;
  bool _disposed = false;

  CrossIsolateFanoutSession({required this.broadcaster, this.soPath}) {
    _events.listen(_onEvent);
  }

  /// Spawn one reader isolate over a fresh reader handle on the ring.
  /// Resolves after the isolate reports 'ready'. The waiter is registered
  /// BEFORE the spawn so the 'ready' message can never outrun it; the
  /// reader wrapper stays HERE (session-owned lifetime — the isolate gets
  /// only the raw address and may only claim through it).
  Future<ReaderIsolate> spawnReader({
    CrossIsolateReaderConfig config = const CrossIsolateReaderConfig(),
  }) {
    if (_disposed) throw StateError('session disposed');
    final reader = broadcaster.createReader();
    final id = _nextId++;
    _handles[id] = reader;
    final ready = Completer<ReaderIsolate>();
    _ready[id] = ready;
    final cfg = _IsoConfig(
      readerAddress: reader.handleAddress,
      payloadBytes: broadcaster.payloadBytes,
      slotCount: broadcaster.slotCount,
      tickEvery: config.tickEvery,
      paceMs: config.paceMs,
      verifyStride: config.verifyStride,
      maxClaims: config.maxClaims,
      soPath: soPath,
      events: _events.sendPort,
      id: id,
    );
    unawaited(Isolate.spawn(_readerIsolateMain, cfg));
    return ready.future;
  }

  /// Latest known stats for a reader (mid-flight snapshot or final).
  CrossIsolateReaderStats? statsOf(int id) => _lastStats[id];

  void _onEvent(Object? msg) {
    if (msg is! Map) return;
    final id = msg['id'] as int? ?? -1;
    switch (msg['event']) {
      case 'ready':
        final waiter = _ready.remove(id);
        if (waiter != null) {
          final control = msg['control'] as SendPort;
          _controls[id] = control;
          final ri = ReaderIsolate._(
              id, msg['isolate'] as Isolate, control, this);
          _readers[id] = ri;
          waiter.complete(ri);
        }
      case 'stats':
        final s = CrossIsolateReaderStats.fromMap(msg);
        _lastStats[id] = s;
        _pendingStats.remove(id)?.complete(s);
      case 'stopped':
        final s = CrossIsolateReaderStats.fromMap(msg);
        _lastStats[id] = s;
        _pendingStops.remove(id)?.complete(s);
    }
  }

  /// Drain every reader isolate, destroy their handles HERE (lifetime
  /// discipline), and kill the isolates. Returns final stats per id.
  Future<Map<int, CrossIsolateReaderStats>> stopAll() async {
    final finals = <int, CrossIsolateReaderStats>{};
    for (final entry in _readers.entries) {
      finals[entry.key] = await entry.value.stop();
    }
    for (final h in _handles.values) {
      h.destroy();
    }
    _handles.clear();
    for (final r in _readers.values) {
      r.isolate.kill(priority: Isolate.beforeNextEvent);
    }
    _readers.clear();
    _disposed = true;
    _events.close();
    return finals;
  }
}

// ---------------------------------------------------------------------------
// Reader-isolate side (top-level functions only — spawnable).
// ---------------------------------------------------------------------------

class _IsoConfig {
  final int readerAddress;
  final int payloadBytes;
  final int slotCount;
  final int tickEvery;
  final int paceMs;
  final int verifyStride;
  final int maxClaims;
  final String? soPath;
  final SendPort events;
  final int id;
  const _IsoConfig({
    required this.readerAddress,
    required this.payloadBytes,
    required this.slotCount,
    required this.tickEvery,
    required this.paceMs,
    required this.verifyStride,
    required this.maxClaims,
    required this.soPath,
    required this.events,
    required this.id,
  });
}

void _readerIsolateMain(_IsoConfig cfg) async {
  final control = ReceivePort();
  // Announce readiness: the control port AND the current Isolate handle
  // (so the session never races its own spawn future against this
  // message). Lifetime stays with the session — we only ever claim.
  cfg.events.send({
    'event': 'ready',
    'id': cfg.id,
    'control': control.sendPort,
    'isolate': Isolate.current,
  });

  var running = true;
  var statsRequested = false;
  control.listen((msg) {
    if (msg is! Map) return;
    switch (msg['cmd']) {
      case 'stop':
        running = false;
      case 'stats':
        statsRequested = true;
    }
  });

  // Re-open the library IN this isolate (handles cannot cross; the .so is
  // process-global — a second open is refcounted, not a second copy).
  final DynamicLibrary dylib;
  if (cfg.soPath != null) {
    dylib = DynamicLibrary.open(cfg.soPath!);
  } else {
    dylib = WeftNativeBindings.openLibrary();
  }
  final bindings = WeftNativeBindings(dylib);
  final handle = Pointer<Void>.fromAddress(cfg.readerAddress);
  final claim = bindings.fanoutClaim;
  final view = bindings.fanoutView;
  final readerStats = bindings.fanoutReaderStats;

  var tornAccepted = 0;
  var lastSeq = 0;
  var claims = 0;
  final violations = <String>[];

  /// Authoritative counters straight from the C reader (the claim loop
  /// never maintains its own tallies — one source of truth).
  CrossIsolateReaderStats snapshot() {
    final out = calloc<FanoutReaderStatsStruct>();
    try {
      readerStats(handle, out);
      return CrossIsolateReaderStats(
        reads: out.ref.reads,
        fresh: out.ref.fresh,
        drops: out.ref.drops,
        skippedMidOverwrite: out.ref.skippedMidOverwrite,
        tornExhausted: out.ref.tornExhausted,
        tornAccepted: tornAccepted,
        lastSeq: lastSeq,
        violations: List.of(violations),
      );
    } finally {
      calloc.free(out);
    }
  }

  // ASYNC ROUND LOOP — load-bearing: every round awaits once so the event
  // loop can deliver control messages (stop/stats) and service the VM's
  // scheduler. A synchronous spin here would starve the very port the
  // stop-protocol rides on (the DFT torture could bound its loop by frame
  // count; a production reader cannot — it must yield). paceMs > 0 parks
  // the timer instead of the CPU (Law 1: no spin).
  var round = 0;
  while (running) {
    round++;
    if (cfg.paceMs > 0) {
      await Future<void>.delayed(Duration(milliseconds: cfg.paceMs));
    } else {
      await Future<void>.delayed(Duration.zero);
    }
    if (statsRequested) {
      statsRequested = false;
      cfg.events.send({'event': 'stats', 'id': cfg.id, ...snapshot().toMap()});
    }
    if (round % cfg.tickEvery != 0) continue;
    final rec = claim(handle);
    claims++;
    if (rec.ref.fresh != 0) {
      final seq = rec.ref.seq;
      if (cfg.verifyStride > 0) {
        final v = view(handle);
        for (var i = 0; i < cfg.payloadBytes; i += cfg.verifyStride) {
          if (v[i] != _pat(seq, i)) {
            tornAccepted++;
            violations.add('torn/corrupt frame accepted at seq $seq');
            break;
          }
        }
      }
      if (lastSeq > 0 && seq <= lastSeq) {
        violations.add('non-monotonic seq $seq after $lastSeq');
      }
      lastSeq = seq;
    }
    if (claims >= cfg.maxClaims) {
      violations.add('starvation guard hit at $claims claims (last=$lastSeq)');
      break;
    }
  }

  cfg.events.send({'event': 'stopped', 'id': cfg.id, ...snapshot().toMap()});
  control.close();
}

// The canonical payload pattern (04-LITMUS §0.1) — independent Dart
// reimplementation, the same discipline as every port's battery.
int _mix32(int x) {
  var v = x & 0xFFFFFFFF;
  v = (v ^ (v >>> 16)) & 0xFFFFFFFF;
  v = (v * 0x7FEB352D) & 0xFFFFFFFF;
  v = (v ^ (v >>> 15)) & 0xFFFFFFFF;
  v = (v * 0x846CA68B) & 0xFFFFFFFF;
  v = (v ^ (v >>> 16)) & 0xFFFFFFFF;
  return v;
}

int _pat(int seq, int i) {
  final x = (seq * 2654435761 + i * 2246822519) & 0xFFFFFFFF;
  return _mix32(x) & 0xFF;
}
