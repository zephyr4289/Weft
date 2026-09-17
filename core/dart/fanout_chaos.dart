// fanout_chaos.dart — RFC 0011 deterministic chaos engine, Dart port.
//
// "chaos contract" — mirrors core/c/fanout_chaos.c (the reference oracle)
// micro-step for micro-step, draw for draw. Given the same config, the
// stepped verdict JSON is BYTE-IDENTICAL to the C engine's (verified by
// ci/scripts/run_chaos_parity.sh; the golden fixture lives in
// tools/chaos-fixtures/). Read core/c/fanout_chaos.h for the normative
// contract; this port adds nothing and drops nothing.
//
// u32 discipline: Dart VM ints are 64-bit signed; every contract operation
// masks to the low 32 bits (`& 0xFFFFFFFF`) so the arithmetic is the exact
// analog of C's uint32_t. `>>>` (logical shift right) since Dart 2.14.
// tword's product exceeds 2^32 but stays < 2^63, so the low-32 mask after
// the multiply is exact (no precision loss) — same low-32 result as C.
//
// LAW 4 honesty: the stepped engine models the RFC 0004 protocol exactly
// (FI1/FI2 brackets, the bounded 4-attempt claim loop, latest-wins). The
// real-ring Flutter chaos leg lives in packages/flutter_weft tests; this
// file is the standalone parity oracle (no Flutter dependency).

import 'dart:io';

class ChaosConfig {
  final int seed; // master seed (u32)
  final int steps; // scheduler step budget (stepped mode)
  final int slots; // M — ring depth
  final int words; // W — payload words per slot
  final int readers; // R — concurrent reader SMs
  final int frames; // F — frames the writer publishes
  final int chaosRate; // per-mille fault probability per step

  const ChaosConfig(this.seed, this.steps, this.slots, this.words,
      this.readers, this.frames, this.chaosRate);
}

class ChaosVerdict {
  final bool pass;
  final String engine;
  final String json; // the contract JSON (byte-identical across ports)

  const ChaosVerdict(this.pass, this.engine, this.json);
}

// ---------------------------------------------------------------------------
// The chaos contract: PRNG + pattern (mirror of core/c/fanout_chaos.c)
// ---------------------------------------------------------------------------

const int _u32Mask = 0xFFFFFFFF;

int _mix32(int x0) {
  var x = x0 & _u32Mask;
  x = (x ^ (x >>> 16)) & _u32Mask;
  x = (x * 0x7FEB352D) & _u32Mask;
  x = (x ^ (x >>> 15)) & _u32Mask;
  x = (x * 0x846CA68B) & _u32Mask;
  x = (x ^ (x >>> 16)) & _u32Mask;
  return x;
}

class _ChaosRng {
  int _a = 0, _b = 0, _c = 0, _d = 0;

  void seed(int seed) {
    _a = _mix32(seed ^ 0xA341316C);
    _b = _mix32(seed ^ 0xC8013EA4);
    _c = (_a ^ 0x9E3779B9) & _u32Mask;
    _d = (_b ^ 0x85EBCA6B) & _u32Mask;
  }

  int next() {
    var t = _d;
    final s = _a;
    _d = _c;
    _c = _b;
    _b = s;
    t = (t ^ ((t << 11) & _u32Mask)) & _u32Mask;
    t = (t ^ (t >>> 8)) & _u32Mask;
    _a = (t ^ s ^ (s >>> 10)) & _u32Mask;
    return _a;
  }
}

int _tword(int seq, int w) =>
    _mix32(((seq * 2654435761) + w) & _u32Mask);

// ---------------------------------------------------------------------------
// Stepped engine — the deterministic scheduler (mirror of the C oracle)
// ---------------------------------------------------------------------------

const int _wIdle = 0, _wFill = 1, _wStamp = 2, _wPublish = 3, _wDone = 4;
const int _rIdle = 0, _rStampB = 1, _rCopy = 2, _rStampA = 3, _rAccept = 4;
const int _rTickEnd = 5, _rSkipTick = 6, _rExhaustedTick = 7, _rDone = 8;

const List<int> _freezeAdd = [1, 2, 3, 0]; // preempt/stall/throttle/reorder

