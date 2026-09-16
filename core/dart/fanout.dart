// fanout.dart — RFC 0004: Multi-Consumer Fan-Out ring, Dart driver layer
//
// WHY EXISTS: RFC 0004 (Accepted as driver-layer pattern, round-6 §4). The
// Triad kernel is 1-writer/1-reader by design; applications that need a
// primary canvas, a minimap, a flight recorder, and a network visualizer on
// one stream cannot bind N readers to one Triad. The TS port ships the ring
// over a SharedArrayBuffer (core/ts/fanout.ts) and Series 4 brought
// byte-compatible rings to C and Rust; Series 5 brought it to Kotlin and
// Swift — this module completes the six-port fan-out story for Dart with
// the SAME BYTE-COMPATIBLE LAYOUT, so the ring bytes interop with every
// other port through dart:ffi (a Pointer<Uint8>.asTypedList(...) view of a
// C-produced ring attaches via WeftFanoutReader.fromBytes).
//
// SINGLE-ISOLATE REFERENCE — the honesty load-bearing wall of this port,
// exactly like its kernel (core/dart/weft.dart, WO-P4 decision 4): Dart
// isolates share no memory and the language has no atomics. All ctrl and
// payload accesses are PLAIN ByteData reads/writes — valid only because
// writer and readers run on ONE event loop (e.g., a Worker isolate that
// owns both, or a same-isolate multi-consumer pipeline). No cross-thread
// ordering claims transfer from the C/Rust/Kotlin proofs; production
// cross-thread fan-out on Flutter goes through dart:ffi to the C ring
// (core/c/fanout.h) — same bytes, real atomics.
//
// RING LAYOUT (byte-identical to core/ts/fanout.ts and core/c/fanout.h):
//   byte 0              latestSeq   i64  0 = no frame yet; frames from 1
//   byte 8              publishes   i64  telemetry (one add per publish)
//   byte 16 + 8k        slotSeq[k]  i64  0 = INVALIDATED (fill in progress)
//   byte 16 + 8M        payload     M slots x payload_bytes (slot k at +k*payload_bytes)
// payload_bytes MUST be a multiple of 4 (u32 word granularity).
// ring_bytes = 16 + 8M + M*payload_bytes — identical formula in all ports.
// Byte order: LITTLE_ENDIAN everywhere (Endian.little on every accessor —
// the wire contract).
//
// PROTOCOL (RFC 0004 §Reference-level specification — same as every port):
//   Writer (single, by contract):
//     begin():   wSeq += 1; k = (wSeq-1) mod M;
//                slotSeq[k] <- 0  (invalidate BEFORE the fill — the FI1 bracket)
//                return slot payload view (ByteData slice)
//     publish(): slotSeq[k] <- wSeq; latestSeq <- wSeq; publishes += 1
//   Reader (N, independent; each owns its pre-allocated copy buffer — Law 2):
//     claim():   L = latestSeq; if L == 0 or L == lastSeq: not fresh
//                else bounded (<= 4 attempts):
//                  k = (L-1) mod M; sB = slotSeq[k]
//                  if sB != L: re-read latestSeq; unchanged -> SKIP this tick
//                    (Law 1: no spin; counted, never silent); changed -> chase
//                  copy slot k -> reader buffer; sA = slotSeq[k]
//                  if sA == L: consistent frame L; dropped = L - lastSeq - 1;
//                    advance lastSeq (FI2: per-slot stamp monotonicity)
//                  else: torn copy; retry on the newest completed frame
//                attempts exhausted: not fresh, counted, never a spin
//   (Single-isolate note: the revalidation CAN still observe a changed stamp
//   mid-copy — an await between the copy halves is legal Dart — which is
//   why the bracket discipline is retained verbatim instead of elided: the
//   protocol shape stays identical to the concurrent ports, and a
//   same-isolate producer/consumer pipeline interleaved by awaits keeps the
//   tear freedom.)
//
// LAW 2: begin/publish/claim allocate nothing on the hot path (the payload
//        views are ByteData sublistViews over the same bytes — views, not
//        copies; constructors may allocate).
// LAW 1: every path is bounded; a skip or exhausted retry is counted in
//        reader stats, never silent, never a spin.
// LAW 4: honest boundaries — dropped = L - lastSeq - 1 assumes the
//        single-writer monotonic contract; multi-canvas renders are
//        approximately synchronized (latest-wins per consumer); the payload
//        contract is byte-granular u32 words (float users go through
//        wordToFloat(view()[i]) — the bit-pattern reinterpretation).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import 'dart:typed_data';

