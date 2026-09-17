// governed_consumer.dart — RFC-0009 Series 7: the composed display
// consumer (fan-out reader + FreshnessGovernor + CadencePolicy + two-frame
// history over the Series-7 buffer recyclers), Dart driver layer.
//
// WHY EXISTS: RFC-0009's two open questions both resolved "composed" — the
// governor never touches a Triad or a ring; the reader's per-consumer
// staleness is the input. This class IS that composition, once per display
// tick, so the app does not hand-roll it per screen:
//
//   ticker ──tick──> CONSUMER ──┬─ claim()          (fan-out reader, zero alloc)
//                               ├─ governor.step()  (staleness CLASS: FastPath /
//                               │                    Skip / Snapshot / Reseed —
//                               │                    advisory; the app decides
//                               │                    what a class MEANS)
//                               ├─ policy.step()    (PRESENTATION: present? interp?
//                               │                    alphaQ12 — the raster decision)
//                               └─ raster           (blend(prev, new, alphaQ12)
//                                                    into a pooled slot — LATEST/
//                                                    BURST copy newest directly)
//
// ZERO-GC PER TICK (Law 2): claim mutates the reader's record;
// governor/policy mutate identity-stable records; the two-frame history is
// two preallocated Uint32Lists; the raster is a pooled slot from
// WeftBufferRecycler (LIVE for the consumer's lifetime — the
// memory-pressure backstop can never take it mid-blend; dispose() returns
// it to the pool). Dart's proof is the identity discipline + counter
// equations (the per-port honesty wall; the JVM byte audit is the Kotlin
// leg's proof).
//
// PACED CONTINUITY (the RFC's construction): on a fresh claim the window
// advances — prevWords := newWords, newWords := reader.view() — and the
// blend at alpha=0 equals prev (the completed previous blend): the raster
// is continuous by construction, never extrapolated past the newest frame
// (saturated alpha holds).
//
// THE GOVERNOR STAYS ADVISORY: tick() returns the policy decision (what
// the display loop needs every frame); the ladder's action is exposed via
// [action] + [actionChanged] for the app's CLASS response. Nothing in
// this class branches on a telemetry counter (AXIOM T).
//
// SINGLE ISOLATE (the Dart port's honesty wall): one consumer per isolate,
// driven by the paint callback on that isolate's event loop.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (Dart VM battery
// green via the plain-dart expect shim; flutter-packages CI is the cover).

import 'dart:typed_data';

import 'fanout.dart';
import 'governor.dart';
import 'recycler.dart';

/// One governed display consumer. Single-isolate by contract; the reader
/// is borrowed, never owned.
class GovernedFanoutConsumer {
  /// Borrowed fan-out reader; claim() is called once per tick().
  final WeftFanoutReader reader;
  /// A [CadencePolicyKind] PROTOCOL value.
  final int policyKind;

  final FreshnessGovernor _governor;
  late final CadencePolicy _policy;

  /// Words per frame (payloadBytes / 4).
  final int words;

  /// The recycler the raster slot came from (null = privately owned).
  late final WeftBufferRecycler? _myPool;

  /// The raster slot (pooled; LIVE for this consumer's lifetime). Packed
  /// u32 words, little-endian — Impeller-safe raw bytes for the painter.
  late final Uint8List raster;

  // --- two-frame history (preallocated; zero alloc per tick) ---
  final Uint32List _prevWords;
  final Uint32List _newWords;

  // --- advisory state (AXIOM T) ---
  /// The ladder's latest action (identity-stable record).
  GovernorAction get action => _governor.act;
  /// True when this tick's ladder action differs from the last (class
  /// change edge — the app's hook for class responses).
  bool actionChanged = false;
  int _lastActionKind = GovernorActionKind.fastPath;

  /// The policy's counters (presents, coalescedByDecision, ...).
  CadencePolicy get cadence => _policy;
  /// The ladder's counters (decidedDrops, reseeds, ...).
  FreshnessGovernor get staleness => _governor;

  /// The ladder's clock (milliseconds). Injectable for deterministic
  /// tests; wall-clock by default.
  int Function() clock = _defaultClock;
  static int _defaultClock() => DateTime.now().millisecondsSinceEpoch;

