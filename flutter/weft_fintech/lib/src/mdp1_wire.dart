// mdp1_wire.dart — MDP1 aggregated book snapshot wire, Dart side.
//
// Mirrors packages/fintech/src/mdp1.js byte-for-byte (Pillar 6 / Law 4):
//   304-byte little-endian record:
//     0   magic "MDP1" | 4 version u16 | 6 flags u16
//     8   seq u64 | 16 last_ts_ns u64 | 24 best_bid u32 | 28 best_ask u32
//     32  bid_levels[10] x12 {price,size,orders} | 152 ask_levels[10] x12
//     272 msg_count u64 | 280 trade_count u64 | 288 last_match u64
//     296 crc32 u32 (poly 0xEDB88320, init/final 0xFFFFFFFF, over [0,296))
//     300 reserved u32
//
// All multi-byte reads use ByteData with EXPLICIT Endian.little (Law 2).
// u64 fields compose as lo + hi * 4294967296 — exact Dart ints, never
// floats. The flyweight binds to the CALLER's bytes: book state never
// crosses into widget state (Law 4 / W4-06).
//
// Audit contract: constants below are `const int <name> = <value>;` so
// test/static_audit.mjs can diff them against the TS reference.

// ignore_for_file: constant_identifier_names

library;

import 'dart:typed_data';

const int MDP1_SIZE = 304;
const int MDP1_VERSION = 1;
const int MDP1_TOP_LEVELS = 10;

const int F_BOOK_VALID = 1 << 0;
const int F_CROSSED = 1 << 1;
const int F_LOCKED = 1 << 2;

const int MDP1_OFF_VERSION = 4;
const int MDP1_OFF_FLAGS = 6;
const int MDP1_OFF_SEQ = 8;
const int MDP1_OFF_LAST_TS = 16;
const int MDP1_OFF_BEST_BID = 24;
const int MDP1_OFF_BEST_ASK = 28;
const int MDP1_OFF_BIDS = 32;
const int MDP1_OFF_ASKS = 152;
const int MDP1_OFF_MSG_COUNT = 272;
const int MDP1_OFF_TRADE_COUNT = 280;
const int MDP1_OFF_LAST_MATCH = 288;
const int MDP1_OFF_CRC = 296;
const int MDP1_CRC_END = 296; // CRC covers [0, 296)

const List<int> MDP1_MAGIC_BYTES = <int>[0x4D, 0x44, 0x50, 0x31]; // "MDP1"
const int MDP1_MAGIC = 0x3150444d; // little-endian u32 read of "MDP1"

// Typed failure codes — Law 4 taxonomy (TS decision order).
const int MDP1_OK = 0;
const int MDP1_E_SHORT = 1; // < 304 bytes
const int MDP1_E_MAGIC = 2;
const int MDP1_E_VERSION = 3;
const int MDP1_E_CRC = 4;

Uint32List? _crcTable;

/// CRC-32/ISO-HDLC table (poly 0xEDB88320), built once per isolate.
Uint32List crcTable() {
  final t = _crcTable;
  if (t != null) return t;
  final built = Uint32List(256);
  for (int n = 0; n < 256; n++) {
    int c = n;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) != 0 ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    built[n] = c & 0xFFFFFFFF;
  }
  _crcTable = built;
  return built;
}

/// CRC-32 over bytes [start, end) of `u8` (defaults to the MDP1 header
/// region [0, 296)). Table-driven, allocation-free per call.
int mdp1Crc32(Uint8List u8, [int start = 0, int end = MDP1_CRC_END]) {
  final table = _crcTable ?? crcTable();
  int crc = 0xFFFFFFFF;
  for (int i = start; i < end; i++) {
    crc = (crc >>> 8) ^ table[(crc ^ u8[i]) & 0xFF];
  }
  return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
}

/// Reusable flyweight over one MDP1 record. `retarget` re-points it at new
/// bytes — the widget owns ONE snapshot for the whole feed lifetime.
class Mdp1Snapshot {
  ByteData _dv;
  Uint8List _u8;

  Mdp1Snapshot(Uint8List buffer)
      : _dv = ByteData.sublistView(buffer),
        _u8 = buffer;

  /// Re-point this flyweight at a fresh record (zero allocation beyond the
  /// single sublist view — performed once per delivered frame, never per
  /// level or per paint).
  void retarget(Uint8List buffer) {
    _u8 = buffer;
    _dv = ByteData.sublistView(buffer);
  }

  /// Fail-closed structural check: returns MDP1_OK or a typed E_* code
  /// evaluated in the TS decision order (short -> magic -> version -> CRC).
  int validate() {
    if (_u8.length < MDP1_SIZE) return MDP1_E_SHORT;
    if (_dv.getUint32(0, Endian.little) != MDP1_MAGIC) return MDP1_E_MAGIC;
    if (_dv.getUint16(MDP1_OFF_VERSION, Endian.little) != MDP1_VERSION) {
      return MDP1_E_VERSION;
    }
    final stored = _dv.getUint32(MDP1_OFF_CRC, Endian.little);
    if (stored != mdp1Crc32(_u8)) return MDP1_E_CRC;
    return MDP1_OK;
  }

  int get flags => _dv.getUint16(MDP1_OFF_FLAGS, Endian.little);
  bool get bookValid => (flags & F_BOOK_VALID) != 0;
  bool get crossed => (flags & F_CROSSED) != 0;
  bool get locked => (flags & F_LOCKED) != 0;

  /// u64 seq as exact Dart int (lo + hi * 2^32).
  int get seq {
    final lo = _dv.getUint32(MDP1_OFF_SEQ, Endian.little);
    final hi = _dv.getUint32(MDP1_OFF_SEQ + 4, Endian.little);
    return hi * 4294967296 + lo;
  }

  int get lastTsNs {
    final lo = _dv.getUint32(MDP1_OFF_LAST_TS, Endian.little);
    final hi = _dv.getUint32(MDP1_OFF_LAST_TS + 4, Endian.little);
    return hi * 4294967296 + lo;
  }

  int get bestBid => _dv.getUint32(MDP1_OFF_BEST_BID, Endian.little);
  int get bestAsk => _dv.getUint32(MDP1_OFF_BEST_ASK, Endian.little);

  int bidPrice(int i) =>
      _dv.getUint32(MDP1_OFF_BIDS + i * 12, Endian.little);
  int bidSize(int i) =>
      _dv.getUint32(MDP1_OFF_BIDS + i * 12 + 4, Endian.little);
  int bidOrders(int i) =>
      _dv.getUint32(MDP1_OFF_BIDS + i * 12 + 8, Endian.little);
  int askPrice(int i) =>
      _dv.getUint32(MDP1_OFF_ASKS + i * 12, Endian.little);
  int askSize(int i) =>
      _dv.getUint32(MDP1_OFF_ASKS + i * 12 + 4, Endian.little);
  int askOrders(int i) =>
      _dv.getUint32(MDP1_OFF_ASKS + i * 12 + 8, Endian.little);

  int get msgCount => _dv.getUint32(MDP1_OFF_MSG_COUNT, Endian.little);
  int get tradeCount => _dv.getUint32(MDP1_OFF_TRADE_COUNT, Endian.little);

  int get lastMatch {
    final lo = _dv.getUint32(MDP1_OFF_LAST_MATCH, Endian.little);
    final hi = _dv.getUint32(MDP1_OFF_LAST_MATCH + 4, Endian.little);
    return hi * 4294967296 + lo;
  }

  bool get crcOk =>
      _dv.getUint32(MDP1_OFF_CRC, Endian.little) == mdp1Crc32(_u8);
}
