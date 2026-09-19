// weft.dart — Triad Protocol kernel (Dart/Flutter reference port)
//
// WHY EXISTS: Implements the Triad Protocol as a SINGLE-ISOLATE REFERENCE
// implementation. Dart isolates share no memory and the language has no
// atomics. The exchange maps to plain field assignment, valid only because
// writer and reader run on one event loop. Per WO-P4 decision 4 and
// docs/PORTS.md §3. Production Flutter usage goes through dart:ffi to the
// C kernel — specified, NOT implemented.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
// SINGLE-ISOLATE REFERENCE: no cross-thread ordering claims transfer from
// the C/Rust proof. This is the honesty load-bearing wall of the Dart port.

import 'dart:typed_data';

/// Magic "WEFT" little-endian: 0x54464557
const int weftMagic = 0x54464557;
const int weftVersion1 = 1;

/// Upper bound for payload_max (1 MiB) — the TIER4 §5 validation wall
/// (issue #19). Mirrors WEFT_PAYLOAD_MAX_LIMIT (core/c/weft.h).
const int weftPayloadMaxLimit = 1 << 20;

/// Publish result (02 §4).
enum PubResult { ok, droppedRevoked, invalid }

/// Decode result (03-ENVELOPE §2).
enum DecodeResult { ok, short, badMagic, badHeader }

/// A single Weft: three buffers + the single shared `latest`.
///
/// SINGLE-ISOLATE: `latest` is a plain `int`, not an atomic. The exchange
/// (`latest = wWork`) is a plain assignment — valid only because writer and
/// reader run on one event loop. No cross-thread ordering claims.
class Weft {
  final int payloadMax;
  final int bufSize;

  /// Three buffers (ByteData for LE access).
  late final List<ByteData> _buffers;

  // The single shared index. PLAIN FIELD — single-isolate only.
  int _latest = 0;
  int _wWork = 1;
  int _rWork = 2;

  // I6 (plain — single isolate)
  bool _revoked = false;
  int _epoch = 0;

  // Telemetry (advisory per AXIOM T)
  int _tPublish = 0;
  int _tClaim = 0;
  int _tDrop = 0;
  int _tInvalid = 0;

    // init: constructor serves as init for API parity with C kernel.
  Weft(this.payloadMax) : bufSize = ((16 + payloadMax + 8 + 63) ~/ 64) * 64 {
    // TIER4 §5 validation wall (issue #19): fail fast on programmer error —
    // the C kernel's -1 refusal maps to a thrown ArgumentError in Dart.
    if (payloadMax <= 0 || payloadMax > weftPayloadMaxLimit) {
      throw ArgumentError.value(payloadMax, 'payloadMax',
          'must be in [1, $weftPayloadMaxLimit] (TIER4 §5)');
    }
    _buffers = List.generate(3, (_) => ByteData(bufSize));
    // Per 04-LITMUS §0.6: null frame with pat(0,i) payload.
    for (var i = 0; i < 3; i++) {
      _envelopeEncodeV1(_buffers[i], 0, payloadMax);
      for (var j = 0; j < payloadMax; j++) {
        _buffers[i].setUint8(16 + j, pat(0, j));
      }
    }
  }

  // --- Writer ---

  ByteData get wBegin => ByteData.sublistView(_buffers[_wWork], 16, 16 + payloadMax);

  /// Publish: write envelope + canary, then assign latest.
  /// PLAIN ASSIGNMENT — single isolate only.
  PubResult publish(int seq, int payloadLen) {
    if (_revoked) {
      _epoch += 1; // ACK (plain increment)
      _tDrop += 1;
      return PubResult.droppedRevoked;
    }

    if (payloadLen < 0 || payloadLen > payloadMax) {
      _tInvalid++;
      return PubResult.invalid;
    }

    _envelopeEncodeV1(_buffers[_wWork], seq, payloadLen);
    _buffers[_wWork].setInt64(bufSize - 8, seq, Endian.little); // canary

    // THE exchange: plain assignment (single-isolate)
    final old = _latest;
    _latest = _wWork;
    _wWork = old;

    _tPublish += 1;
    return PubResult.ok;
  }

  // --- Reader ---

  /// Claim the freshest published buffer. NEVER fails.
  int claim() {
    final mine = _latest;
    _latest = _rWork;
    _rWork = mine;
    _tClaim += 1;
    return mine;
  }

  int rSeq() => _buffers[_rWork].getInt32(8, Endian.little);
  int rMagic() => _buffers[_rWork].getInt32(0, Endian.little);
  int rPayloadLen() => _buffers[_rWork].getInt32(12, Endian.little);
  int rCanary() => _buffers[_rWork].getInt64(bufSize - 8, Endian.little);

  Uint8List rReadSlice(int offset, int len) {
    if (offset >= bufSize) return Uint8List(0);
    final n = len < bufSize - offset ? len : bufSize - offset;
    return _buffers[_rWork].buffer.asUint8List(offset, n);
  }

  // --- I6 handshake ---

  void revoke() { _revoked = true; }

