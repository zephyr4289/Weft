// weft_ring.dart — WeftRing: dart:ffi attach + wait-free seqlock consumer.
//
// Spec: docs/weft-tensor/LAYOUT-V1.md (NORMATIVE). TS reference:
// packages/weft-tensor/src/ring.js (same protocol, Dart host).
//
// Publish protocol (producer): slot header is written with COMMITTED bit
// CLEAR (torn marker), then all fields, then the COMMITTED bit, then the
// u64 producer_seq publish stores — HI WORD FIRST, LO WORD LAST (TS parity).
//
// Acquire protocol (consumer, wait-free): read producer_seq (lo/hi), pick
// slot (s-1) % slot_count, validate magic + committed bit + slot seq == s,
// bounded retry (slot_count attempts) else null (overrun window). A torn
// producer_seq read can never corrupt a consumer — every acquire
// re-validates the slot's own 64-bit seq + magic + committed bit (seqlock).
//
// Law 1 (zero steady-state allocation): the acquire path allocates NOTHING —
// the ByteData window, per-slot payload views, the SlotHeaderInfo scratch,
// the flyweight WeftTensorView and the stats struct are all created ONCE at
// attach. Law 2 (strict LE): every multi-byte access passes Endian.little
// explicitly. Law 4 (boundary validation): full header validation at attach;
// slot header validation on every acquire.
library weft_flutter_tensor.src.weft_ring;

import 'dart:ffi' as ffi;
import 'dart:typed_data';

import 'ring_header.dart';
import 'weft_tensor_view.dart';

/// Diagnostics counters — one instance per ring, mutated in place (Law 1).
class WeftRingStats {
  int commits = 0;
  int acquireCalls = 0;
  int tornReads = 0;
  int overruns = 0;

  void reset() {
    commits = 0;
    acquireCalls = 0;
    tornReads = 0;
    overruns = 0;
  }

  @override
  String toString() => 'WeftRingStats(commits=$commits, acquireCalls='
        '$acquireCalls, tornReads=$tornReads, overruns=$overruns)';
}

/// Producer-side commit handle — ONE instance per ring, reused (Law 1).
class WeftCommitHandle {
  WeftCommitHandle._();

  int seq = 0;
  int slot = 0;
  Uint8List? payloadU8;
}

/// Single-producer / multi-consumer seqlock tensor ring over WTR1 memory.
///
/// Attach (once, cold path):
/// ```dart
/// final ring = WeftRing.attachPointer(ptr, byteLength); // dart:ffi memory
/// final ring = WeftRing.attachByteData(bd);             // in-memory/fixture
/// ```
/// Consume (hot path, zero allocation):
/// ```dart
/// final view = ring.acquireLatest();   // REUSED flyweight or null
/// if (view != null && view.seq != lastSeq) { ...read view.payloadView... }
/// ```
class WeftRing {
  WeftRing._(this.byteData, this.layout, {this.pointer})
      : bytes = Uint8List.sublistView(byteData) {
    _length = layout.byteLength;
    _defaultFourcc = fourccFromString('RAW ');
    // Attach-time preallocation: one payload view per slot (full capacity).
    // The acquire path indexes this list — it never creates views (Law 1).
    final caps = <Uint8List>[];
    for (var s = 0; s < layout.slotCount; s++) {
      final start = layout.headerSize + s * layout.slotStride + slotHeaderSize;
      caps.add(Uint8List.sublistView(bytes, start, start + layout.payloadCap));
    }
    _slotPayloads = caps;
  }

  /// Attach to FFI memory (native producer, mmap'd ring, adapter handoff).
  /// Full Law-4 validation runs here — throws [LayoutException] on any
  /// corruption class.
  static WeftRing attachPointer(ffi.Pointer<ffi.Uint8> pointer, int byteLength) {
    final view = pointer.asTypedList(byteLength);
    final bd = ByteData.view(
        view.buffer, view.offsetInBytes, view.lengthInBytes);
    return WeftRing._attach(bd, pointer: pointer);
  }

  /// Attach to an existing [ByteData] window (in-memory rings, fixtures,
  /// tests). The window must contain header + all slots.
  static WeftRing attachByteData(ByteData bd, {int? byteLength}) {
    return WeftRing._attach(bd, byteLength: byteLength);
  }

