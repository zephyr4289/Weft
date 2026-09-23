// weft_replay.dart — RFC 0019: deterministic time-travel replay fold, Dart
// driver layer.
//
// WHY EXISTS: RFC-0019's fold (a pure state machine over a .weftrec v4
// event stream that reconstructs the full shadow kernel state per event and
// reduces it to a 64-bit FNV-1a hash) shipped as the C reference
// (core/c/weft_replay.{h,c}) with TS/Kotlin/Swift twins — this file is the
// Dart half, operation-identical to the C: the transition table and the
// 111-byte canonical serialization are NORMATIVE (RFC-0019) — cross-runtime
// hash logs are byte-compared by fixtures/xlang-replay/run.sh. Read the
// RFC's "modeling contract" before changing ANY line here: the three
// declared abstractions (null-frame len 0, PUBLISH-implies-unrevoked,
// claim validation) are what make the fold deterministic without payload
// bytes.
//
// ARITHMETIC PARITY: Dart VM ints are 64-bit two's complement, so the u32
// state is emulated with explicit masking — & 0xFFFFFFFF after every
// update that can wrap, >>> (logical shift, Dart 2.14+) for the unsigned
// shifts, and _toI32 for the C's (int32_t) reinterpretations. The u64
// FNV-1a has NO native wrapping multiply in Dart — the hash is a BigInt
// (the fold is a debugger path, not hot; determinism is the contract, and
// BigInt is exact). Counters fit VM ints (<= 2^53 in any real capture).
//
// PINNED PARITY VECTORS (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (the RFC-0019 fixture grammar,
//                                     seed 0x00C0FFEE)
//
// The speculative copy is REAL (the C's `weft_replay_state_t next = *s`):
// a step works on a fresh copy and assigns back ONLY on success, so a
// DISAGREE leaves the fold at the last good step — the debugger can
// inspect it (rule 3: never silent).
//
// The copy allocates (like the TS spread); the fold is a debugger, not a
// hot path — declared honestly, per the repo's per-port honesty culture.
//
// STATUS: SOURCE-ONLY, PENDING TOOLCHAIN VERIFICATION (not compilable in
// the x86_64 Linux sandbox — no dart; the arithmetic was validated
// op-for-op against the C reference before translation, and the fixture
// gate runs when flutter-packages CI brings the toolchain).

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/// Fold verdicts — PROTOCOL (do not renumber).
abstract final class ReplayResult {
  static const int ok = 0;        // step applied, hash updated
  static const int disagree = -1; // trace/model disagreement (claim seq mismatch)
  static const int badKind = -2;  // unknown event kind in the stream
}

/// RFC-0014 event kinds (u16) — PROTOCOL (the fold consumes them; do not
/// renumber).
abstract final class WeftTraceKind {
  static const int publish = 1;     // aux = payload_len, data = seq
  static const int claim = 2;       // data = seq claimed
  static const int drop = 3;        // aux = epoch at ACK, data = seq refused
  static const int revoke = 4;      // data = pre-revoke epoch
  static const int ack = 5;         // data = epoch after ACK
  static const int stall = 6;       // data = attempts (consumer skip)
  static const int tear = 7;        // data = seq of torn frame
  static const int canaryFail = 8;  // data = seq of bad frame
}

/// Checkpoint interval (jump cost bound) — RFC-0019.
const int weftReplayCheckpoint = 64;

// ---------------------------------------------------------------------------
// Shadow kernel state (RFC-0019 fold state)
// ---------------------------------------------------------------------------

/// One ring slot of the shadow state (seq/len/ver — the fold carries no
/// payload bytes, only the slot bookkeeping). u32 values live as
/// non-negative ints.
class ReplayBuf {
  int seq; // u32
  int len; // u32
  int ver; // u16
  ReplayBuf()
      : seq = 0,
        len = 0,
        ver = 1;
}

/// The RFC-0019 fold state. Call [weftReplayInit] before the first step —
/// the constructor leaves the raw zeroed fields (the C is a POD; init is
/// the contract, including the pinned step-0 hash).
class WeftReplayState {
  int latest = 0;   // u32 (masked)
  int epoch = 0;    // u32 (masked)
  int wWork = 0;    // u32 (masked)
  int rWork = 0;    // u32 (masked)
  int revoked = 0;  // 0/1 (u8 on the wire)

  final List<ReplayBuf> buf = [ReplayBuf(), ReplayBuf(), ReplayBuf()];

  int tPublish = 0; // u64 counter — VM int (any real capture stays <= 2^53)
  int tClaim = 0;   // u64
  int tDrop = 0;    // u64
  int tInvalid = 0; // u64
  int tWsteps = 0;  // u64
  int tRsteps = 0;  // u64
  int tStall = 0;   // u32 (masked)
  int tTear = 0;    // u32 (masked)
  int tCanary = 0;  // u32 (masked)
  int step = 0;     // events consumed (u32, masked)