/// Maximum ring depth. RFC 0004 recommends 4-8; the bound matches the C
/// port's WEFT_FANOUT_MAX_SLOTS so geometry validates identically.
const int weftFanoutMaxSlots = 64;

/// Bounded claim attempts (same constant and rationale as every port).
const int weftFanoutMaxClaimAttempts = 4;

/// Total ring size in bytes for the given geometry (the interop contract —
/// identical to core/ts/fanout.ts and core/c/fanout.c). 0 on bad geometry.
int weftFanoutRingBytes(int payloadBytes, int slotCount) {
  if (payloadBytes <= 0 || payloadBytes % 4 != 0) return 0;
  if (slotCount < 2 || slotCount > weftFanoutMaxSlots) return 0;
  return 16 + 8 * slotCount + slotCount * payloadBytes;
}

/// Claim result record — reader-owned and identity-stable, mutated in place
/// per claim so the hot path allocates nothing (Law 2). Read the fields
/// synchronously after claim(); do not retain the record across claims
/// expecting a snapshot.
class FanoutClaim {
  /// A new consistent frame was claimed this tick.
  bool fresh;

  /// Frame seq now held (last consistent if !fresh).
  int seq;

  /// Frames completed without this reader ever observing them.
  int dropped;

  FanoutClaim({this.fresh = false, this.seq = 0, this.dropped = 0});
}

/// Advisory reader statistics (AXIOM T: advisory, never a correctness
/// reference). Field names mirror the TS/C/Kotlin/Swift ports.
class FanoutReaderStats {
  final int reads;
  final int fresh;
  final int drops;
  final int skippedMidOverwrite;
  final int tornExhausted;

  const FanoutReaderStats(this.reads, this.fresh, this.drops,
      this.skippedMidOverwrite, this.tornExhausted);
}

/// Advisory broadcaster state (cold path; AXIOM T applies).
class FanoutDebugStats {
  final int latestSeq;
  final int publishes;
  final int slotCount;
  final int payloadBytes;
  final List<int> slotStamps;

  const FanoutDebugStats(this.latestSeq, this.publishes, this.slotCount,
      this.payloadBytes, this.slotStamps);
}

// ---------------------------------------------------------------------------
// The fan-out ring: one writer + N readers, M pre-allocated slots, one
// publication point (latestSeq). SINGLE-ISOLATE by contract (see header).
// ---------------------------------------------------------------------------

class WeftFanoutBroadcaster {
  /// Per-slot payload capacity in bytes (immutable after init; multiple of 4).
  final int payloadBytes;

  /// Ring depth (immutable after init). RFC 0004 recommends 4-8.
  final int slotCount;

  /// The ring bytes — one contiguous LITTLE_ENDIAN region. Hand a copy (or
  /// an FFI view) to any port; the bytes are the interop contract, exactly
  /// like posting the SAB in the TS port.
  late final Uint8List _bytes;

  /// LE accessor over the ring bytes (the only accessor used — wire order).
  late final ByteData _view;

  /// Writer-private frame counter (0 = no begin yet) and slot of the current
  /// begin() — single writer by contract, the kernel's discipline.
  int _wSeq = 0;
  int _wSlot = 0;
  bool _begun = false;

  /// Allocate a fan-out ring (init may allocate — Law 2 applies to
  /// begin/publish/claim). Zero-initialized ctrl: latestSeq=0 (no frame
  /// yet), publishes=0, every slotSeq=0 (all invalidated) — the same
  /// invariants a fresh SAB gives the TS port.
  WeftFanoutBroadcaster(this.payloadBytes, [this.slotCount = 4]) {
    if (payloadBytes <= 0 || payloadBytes % 4 != 0) {
      throw ArgumentError(
          'payloadBytes must be a positive multiple of 4 (got $payloadBytes)');
    }
    if (slotCount < 2 || slotCount > weftFanoutMaxSlots) {
      throw ArgumentError(
          'slotCount must be in [2, $weftFanoutMaxSlots] (got $slotCount)');
    }
    _bytes = Uint8List(weftFanoutRingBytes(payloadBytes, slotCount));
    _view = ByteData.view(_bytes.buffer, 0, _bytes.length);
  }

