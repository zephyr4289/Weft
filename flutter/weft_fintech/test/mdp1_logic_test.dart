// test/mdp1_logic_test.dart — dependency-free pure-Dart harness (dart run).
//
// Verifies the MDP1 wire decode WITHOUT a Flutter toolchain: CRC known
// answers (externally derived with node:zlib — the same oracle the repo
// used in Pillars 2/4), field offsets, u64 composition, and the
// fail-closed corruption matrix in TS decision order.
//
// Exit: 0 all green; 1 on the first mismatch (print-labeled).

import 'dart:typed_data';

import 'package:weft_fintech/src/mdp1_wire.dart';

int _failures = 0;

void check(String label, bool cond) {
  if (cond) {
    print('PASS  $label');
  } else {
    print('FAIL  $label');
    _failures++;
  }
}

/// Canonical deterministic 304-byte record: byte i = (i*7+13) & 0xff,
/// CRC over [0,296) — node:zlib oracle says 3666738418.
Uint8List canonicalRecord() {
  final b = Uint8List(MDP1_SIZE);
  for (int i = 0; i < MDP1_SIZE; i++) {
    b[i] = (i * 7 + 13) & 0xFF;
  }
  final dv = ByteData.sublistView(b);
  dv.setUint32(MDP1_OFF_CRC, 3666738418, Endian.little);
  return b;
}

/// A VALID record synthesized from field values (CRC computed, not
/// frozen) — exercises the pack -> decode round trip.
Uint8List synthRecord(
    {int seq = 1000000,
    int lastTs = 1758400000123456789 % (1 << 48),
    int bestBid = 901234,
    int bestAsk = 901240}) {
  final b = Uint8List(MDP1_SIZE);
  final dv = ByteData.sublistView(b);
  dv.setUint32(0, MDP1_MAGIC, Endian.little);
  dv.setUint16(MDP1_OFF_VERSION, MDP1_VERSION, Endian.little);
  dv.setUint16(MDP1_OFF_FLAGS, F_BOOK_VALID, Endian.little);
  dv.setUint32(MDP1_OFF_SEQ, seq & 0xFFFFFFFF, Endian.little);
  dv.setUint32(MDP1_OFF_SEQ + 4, seq >>> 32, Endian.little);
  dv.setUint32(MDP1_OFF_LAST_TS, lastTs & 0xFFFFFFFF, Endian.little);
  dv.setUint32(MDP1_OFF_LAST_TS + 4, lastTs >>> 32, Endian.little);
  dv.setUint32(MDP1_OFF_BEST_BID, bestBid, Endian.little);
  dv.setUint32(MDP1_OFF_BEST_ASK, bestAsk, Endian.little);
  for (int i = 0; i < MDP1_TOP_LEVELS; i++) {
    dv.setUint32(MDP1_OFF_BIDS + i * 12, bestBid - i * 2, Endian.little);
    dv.setUint32(MDP1_OFF_BIDS + i * 12 + 4, 1000 * (10 - i), Endian.little);
    dv.setUint32(MDP1_OFF_BIDS + i * 12 + 8, 10 - i, Endian.little);
    dv.setUint32(MDP1_OFF_ASKS + i * 12, bestAsk + i * 2, Endian.little);
    dv.setUint32(MDP1_OFF_ASKS + i * 12 + 4, 1000 * (10 - i), Endian.little);
    dv.setUint32(MDP1_OFF_ASKS + i * 12 + 8, 10 - i, Endian.little);
  }
  dv.setUint32(MDP1_OFF_MSG_COUNT, seq & 0xFFFFFFFF, Endian.little);
  dv.setUint32(MDP1_OFF_TRADE_COUNT, 4242, Endian.little);
  dv.setUint32(MDP1_OFF_CRC, mdp1Crc32(b), Endian.little);
  return b;
}