ChaosVerdict runSteppedChaos(ChaosConfig cfg) {
  if (cfg.slots < 2 || cfg.slots > 64) {
    throw ArgumentError('chaos: slots out of [2,64]');
  }
  if (cfg.words < 1 || cfg.words > 64) {
    throw ArgumentError('chaos: words out of [1,64]');
  }
  if (cfg.readers < 1 || cfg.readers > 4) {
    throw ArgumentError('chaos: readers out of [1,4]');
  }
  if (cfg.chaosRate < 0 || cfg.chaosRate > 1000) {
    throw ArgumentError('chaos: chaosRate out of [0,1000]');
  }

  final M = cfg.slots, W = cfg.words, R = cfg.readers, F = cfg.frames;

  // Model ring.
  final slotSeq = List<int>.filled(M, 0);
  final payload = List.generate(M, (_) => List<int>.filled(W, 0));
  final bracketOpen = List<bool>.filled(M, false);
  var latest = 0;
  var publishes = 0;

  // Writer SM
  var ws = _wIdle;
  var wSeq = 1;
  var wSlot = 0;
  var wWord = 0;
  var wRev = false;

  // Reader SMs
  final rs = List<int>.filled(R, _rIdle);
  final rLast = List<int>.filled(R, 0);
  final rTarget = List.generate(R, (_) => List<int>.filled(W, 0));
  final rL = List<int>.filled(R, 0);
  final rSlot = List<int>.filled(R, 0);
  final rWord = List<int>.filled(R, 0);
  final rAttempts = List<int>.filled(R, 0);
  final rRevActive = List<bool>.filled(R, false);

  // Scheduler
  final rng = _ChaosRng()..seed(cfg.seed);
  final freeze = List<int>.filled(R + 1, 0);
  final revFlag = List<bool>.filled(R + 1, false);

  // Ledger
  final fresh = List<int>.filled(R, 0);
  final dropped = List<int>.filled(R, 0);
  final skips = List<int>.filled(R, 0);
  final exhausted = List<int>.filled(R, 0);
  var tornAccepted = 0;
  var futureClaims = 0;
  var bracketViolations = 0;
  final injected = List<int>.filled(4, 0);
  var stepsExecuted = 0;

  bool allDone() {
    if (ws != _wDone) return false;
    for (var i = 0; i < R; i++) {
      if (rs[i] != _rDone) return false;
    }
    return true;
  }

  void faultDraw() {
    if (rng.next() % 1000 >= cfg.chaosRate) return;
    final victim = rng.next() % (R + 1);
    final kind = rng.next() % 4;
    freeze[victim] += _freezeAdd[kind];
    if (kind == 3) revFlag[victim] = !revFlag[victim];
    injected[kind]++;
  }

  void writerStep() {
    switch (ws) {
      case _wIdle:
        if (wSeq > F) {
          ws = _wDone;
          return;
        }
        wSlot = (wSeq - 1) % M;
        if (bracketOpen[wSlot]) bracketViolations++; // L-C6
        slotSeq[wSlot] = 0; // FI1a: invalidate FIRST
        bracketOpen[wSlot] = true;
        wRev = revFlag[0]; // fault axis: reorder
        wWord = wRev ? (W - 1) : 0;
        ws = _wFill;
      case _wFill:
        payload[wSlot][wWord] = _tword(wSeq, wWord);
        if (wRev) {
          if (wWord == 0) {
            ws = _wStamp;
          } else {
            wWord--;
          }
        } else {
          wWord++;
          if (wWord == W) ws = _wStamp;
        }
      case _wStamp:
        slotSeq[wSlot] = wSeq; // FI1b: stamp (Release)
        bracketOpen[wSlot] = false;
        ws = _wPublish;
      case _wPublish:
        latest = wSeq; // the publication point
        publishes++;
        wSeq++;
        ws = _wIdle;
    }
  }

  void readerStep(int i) {
    switch (rs[i]) {
      case _rIdle:
        final L = latest;
        if (L == 0 || L == rLast[i]) {
          rs[i] = _rTickEnd;
          return;
        }
        rL[i] = L;
        rAttempts[i] = 0;
        rs[i] = _rStampB;
      case _rStampB:
        final L = rL[i];
        rSlot[i] = (L - 1) % M;
        final sB = slotSeq[rSlot[i]];
        if (sB != L) {
          final l2 = latest;
          if (l2 == L) {
            skips[i]++;
            rs[i] = _rSkipTick;
            return;
          }
          rL[i] = l2;
          rAttempts[i]++;
          rs[i] = rAttempts[i] >= 4 ? _rExhaustedTick : _rStampB;
          return;
        }
        rWord[i] = revFlag[i + 1] ? (W - 1) : 0; // fault axis: reorder
        rRevActive[i] = revFlag[i + 1];
        rs[i] = _rCopy;
      case _rCopy:
        final w = rWord[i];
        rTarget[i][w] = payload[rSlot[i]][w];
        if (rRevActive[i]) {
          if (w == 0) {
            rs[i] = _rStampA;
          } else {
            rWord[i] = w - 1;
          }
        } else {
          rWord[i]++;
          if (rWord[i] == W) rs[i] = _rStampA;
        }
      case _rStampA:
        final sA = slotSeq[rSlot[i]];
        if (sA == rL[i]) {
          rs[i] = _rAccept;
          return;
        }
        rL[i] = latest; // torn copy — chase newest
        rAttempts[i]++;
        rs[i] = rAttempts[i] >= 4 ? _rExhaustedTick : _rStampB;
      case _rAccept:
        final L = rL[i];
        if (L > F) futureClaims++; // L-C2
        for (var w = 0; w < W; w++) {
          if (rTarget[i][w] != _tword(L, w)) {
            tornAccepted++; // L-C1
            break;
          }
        }
        dropped[i] += L - rLast[i] - 1;
        rLast[i] = L;
        fresh[i]++;
        rs[i] = _rTickEnd;
      case _rTickEnd:
        rs[i] = (ws == _wDone && rLast[i] == F) ? _rDone : _rIdle;
      case _rSkipTick:
        rs[i] = _rTickEnd;
      case _rExhaustedTick:
        exhausted[i]++;
        rs[i] = _rTickEnd;
    }
  }

  void advance(int tid) {
    if (tid == 0) {
      writerStep();
    } else {
      readerStep(tid - 1);
    }
  }

  // The scheduler loop (chaos contract shape — identical to the C oracle).
  while (stepsExecuted < cfg.steps && !allDone()) {
    final tid = rng.next() % (R + 1);
    faultDraw();
    if (freeze[tid] > 0) {
      freeze[tid]--; // scheduled, but made no progress
    } else {
      advance(tid);
    }
    stepsExecuted++;
  }

  // Drain: round-robin (writer, reader 1..R), no faults, until done.
  final drainBound = 64 * (F + 16 * R * (F + 8)) + 64;
  var drainedSteps = 0;
  while (!allDone() && drainedSteps < drainBound) {
    for (var tid = 0; tid <= R && !allDone(); tid++) {
      if (freeze[tid] > 0) freeze[tid] = 0; // faults end with the budget
      if (!allDone()) advance(tid);
    }
    drainedSteps++;
  }
  final drained = allDone();

  // L-C3 telescoping + L-C4 completion (adjudicated once, at drain end).
  var telescopingOk = drained;
  for (var i = 0; i < R; i++) {
    if (dropped[i] != rLast[i] - fresh[i]) telescopingOk = false;
    if (drained && rLast[i] != F) telescopingOk = false;
  }
  if (publishes != F) telescopingOk = false; // L-C4

  final pass = telescopingOk &&
      tornAccepted == 0 &&
      futureClaims == 0 &&
      bracketViolations == 0;

  // Contract JSON — byte-identical to the C oracle (fixed field order).
  final json = '{"engine":"weft-chaos-stepped","v":1,"seed":${cfg.seed},'
      '"steps":${cfg.steps},'
      '"slots":$M,"words":$W,"readers":$R,"frames":$F,'
      '"chaosRate":${cfg.chaosRate},'
      '"stepsExecuted":$stepsExecuted,'
      '"injections":{"preempt":${injected[0]},"stall":${injected[1]},'
      '"throttle":${injected[2]},"reorder":${injected[3]}},'
      '"ledger":{"publishes":$publishes,'
      '"fresh":[${fresh.join(",")}],'
      '"dropped":[${dropped.join(",")}],'
      '"lastSeq":[${rLast.join(",")}],'
      '"skips":[${skips.join(",")}],'
      '"exhausted":[${exhausted.join(",")}],'
      '"tornAccepted":$tornAccepted,"futureClaims":$futureClaims,'
      '"bracketViolations":$bracketViolations,"drained":$drained},'
      '"telescoping":"${pass ? "OK" : "VIOLATED"}",'
      '"verdict":"${pass ? "PASS" : "FAIL"}"}';

  return ChaosVerdict(pass, 'stepped', json);
}