  int _payloadBase() => 16 + 8 * slotCount;

  int _slotBase(int k) => _payloadBase() + k * payloadBytes;

  /// Begin the next frame: bumps the frame counter, INVALIDATES the target
  /// slot's stamp (BEFORE the fill — the FI1 bracket) and returns the slot's
  /// payload as a ByteData sublistView (zero copy — a view, not a snapshot;
  /// valid until the next begin()). Zero allocation.
  ByteData begin() {
    _wSeq += 1;
    _wSlot = (_wSeq - 1) % slotCount;
    _begun = true;
    _view.setUint64(16 + 8 * _wSlot, 0, Endian.little);
    final base = _slotBase(_wSlot);
    return ByteData.sublistView(_bytes, base, base + payloadBytes);
  }

  /// Publish the begun frame: stamp the slot, flip latestSeq (the
  /// publication point), bump publishes (advisory). Returns the published
  /// frame seq, or 0 if no begin() ever ran (a detectable no-op, not an
  /// error — same as every port). Zero allocation.
  int publish() {
    if (_wSeq == 0) return 0;
    _view.setUint64(16 + 8 * _wSlot, _wSeq, Endian.little);
    _view.setUint64(0, _wSeq, Endian.little);
    _view.setUint64(8, _view.getUint64(8, Endian.little) + 1, Endian.little);
    return _wSeq;
  }

  /// Create a reader bound to this ring (same isolate — single-isolate
  /// contract, see the header honesty note).
  WeftFanoutReader createReader() => WeftFanoutReader(_bytes, payloadBytes, slotCount);

  /// The raw ring bytes (the interop contract — copy them across ports).
  Uint8List ringBytes() => _bytes;

  /// Advisory state snapshot (cold path — allocates; never call per frame).
  FanoutDebugStats debugStats() {
    final stamps = List<int>.generate(
        slotCount, (k) => _view.getUint64(16 + 8 * k, Endian.little));
    return FanoutDebugStats(
      _view.getUint64(0, Endian.little),
      _view.getUint64(8, Endian.little),
      slotCount,
      payloadBytes,
      stamps,
    );
  }

  /// Whether a begin() has run (test/inspection aid; not on the hot path).
  bool get hasBegun => _begun;
}

// ---------------------------------------------------------------------------
// Reader — the consumer side. N per ring, each fully independent.
// ---------------------------------------------------------------------------

class WeftFanoutReader {
  /// Per-slot payload capacity in bytes (validated against the backing bytes).
  final int payloadBytes;

  /// Ring depth (validated against the backing bytes).
  final int slotCount;

  final Uint8List _bytes;
  final ByteData _view;

  /// The reader's own pre-allocated copy buffer — u32 words
  /// (payloadBytes/4), stable identity for the consumer's lifetime; holds
  /// frame data only after a fresh claim (Law 2).
  late final Uint32List _target;

  /// Last frame seq this reader has held consistent (0 = none yet).
  int _lastSeq = 0;

  /// Preallocated, identity-stable claim record (mutated per claim).
  final FanoutClaim _rec = FanoutClaim();

  /// 4-byte scratch for wordToFloat (zero allocation per call).
  final ByteData _scratch = ByteData(4);

  // Reader-private statistics (advisory; exposed via stats()).
  int _nReads = 0;
  int _nFresh = 0;
  int _nDrops = 0;
  int _nSkip = 0;
  int _nExhausted = 0;

  /// Attach a reader to a ring's bytes. Works with a broadcaster's ring, a
  /// copy from another port, or an FFI-backed view
  /// (Pointer<Uint8>.asTypedList(n)) of a C-produced ring — geometry is
  /// validated against the byte length so a mismatched pair fails fast
  /// instead of tearing. SINGLE-ISOLATE: same event loop as the writer.
  WeftFanoutReader(Uint8List bytes, this.payloadBytes, [this.slotCount = 4])
      : _bytes = bytes,
        _view = ByteData.view(bytes.buffer, bytes.offsetInBytes, bytes.length) {
    if (payloadBytes <= 0 || payloadBytes % 4 != 0) {
      throw ArgumentError(
          'payloadBytes must be a positive multiple of 4 (got $payloadBytes)');
    }
    if (slotCount < 2 || slotCount > weftFanoutMaxSlots) {
      throw ArgumentError(
          'slotCount must be in [2, $weftFanoutMaxSlots] (got $slotCount)');
    }
    final expect = weftFanoutRingBytes(payloadBytes, slotCount);
    if (bytes.length != expect) {
      throw ArgumentError('ring geometry mismatch: expected $expect bytes '
          'for $slotCount slots x $payloadBytes bytes, got ${bytes.length}');
    }
    _target = Uint32List(payloadBytes ~/ 4);
  }