  static WeftRing _attach(ByteData bd, {int? byteLength, ffi.Pointer<ffi.Uint8>? pointer}) {
    final layout = RingHeaderInfo.parse(bd, byteLength: byteLength);
    return WeftRing._(bd, layout, pointer: pointer);
  }

  /// Underlying window (read-only exposure for tooling/tests).
  final ByteData byteData;

  /// Byte view over the same memory (offset-preserving).
  final Uint8List bytes;

  /// Native pointer when attached via [attachPointer], else null.
  final ffi.Pointer<ffi.Uint8>? pointer;

  /// Parsed + validated header (immutable after attach).
  final RingHeaderInfo layout;

  late int _length;
  late List<Uint8List> _slotPayloads;
  late int _defaultFourcc;
  final SlotHeaderInfo _meta = SlotHeaderInfo(); // reused scratch (Law 1)
  final WeftTensorView _view = WeftTensorView(); // reused flyweight (Law 1)
  final WeftCommitHandle _commitHandle = WeftCommitHandle._();
  final WeftRingStats stats = WeftRingStats();

  int get slotCount => layout.slotCount;
  int get slotStride => layout.slotStride;
  int get payloadCap => layout.payloadCap;
  int get byteLength => _length;
  int get tickHz => layout.tickHz;

  /// Preallocated payload-capacity view for slot [slot] (producer write path).
  Uint8List payloadView(int slot) => _slotPayloads[slot];

  /// Latest committed sequence (0 = nothing ever committed). Composed from
  /// the LE lo/hi halves; exact (< 2^53, enforced at attach).
  int get producerSeq {
    final lo = byteData.getUint32(offProducerSeq, Endian.little);
    final hi = byteData.getUint32(offProducerSeq + 4, Endian.little);
    return (hi << 32) | lo;
  }

  // -- consumer (wait-free, zero allocation) ---------------------------------

  /// Acquire the latest committed frame into the REUSED flyweight view.
  ///
  /// Returns null when nothing is committed yet (`producer_seq == 0`), when
  /// nothing NEWER than [afterSeq] exists, or after [slotCount] bounded
  /// retries through a torn/overrun window (stats.tornReads / stats.overruns
  /// say which). The SAME [WeftTensorView] instance is returned every time —
  /// treat it as valid only until the next acquire.
  WeftTensorView? acquireLatest({int? afterSeq}) {
    stats.acquireCalls++;
    final sc = layout.slotCount;
    for (var attempt = 0; attempt < sc; attempt++) {
      final s = producerSeq;
      if (s == 0) return null; // nothing ever committed
      if (afterSeq != null && s <= afterSeq) return null; // nothing new
      final slot = (s - 1) % sc;
      final base = layout.headerSize + slot * layout.slotStride;
      if (readSlotHeader(byteData, base, _meta) &&
          _meta.seq == s &&
          _meta.payloadLen <= layout.payloadCap) {
        _view.bind(this, base, _meta, _slotPayloads[slot]);
        return _view;
      }
      // Torn read / producer lapped the slot / impossible payload_len —
      // bounded seqlock retry (Law 4: report loss, never corruption).
      stats.tornReads++;
    }
    stats.overruns++;
    return null;
  }

  /// Acquire the frame with an EXACT sequence number, or null when it has
  /// already fallen out of the ring (consumer too slow) or is not yet
  /// committed. Zero allocation (same reused flyweight).
  WeftTensorView? acquireFrame(int seq) {
    if (seq < 1) return null;
    stats.acquireCalls++;
    final sc = layout.slotCount;
    final latest = producerSeq;
    if (seq > latest) return null; // not yet
    if (latest - seq >= sc) {
      stats.overruns++;
      return null; // gone — overwrite semantics, frame loss is reported
    }
    final slot = (seq - 1) % sc;
    final base = layout.headerSize + slot * layout.slotStride;
    if (readSlotHeader(byteData, base, _meta) &&
        _meta.seq == seq &&
        _meta.payloadLen <= layout.payloadCap) {
      _view.bind(this, base, _meta, _slotPayloads[slot]);
      return _view;
    }
    return null;
  }

  // -- producer (single-producer; zero allocation beyond the byte copy) ------

  /// Zero-copy producer fast path: returns the REUSED commit handle for the
  /// NEXT slot (seq = producerSeq + 1). Write your payload straight into
  /// [WeftCommitHandle.payloadU8], then call [finishCommit].
  /// Not re-entrant: one producer at a time (WTR1 contract).
  WeftCommitHandle beginCommit() {
    final seq = producerSeq + 1;
    final slot = (seq - 1) % layout.slotCount;
    _commitHandle
      ..seq = seq
      ..slot = slot
      ..payloadU8 = _slotPayloads[slot];
    return _commitHandle;
  }