  /// FNV-1a-64 over the canonical serialization — u64 as BigInt (no
  /// native wrapping multiply in Dart; BigInt is exact). Render with
  /// hash.toRadixString(16).padLeft(16, '0').
  BigInt hash = BigInt.zero;
}

// ---------------------------------------------------------------------------
// Serialization (canonical LE byte layout — RFC-0019, normative)
// ---------------------------------------------------------------------------

/// Canonical little-endian serialization (RFC-0019): 111 bytes —
/// u32 latest@0, u32 epoch@4, u32 w_work@8, u32 r_work@12, u8 revoked@16,
/// 3x(u32 seq, u32 len, u16 ver)@17, u64 t_publish/t_claim/t_drop/t_invalid/
/// t_wsteps/t_rsteps@47..94, u32 t_stall@95, t_tear@99, t_canary@103,
/// u32 step@107. Exposed for cross-port byte tests.
List<int> weftReplaySerialize(WeftReplayState s) {
  final out = List<int>.filled(111, 0);
  var p = 0;
  void putU32(int v) {
    out[p] = v & 0xFF;
    out[p + 1] = (v >>> 8) & 0xFF;
    out[p + 2] = (v >>> 16) & 0xFF;
    out[p + 3] = (v >>> 24) & 0xFF;
    p += 4;
  }

  void putU16(int v) {
    out[p] = v & 0xFF;
    out[p + 1] = (v >>> 8) & 0xFF;
    p += 2;
  }

  void putU64(int v) {
    for (var i = 0; i < 8; i++) {
      out[p + i] = (v >>> (8 * i)) & 0xFF;
    }
    p += 8;
  }

  putU32(s.latest);
  putU32(s.epoch);
  putU32(s.wWork);
  putU32(s.rWork);
  out[p] = s.revoked & 1;
  p += 1;
  for (var i = 0; i < 3; i++) {
    putU32(s.buf[i].seq);
    putU32(s.buf[i].len);
    putU16(s.buf[i].ver);
  }
  putU64(s.tPublish);
  putU64(s.tClaim);
  putU64(s.tDrop);
  putU64(s.tInvalid);
  putU64(s.tWsteps);
  putU64(s.tRsteps);
  putU32(s.tStall);
  putU32(s.tTear);
  putU32(s.tCanary);
  putU32(s.step);
  return out;
}

// ---------------------------------------------------------------------------
// FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3)
// ---------------------------------------------------------------------------

// 0xcbf29ce484222325 exceeds the signed-64 literal range — BigInt.parse.
final BigInt _fnvOffset = BigInt.parse('cbf29ce484222325', radix: 16);
final BigInt _fnvPrime = BigInt.parse('100000001b3', radix: 16);
final BigInt _mask64 = (BigInt.one << 64) - BigInt.one;

/// FNV-1a 64 over a byte range — one definition shared by every fold path.
/// BigInt keeps the mod-2^64 multiply exact.
BigInt weftReplayFnv1a(List<int> data) {
  var h = _fnvOffset;
  for (final b in data) {
    h ^= BigInt.from(b);
    h = (h * _fnvPrime) & _mask64;
  }
  return h;
}

// ---------------------------------------------------------------------------
// Fold
// ---------------------------------------------------------------------------

/// Reset to the RFC-0019 initial state (including the initial hash — the
/// hash of step 0 is a pinned parity vector across all ports).
void weftReplayInit(WeftReplayState s) {
  s.latest = 0;
  s.epoch = 0;
  s.wWork = 1;
  s.rWork = 2;
  s.revoked = 0;
  for (final b in s.buf) {
    b.seq = 0;
    b.len = 0;
    b.ver = 1;
  }
  s.tPublish = 0;
  s.tClaim = 0;
  s.tDrop = 0;
  s.tInvalid = 0;
  s.tWsteps = 0;
  s.tRsteps = 0;
  s.tStall = 0;
  s.tTear = 0;
  s.tCanary = 0;
  s.step = 0;
  s.hash = weftReplayFnv1a(weftReplaySerialize(s));
}