  /// destroy: API parity with C kernel. On a GC'd runtime, dropping the last
  /// reference IS the deallocation; destroy() revokes the writer so a late
  /// publish ACKs as droppedRevoked instead of writing into a channel nobody
  /// owns. The epoch is NOT incremented here — the ACK belongs to the
  /// writer's own publish path. (The previous revision faked the ACK
  /// incrementally, diverging from every other port's destroy semantics.)
  void destroy() { revoke(); }

  /// Single-isolate reclaim: the epoch can only advance when this event loop
  /// runs other code — a synchronous busy-wait can NEVER observe a change
  /// (it would freeze the isolate for the whole bound and then fail, since
  /// the writer lives on the same loop). So the sync path checks the epoch
  /// ONCE and returns immediately — a zero-width bound, bounded by
  /// construction — and directs the caller to [reclaimAsync] for a real
  /// bounded wait. [timeoutMs] is honored where waiting is real: on the
  /// async path (clamped by [maxReclaimTimeoutMs], TIER4 §4, issue #19).
  bool reclaim(int preRevokeEpoch, int timeoutMs) {
    // The bound is trivially satisfied: a single check waits for nothing.
    // Declared: sync reclaim NEVER waits and NEVER spins — the anti-hang
    // property issue #19 task 4 demands, achieved by not waiting at all.
    return _epoch != preRevokeEpoch;
  }

  /// Async reclaim: yields to the event loop between checks so a pending
  /// writer turn can ACK. Returns true once the epoch advances past
  /// [preRevokeEpoch]; false after the EFFECTIVE bound (min of [timeoutMs]
  /// and the [maxReclaimTimeoutMs] ceiling — TIER4 §4, issue #19)
  /// milliseconds. Timeouts are counted ([tReclaimTimeoutsCount]), never
  /// silent; after false the caller must NOT poison/free (no ACK, A1).
  Future<bool> reclaimAsync(int preRevokeEpoch, int timeoutMs) async {
    var effectiveMs = timeoutMs;
    if (maxReclaimTimeoutMs != 0 && effectiveMs > maxReclaimTimeoutMs) {
      effectiveMs = maxReclaimTimeoutMs;
    }
    final sw = Stopwatch()..start();
    while (_epoch == preRevokeEpoch) {
      if (sw.elapsedMilliseconds >= effectiveMs) {
        _tReclaimTimeouts += 1;
        return false;
      }
      await Future<void>.delayed(Duration.zero);
    }
    return true;
  }

  /// TIER4 §4: runtime-configurable reclaim ceiling (ms); 0 disables.
  int maxReclaimTimeoutMs = 1000;
  void setMaxReclaimTimeout(int maxMs) { maxReclaimTimeoutMs = maxMs; }
  int _tReclaimTimeouts = 0;
  int get tReclaimTimeoutsCount => _tReclaimTimeouts;

  // --- Telemetry (advisory) ---
  int get tPublishCount => _tPublish;
  int get tClaimCount => _tClaim;
  int get tDropCount => _tDrop;
  int get tInvalidCount => _tInvalid;
  int get epochVal => _epoch;

  /// debug: API parity with C kernel.
  Map<String, dynamic> debugState() => {'latest': _latest, 'wWork': _wWork, 'rWork': _rWork, 'revoked': _revoked, 'epoch': _epoch,'tPublish': _tPublish, 'tClaim': _tClaim, 'tDrop': _tDrop,'advisory': true};
}

// --- Envelope pure functions ---

void _envelopeEncodeV1(ByteData buf, int seq, int payloadLen) {
  _envelopeEncode(buf, weftVersion1, 16, seq, payloadLen);
}

void _envelopeEncode(ByteData buf, int version, int headerSize, int seq, int payloadLen) {
  buf.setInt32(0, weftMagic, Endian.little);
  buf.setInt16(4, version, Endian.little);
  buf.setInt16(6, headerSize, Endian.little);
  buf.setInt32(8, seq, Endian.little);
  buf.setInt32(12, payloadLen, Endian.little);
  for (var i = 16; i < headerSize; i++) {
    buf.setUint8(i, 0xAA);
  }
}

DecodeResult envelopeDecode(ByteData buf, int avail) {
  if (avail < 16) return DecodeResult.short;
  if (buf.getInt32(0, Endian.little) != weftMagic) return DecodeResult.badMagic;
  final hs = buf.getInt16(6, Endian.little);
  if (hs < 16 || hs > avail) return DecodeResult.badHeader;
  final pl = buf.getInt32(12, Endian.little);
  if (pl > avail - hs) return DecodeResult.short;
  return DecodeResult.ok;
}

int negotiate(int writerVersion, List<int> readerVersions) {
  var chosen = 0;
  for (var rv in readerVersions) {
    if (rv <= writerVersion && rv > chosen) chosen = rv;
  }
  return chosen;
}

// --- Payload pattern (04-LITMUS §0.1) ---

int _mix32(int x) {
  x = x ^ (x >>> 16);
  x = (x * 0x7FEB352D) & 0xFFFFFFFF;
  x = x ^ (x >>> 15);
  x = (x * 0x846CA68B) & 0xFFFFFFFF;
  x = x ^ (x >>> 16);
  return x & 0xFFFFFFFF;
}

int pat(int seq, int i) {
  final x = (seq * 2654435761 + i * 2246822519) & 0xFFFFFFFF;
  return _mix32(x) & 0xFF;
}