void main() {
  // -- CRC known answers (node:zlib-derived, re-derived by the audit) -----
  check('crc KAT1: crc32("WEFT") == 3421166146',
      mdp1Crc32(Uint8List.fromList('WEFT'.codeUnits), 0, 4) == 3421166146);
  final asc = Uint8List(256);
  for (int i = 0; i < 256; i++) {
    asc[i] = i;
  }
  check('crc KAT2: crc32(0..255) == 688229491', mdp1Crc32(asc, 0, 256) == 688229491);
  check('crc KAT3: canonical 304B record region == 3666738418',
      mdp1Crc32(canonicalRecord()) == 3666738418);

  // -- synth record decodes exactly ----------------------------------------
  final snap = Mdp1Snapshot(synthRecord());
  check('synth: validate == MDP1_OK', snap.validate() == MDP1_OK);
  check('synth: seq composes lo+hi*2^32 (exact)', snap.seq == 1000000);
  check('synth: lastTsNs exact', snap.lastTsNs == 1758400000123456789 % 281474976710656);
  check('synth: bestBid/bestAsk', snap.bestBid == 901234 && snap.bestAsk == 901240);
  check('synth: bid level 0 fields', snap.bidPrice(0) == 901234 &&
      snap.bidSize(0) == 10000 && snap.bidOrders(0) == 10);
  check('synth: ask level 3 fields', snap.askPrice(3) == 901246 &&
      snap.askSize(3) == 7000 && snap.askOrders(3) == 7);
  check('synth: tradeCount', snap.tradeCount == 4242);
  check('synth: crcOk', snap.crcOk);
  check('synth: bookValid flag', snap.bookValid && !snap.crossed && !snap.locked);

  // -- flyweight retarget (one snapshot reused across records) --------------
  final recA = synthRecord(seq: 7);
  final recB = synthRecord(seq: 9);
  final fly = Mdp1Snapshot(recA);
  check('flyweight: bind A seq 7', fly.validate() == MDP1_OK && fly.seq == 7);
  fly.retarget(recB);
  check('flyweight: retarget B seq 9', fly.validate() == MDP1_OK && fly.seq == 9);

  // -- corruption matrix (TS decision order: short -> magic -> version ->
  //    CRC) -------------------------------------------------------------------
  final valid = synthRecord();

  final short = Uint8List.sublistView(valid, 0, 303);
  check('corrupt: short record -> MDP1_E_SHORT',
      Mdp1Snapshot(short).validate() == MDP1_E_SHORT);

  final badMagic = Uint8List.fromList(valid);
  badMagic[0] = 0x4D; // "MDPX"
  badMagic[3] = 0x58;
  check('corrupt: bad magic -> MDP1_E_MAGIC',
      Mdp1Snapshot(badMagic).validate() == MDP1_E_MAGIC);

  final badVersion = Uint8List.fromList(valid);
  badVersion[MDP1_OFF_VERSION] = 2;
  check('corrupt: bad version -> MDP1_E_VERSION',
      Mdp1Snapshot(badVersion).validate() == MDP1_E_VERSION);

  final badCrc = Uint8List.fromList(valid);
  final dvC = ByteData.sublistView(badCrc);
  dvC.setUint32(MDP1_OFF_CRC, dvC.getUint32(MDP1_OFF_CRC, Endian.little) ^ 1,
      Endian.little);
  check('corrupt: flipped CRC -> MDP1_E_CRC',
      Mdp1Snapshot(badCrc).validate() == MDP1_E_CRC);

  // payload corruption (outside CRC region is impossible — CRC covers
  // [0,296); flip a level byte INSIDE the region)
  final badLevel = Uint8List.fromList(valid);
  badLevel[MDP1_OFF_BIDS + 4] ^= 0xFF;
  check('corrupt: level payload flip -> MDP1_E_CRC',
      Mdp1Snapshot(badLevel).validate() == MDP1_E_CRC);

  // -- constants pin (byte-list magic + little-endian u32 read) -------------
  check('magic byte list is "MDP1"',
      MDP1_MAGIC_BYTES[0] == 0x4D &&
          MDP1_MAGIC_BYTES[1] == 0x44 &&
          MDP1_MAGIC_BYTES[2] == 0x50 &&
          MDP1_MAGIC_BYTES[3] == 0x31);
  check('magic u32 == 0x3150444d', MDP1_MAGIC == 0x3150444d);
  check('MDP1_SIZE == 304 && MDP1_TOP_LEVELS == 10',
      MDP1_SIZE == 304 && MDP1_TOP_LEVELS == 10);

  if (_failures > 0) {
    print('\n$_failures FAILURE(S)');
    _failures = 0;
    throw StateError('mdp1_logic_test failed');
  }
  print('\nALL GREEN');
}