/// The C's `weft_replay_state_t next = *s` — a real deep copy (buf slots
/// included); the fold works on this and assigns back ONLY on success.
WeftReplayState _copyOf(WeftReplayState s) {
  final c = WeftReplayState();
  c.latest = s.latest;
  c.epoch = s.epoch;
  c.wWork = s.wWork;
  c.rWork = s.rWork;
  c.revoked = s.revoked;
  for (var i = 0; i < 3; i++) {
    c.buf[i].seq = s.buf[i].seq;
    c.buf[i].len = s.buf[i].len;
    c.buf[i].ver = s.buf[i].ver;
  }
  c.tPublish = s.tPublish;
  c.tClaim = s.tClaim;
  c.tDrop = s.tDrop;
  c.tInvalid = s.tInvalid;
  c.tWsteps = s.tWsteps;
  c.tRsteps = s.tRsteps;
  c.tStall = s.tStall;
  c.tTear = s.tTear;
  c.tCanary = s.tCanary;
  c.step = s.step;
  c.hash = s.hash;
  return c;
}

void _assignBack(WeftReplayState dst, WeftReplayState src) {
  dst.latest = src.latest;
  dst.epoch = src.epoch;
  dst.wWork = src.wWork;
  dst.rWork = src.rWork;
  dst.revoked = src.revoked;
  for (var i = 0; i < 3; i++) {
    dst.buf[i].seq = src.buf[i].seq;
    dst.buf[i].len = src.buf[i].len;
    dst.buf[i].ver = src.buf[i].ver;
  }
  dst.tPublish = src.tPublish;
  dst.tClaim = src.tClaim;
  dst.tDrop = src.tDrop;
  dst.tInvalid = src.tInvalid;
  dst.tWsteps = src.tWsteps;
  dst.tRsteps = src.tRsteps;
  dst.tStall = src.tStall;
  dst.tTear = src.tTear;
  dst.tCanary = src.tCanary;
  dst.step = src.step;
  dst.hash = src.hash;
}

/// Apply ONE event. Returns [ReplayResult.ok] and updates s.hash, or a
/// verdict WITHOUT mutating state (a disagreement leaves the fold at the
/// last good step — the debugger can inspect it).
///
/// `kind` is the RFC-0014 u16 kind; `aux`/`data` are the u32 payload values
/// (masked). The transition table is NORMATIVE (weft_replay.c) — mirror,
/// never redesign.
int weftReplayStep(WeftReplayState s, int kind, int aux, int data) {
  // speculative copy: disagreement leaves the caller's state untouched
  final next = _copyOf(s);
  switch (kind) {
    case WeftTraceKind.publish:
      // buf[w_work] = {seq, aux, v1}; old = latest; latest = w_work;
      // w_work = old. THE exchange, exactly as weft.c's step 4-5.
      next.buf[next.wWork].seq = data & 0xFFFFFFFF;
      next.buf[next.wWork].len = aux & 0xFFFFFFFF;
      next.buf[next.wWork].ver = 1;
      final old = next.latest;
      next.latest = next.wWork;
      next.wWork = old;
      next.revoked = 0; // modeling rule 2: publish implies rebind
      next.tPublish++;
      next.tWsteps++;
    case WeftTraceKind.claim:
      // mine = latest; latest = r_work; r_work = mine.
      final mine = next.latest;
      next.latest = next.rWork;
      next.rWork = mine;
      if (next.buf[mine].seq != (data & 0xFFFFFFFF)) {
        return ReplayResult.disagree; // rule 3: never silent
      }
      next.tClaim++;
      next.tRsteps++;
    case WeftTraceKind.drop:
      // aux is the u16 epoch at ACK — zero-extend (the C's uint16_t aux
      // field widens implicitly; the mask makes it explicit).
      next.epoch = aux & 0xFFFF;
      next.tDrop++;
    case WeftTraceKind.revoke:
      next.revoked = 1;
    case WeftTraceKind.ack:
      next.epoch = data & 0xFFFFFFFF; // epoch after ACK
    case WeftTraceKind.stall:
      next.tStall = (next.tStall + 1) & 0xFFFFFFFF;
    case WeftTraceKind.tear:
      next.tTear = (next.tTear + 1) & 0xFFFFFFFF;
    case WeftTraceKind.canaryFail:
      next.tCanary = (next.tCanary + 1) & 0xFFFFFFFF;
    default:
      return ReplayResult.badKind;
  }
  next.step = (next.step + 1) & 0xFFFFFFFF;
  next.hash = weftReplayFnv1a(weftReplaySerialize(next));
  _assignBack(s, next);
  return ReplayResult.ok;
}

/// Convenience: fold `evs` (kind/aux/data triple lists), appending per-step
/// hashes to `hashesOut` when non-null. Returns the last verdict.
int weftReplayFold(WeftReplayState s, List<List<int>> evs,
    [List<BigInt>? hashesOut]) {
  var rc = ReplayResult.ok;
  for (final e in evs) {
    rc = weftReplayStep(s, e[0], e[1], e[2]);
    if (rc != ReplayResult.ok) return rc;
    hashesOut?.add(s.hash);
  }
  return rc;
}