  /// Convenience: attach from a broadcaster directly.
  WeftFanoutReader.fromBroadcaster(WeftFanoutBroadcaster b)
      : this(b.ringBytes(), b.payloadBytes, b.slotCount);

  int _payloadBase() => 16 + 8 * slotCount;

  /// Claim the freshest completed frame into this reader's buffer. Never
  /// blocks, never spins unboundedly, never fails: a tick on which no
  /// consistent newer frame is available returns fresh=false and the reader
  /// keeps its last consistent frame. Returns the reader-owned claim record
  /// (identity-stable, mutated in place — zero allocation per claim).
  ///
  /// `dropped` counts frames that completed without this reader ever
  /// observing them (RFC 0004 per-reader drop accounting). The telescoping
  /// identity sum(dropped) == lastSeq - freshClaims holds exactly.
  FanoutClaim claim() {
    _nReads++;
    var L = _view.getUint64(0, Endian.little);
    if (L == 0 || L == _lastSeq) {
      _rec.fresh = false;
      _rec.seq = _lastSeq;
      _rec.dropped = 0;
      return _rec;
    }
    for (var attempt = 0; attempt < weftFanoutMaxClaimAttempts; attempt++) {
      final k = (L - 1) % slotCount;
      final sB = _view.getUint64(16 + 8 * k, Endian.little);
      if (sB != L) {
        // Slot mid-overwrite (stamp 0) or already re-stamped by a newer
        // frame. Re-read latestSeq: unchanged -> graceful skip (Law 1);
        // changed -> a newer frame completed, chase it.
        final L2 = _view.getUint64(0, Endian.little);
        if (L2 == L) {
          _nSkip++;
          _rec.fresh = false;
          _rec.seq = _lastSeq;
          _rec.dropped = 0;
          return _rec;
        }
        L = L2;
        continue;
      }
      // Stamp matches frame L: copy, then re-validate (the bracket retained
      // verbatim — see the single-isolate note in the header). The copy is
      // an explicit LE word loop: word VALUES, not byte reinterpretation,
      // so the contract holds on any host endianness.
      final base = _payloadBase() + k * payloadBytes;
      for (var w = 0; w < _target.length; w++) {
        _target[w] = _view.getUint32(base + 4 * w, Endian.little);
      }
      final sA = _view.getUint64(16 + 8 * k, Endian.little);
      if (sA == L) {
        // Consistent frame L (FI2: unchanged stamp proves no overwrite
        // began during the copy).
        final dropped = L - _lastSeq - 1;
        _nDrops += dropped;
        _lastSeq = L;
        _nFresh++;
        _rec.fresh = true;
        _rec.seq = _lastSeq;
        _rec.dropped = dropped;
        return _rec;
      }
      // Torn copy detected — retry on the newest completed frame.
      L = _view.getUint64(0, Endian.little);
    }
    // Bounded retries exhausted: keep the last consistent frame. Counted,
    // never silent, never a spin (Law 1).
    _nExhausted++;
    _rec.fresh = false;
    _rec.seq = _lastSeq;
    _rec.dropped = 0;
    return _rec;
  }

  /// The reader's pre-allocated copy buffer as u32 words (stable identity).
  /// Meaningful after a fresh claim; read it live before the next claim —
  /// the same discipline as the kernel's rLive (A3). Float users:
  /// wordToFloat(view()[i]) — the TS port stores Float32 values; read as LE
  /// u32 words they are the bit patterns.
  Uint32List view() => _target;

  /// Zero-allocation u32-word -> float reinterpretation (one 4-byte scratch,
  /// reader-owned — the Dart analog of Kotlin's Float.fromBits).
  double wordToFloat(int word) {
    _scratch.setUint32(0, word, Endian.little);
    return _scratch.getFloat32(0, Endian.little);
  }

  /// Advisory statistics snapshot (cold path; AXIOM T: advisory, never a
  /// correctness reference).
  FanoutReaderStats stats() => FanoutReaderStats(
      _nReads, _nFresh, _nDrops, _nSkip, _nExhausted);
}