  GovernedFanoutConsumer(
    this.reader, {
    required this.policyKind,
    WeftBufferRecycler? rasterPool,
    GovernorConfig governorConfig = governorDefaults,
    int reassessTicks = 8,
  })  : _governor = FreshnessGovernor(config: governorConfig),
        words = reader.payloadBytes ~/ 4,
        _prevWords = Uint32List(reader.payloadBytes ~/ 4),
        _newWords = Uint32List(reader.payloadBytes ~/ 4) {
    final bytes = reader.payloadBytes;
    if (rasterPool != null) {
      final slot = rasterPool.acquire();
      if (slot.length < bytes) {
        rasterPool.release(slot);
        throw ArgumentError(
            'raster pool slot too small: ${slot.length} < $bytes');
      }
      raster = slot;
      _myPool = rasterPool;
    } else {
      raster = Uint8List(bytes);
      _myPool = null;
    }
    _rasterOrNull = raster;
    _policy = CadencePolicy(CadenceConfig(policyKind,
        reassessTicks: reassessTicks));
  }

  Uint8List? _rasterOrNull;

  /// One display tick: claim -> ladder -> policy -> raster. Returns the
  /// presentation decision (identity-stable — read synchronously).
  /// Zero allocation.
  PresentDecision tick() {
    final rec = reader.claim(); // zero alloc; mutates the reader's record
    // The ladder: staleness CLASS from this reader's drop accounting
    // (rec.dropped == the frames this consumer missed since its last
    // fresh claim — RFC-0008's per-consumer framesBehind).
    final a = _governor.step(rec.fresh ? rec.dropped : 0, clock());
    actionChanged = a.kind != _lastActionKind;
    _lastActionKind = a.kind;
    // The window: on a fresh claim, prev := new, new := view.
    if (rec.fresh) {
      _prevWords.setAll(0, _newWords);
      _newWords.setAll(0, reader.view());
    }
    // The presentation decision.
    final d = _policy.step(rec.seq);
    // The raster: blend at alpha (PACED) or copy newest (LATEST/BURST).
    if (d.present) {
      if (d.interp) {
        final alpha = d.alphaQ12;
        final inv = cadenceAlphaOneQ12 - alpha;
        final r = raster;
        for (var i = 0; i < words; i++) {
          final packed = _blendQ12(_prevWords[i], _newWords[i], alpha, inv);
          r[4 * i] = packed & 0xFF;
          r[4 * i + 1] = (packed >>> 8) & 0xFF;
          r[4 * i + 2] = (packed >>> 16) & 0xFF;
          r[4 * i + 3] = (packed >>> 24) & 0xFF;
        }
      } else {
        final r = raster;
        for (var i = 0; i < words; i++) {
          final packed = _newWords[i];
          r[4 * i] = packed & 0xFF;
          r[4 * i + 1] = (packed >>> 8) & 0xFF;
          r[4 * i + 2] = (packed >>> 16) & 0xFF;
          r[4 * i + 3] = (packed >>> 24) & 0xFF;
        }
      }
    }
    return d;
  }

  /// Stop the consumer. The raster slot returns to its pool (the next
  /// trim can reclaim it — the consumer is done drawing). Idempotent.
  void dispose() {
    final r = _rasterOrNull;
    if (r == null) return;
    _rasterOrNull = null;
    _myPool?.release(r);
  }

  /// Per-channel u32 blend in Q12 — the raster op (zero alloc).
  static int _blendQ12(int a, int b, int alpha, int inv) {
    final r = ((a & 0xff) * inv + (b & 0xff) * alpha) >>> 12;
    final g = ((a >>> 8 & 0xff) * inv + (b >>> 8 & 0xff) * alpha) >>> 12;
    final bl = ((a >>> 16 & 0xff) * inv + (b >>> 16 & 0xff) * alpha) >>> 12;
    final al = ((a >>> 24 & 0xff) * inv + (b >>> 24 & 0xff) * alpha) >>> 12;
    return r | (g << 8) | (bl << 16) | (al << 24);
  }
}