  /// Publish a frame previously targeted by [beginCommit]. Returns [seq].
  /// TS-parity ordering: header with COMMITTED CLEAR -> fields -> COMMITTED
  /// -> producer_seq hi store -> producer_seq lo store (publish fence).
  int finishCommit(
    int byteLen, {
    int timestampNs = 0,
    int durationUs = 0,
    int? fourccCode,
    int? rank,
    int planes = 1,
  }) {
    if (byteLen < 0 || byteLen > layout.payloadCap) {
      throw LayoutException('WTR1_COMMIT_RANGE',
          'payload ${byteLen}B > cap ${layout.payloadCap}B');
    }
    final seq = _commitHandle.seq;
    final slot = _commitHandle.slot;
    final base = layout.headerSize + slot * layout.slotStride;
    final bd = byteData;

    final tsLo = timestampNs & 0xffffffff;
    final tsHi = (timestampNs >>> 32) & 0xffffffff;
    final fcc = fourccCode ?? _defaultFourcc;
    final rk = rank ?? layout.rank;

    bd.setUint8(base + soffMagic + 0, slotMagicBytes[0]);
    bd.setUint8(base + soffMagic + 1, slotMagicBytes[1]);
    bd.setUint8(base + soffMagic + 2, slotMagicBytes[2]);
    bd.setUint8(base + soffMagic + 3, slotMagicBytes[3]);
    bd.setUint32(base + soffSlotFlags, 0, Endian.little); // torn marker
    bd.setUint32(base + soffPayloadLen, byteLen, Endian.little);
    bd.setUint32(base + soffSeq, seq & 0xffffffff, Endian.little);
    bd.setUint32(base + soffSeq + 4, (seq >>> 32) & 0xffffffff, Endian.little);
    bd.setUint32(base + soffTimestampNs, tsLo, Endian.little);
    bd.setUint32(base + soffTimestampNs + 4, tsHi, Endian.little);
    bd.setUint32(base + soffDurationUs, durationUs, Endian.little);
    bd.setUint32(base + soffFourcc, fcc, Endian.little);
    bd.setUint8(base + soffRank, rk);
    bd.setUint8(base + soffPlanes, planes);
    bd.setUint32(base + soffPlaneOffset, 0, Endian.little);
    bd.setUint32(base + soffPlaneOffset + 4, 0, Endian.little);
    bd.setUint32(base + soffPlaneOffset + 8, 0, Endian.little);
    bd.setUint32(base + soffPlaneSize, byteLen, Endian.little);
    bd.setUint32(base + soffPlaneSize + 4, 0, Endian.little);
    bd.setUint32(base + soffPlaneSize + 8, 0, Endian.little);
    bd.setUint32(base + soffSlotFlags, slotFlagCommitted, Endian.little);

    // Publish: hi word first, LO WORD LAST (TS-parity fence ordering).
    bd.setUint32(offProducerSeq + 4, (seq >>> 32) & 0xffffffff, Endian.little);
    bd.setUint32(offProducerSeq, seq & 0xffffffff, Endian.little);
    stats.commits++;
    return seq;
  }

  /// Copy-in convenience: [beginCommit] -> byte copy -> [finishCommit].
  /// The producer MAY copy (it owns the write side); the consumer never does.
  int commit(Uint8List src, {int timestampNs = 0, int durationUs = 0, int? fourccCode}) {
    // Fail-closed BEFORE any byte lands in the ring (Law 4).
    if (src.length > layout.payloadCap) {
      throw LayoutException('WTR1_COMMIT_RANGE',
          'payload ${src.length}B > cap ${layout.payloadCap}B');
    }
    final h = beginCommit();
    final dst = h.payloadU8!;
    final n = src.length;
    for (var i = 0; i < n; i++) {
      dst[i] = src[i];
    }
    return finishCommit(n,
        timestampNs: timestampNs, durationUs: durationUs, fourccCode: fourccCode);
  }

  /// Detach diagnostics (the attached memory is owned by the caller — this
  /// class never frees a native pointer).
  void dispose() {
    // No-op today: buffers/pointer are caller-owned by design (zero-copy).
  }

  @override
  String toString() => 'WeftRing(${layout.describe()})';
}
