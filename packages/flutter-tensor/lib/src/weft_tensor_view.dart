// weft_tensor_view.dart — zero-allocation frame flyweight (Law 1).
//
// One WeftTensorView instance is created per WeftRing AT ATTACH and REBINDS
// itself to every acquired frame — the acquire path never constructs anything
// (Law 1: zero steady-state allocation). All header-derived geometry
// (shape/strides/dtype) is read through the ring's immutable [RingHeaderInfo]
// rather than copied per frame.
//
// Law 2 (strict LE): field composition happens from lo/hi u32 halves that
// were read with explicit Endian.little in ring_header.dart / weft_ring.dart.
// Law 4 (boundary validation): a view is only ever bound by WeftRing AFTER
// magic + committed-bit + seq validation passed (seqlock decision table).
library weft_flutter_tensor.src.weft_tensor_view;

import 'dart:typed_data';

import 'ring_header.dart';
import 'weft_ring.dart';

/// A bound view of ONE committed WTR1 slot.
///
/// **Flyweight contract**: [WeftRing.acquireLatest] and [WeftRing.acquireFrame]
/// always return the SAME instance, rebound in place. Copy out any scalar you
/// need to keep (they are plain ints); the payload [Uint8List] aliases ring
/// memory and is only valid until the producer laps this slot.
class WeftTensorView {
  WeftRing? _ring;
  int _slotBase = 0;
  int _payloadLen = 0;
  int _seq = 0;
  int _seqLo = 0;
  int _seqHi = 0;
  int _timestampLo = 0;
  int _timestampHi = 0;
  int _durationUs = 0;
  int _slotFlags = 0;
  int _fourcc = 0;
  int _rank = 0;
  int _planes = 0;
  Uint8List? _payload;

  /// Rebind this flyweight onto a validated slot. Zero allocation: every
  /// operation here is a field store (Law 1).
  void bind(
    WeftRing ring,
    int slotBase,
    SlotHeaderInfo meta,
    Uint8List payloadCapView,
  ) {
    _ring = ring;
    _slotBase = slotBase;
    _payloadLen = meta.payloadLen;
    _seq = meta.seq;
    _seqLo = meta.seqLo;
    _seqHi = meta.seqHi;
    _timestampLo = meta.timestampLo;
    _timestampHi = meta.timestampHi;
    _durationUs = meta.durationUs;
    _slotFlags = meta.flags;
    _fourcc = meta.fourcc;
    _rank = meta.rank;
    _planes = meta.planes;
    _payload = payloadCapView;
  }

  /// Ring this view is bound to (null when never bound).
  WeftRing? get ring => _ring;

  /// Byte offset of the slot header inside the ring memory.
  int get slotBase => _slotBase;

  /// Live payload bytes (<= [WeftRing.payloadCap]).
  int get payloadLen => _payloadLen;

  /// Frame sequence number (1-based; equals the producer_seq value after
  /// this commit). Exact int < 2^53 (managed Lo/Hi discipline).
  int get seq => _seq;
  int get seqLo => _seqLo;
  int get seqHi => _seqHi;

  /// Capture timestamp in ns — exact for timestamps < 2^63 ns (Dart VM ints
  /// are 64-bit; composed from the LE lo/hi halves read off the wire).
  int get timestampNs => (_timestampHi << 32) | _timestampLo;
  int get timestampLo => _timestampLo;
  int get timestampHi => _timestampHi;

  /// Producer frame-budget hint in microseconds (e.g. 8333 for 120 fps).
  int get durationUs => _durationUs;

  /// Raw slot flags (bit0 = committed — always set for a bound view).
  int get slotFlags => _slotFlags;

  /// Payload format as a LE u32 fourcc ("RGBA", "BGRA", "I420", "PCM ", ...).
  int get fourccCode => _fourcc;

  /// Per-slot rank as written by the producer (<= 8).
  int get slotRank => _rank;
  int get planes => _planes;

  /// Payload view over this slot's FULL capacity — no bytes are copied and
  /// NO view is allocated per acquire (the ring pre-created it at attach).
  /// Only the first [payloadLen] bytes are live.
  Uint8List get payloadView => _payload!;

  // -- header-derived geometry (through the ring's immutable layout) --------

  /// Ring-header rank (dimensions of the ring's declared tensor shape).
  int get rank => _ring!.layout.rank;

  /// Ring-header shape (u32 per dimension, 0-padded past [rank]).
  List<int> get shape => _ring!.layout.shape;

  /// Ring-header strides in ELEMENTS (row-major; byte stride = stride *
  /// elemSize).
  List<int> get strides => _ring!.layout.strides;

  /// DLPack dtype code (verbatim wire value).
  int get dtypeCode => _ring!.layout.dtypeCode;
  int get dtypeBits => _ring!.layout.dtypeBits;
  int get dtypeLanes => _ring!.layout.dtypeLanes;
  int get elemSize => _ring!.layout.elemSize;

  /// fourcc decoded to a String — DIAGNOSTIC ONLY (allocates; never call in
  /// the steady-state frame path, Law 1).
  String get fourccString => fourccToString(_fourcc);

  @override
  String toString() => 'WeftTensorView(seq=$_seq, len=$_payloadLen, '
      'ts=$_timestampNs ns, fourcc=$fourccString)';
}