// Pinned vectors — MUST match core/c/fanout_chaos.c's selftest (the parity
// anchor; see tools/chaos-fixtures/).
bool chaosSelftest() {
  if (_mix32(0xDEADBEEF) != 3861431939) return false;
  if (_tword(1, 0) != 1834104592) return false;
  if (_tword(7, 3) != 2500287888) return false;
  if (_tword(100, 15) != 4197121613) return false;
  final rng = _ChaosRng()..seed(42);
  const draws = [1034221180, 2302191726, 1921777443, 3822789115, 4193179225,
    3051818586, 2559959645, 2724063783];
  for (final want in draws) {
    if (rng.next() != want) return false;
  }
  final v = runSteppedChaos(
      const ChaosConfig(7, 4000, 2, 2, 2, 4, 300));
  return v.pass && v.json.contains('"verdict":"PASS"');
}

void main(List<String> args) {
  if (args.length >= 8 && args[0] == 'stepped') {
    final cfg = ChaosConfig(
      int.parse(args[7]), // seed
      int.parse(args[1]), // steps
      int.parse(args[2]), // slots
      int.parse(args[3]), // words
      int.parse(args[4]), // readers
      int.parse(args[5]), // frames
      int.parse(args[6]), // chaosRate
    );
    final v = runSteppedChaos(cfg);
    print(v.json);
    stderr.writeln('chaos-stepped(dart): ${v.pass ? "PASS" : "FAIL"}');
    exit(v.pass ? 0 : 1);
  }
  if (args.length == 1 && args[0] == 'selftest') {
    final ok = chaosSelftest();
    stderr.writeln('chaos-selftest(dart): ${ok ? "PASS" : "FAIL"}');
    exit(ok ? 0 : 1);
  }
  stderr.writeln(
      'usage: fanout_chaos.dart stepped <steps> <slots> <words> <readers> '
      '<frames> <chaosRate> <seed> | selftest');
  exit(2);
}
