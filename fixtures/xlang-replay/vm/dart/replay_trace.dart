// replay_trace.dart — RFC 0019 time-travel replay hash-log emitter, Dart side.
//
// Folds the deterministic RFC-0019 fixture scenario (the normative grammar
// below, xorshift32-seeded — the repo's canonical 04-LITMUS §0.2 generator)
// and prints the per-step u64 state hashes as one lowercase-hex line —
// byte-identical to core/c/replay_runner.c (the C reference), the Rust
// replay_xlang bin, replay_emitter.mjs (TS), and the Kotlin/Swift VM
// emitters beside this one. fixtures/xlang-replay/run.sh byte-compares
// them all.
//
// NORMATIVE SCENARIO GRAMMAR (mirror of replay_runner.c's scen_next —
// every emitter implements this EXACTLY):
//
//   state = SEED; latest=0 w_work=1 r_work=2 epoch=0 revoked=0 seq=0
//   bufseq = [0,0,0]                      # shadow seq per buffer slot
//   for i in 0..N:
//     state = xorshift32(state); op = state & 15; u = state (unsigned)
//     op < 7   : seq++; len = (u >> 4) % 1024
//                if !revoked: PUBLISH(aux=len, data=seq);
//                  bufseq[w_work] = seq; (latest,w_work) = (w_work,latest)
//                else: epoch++; DROP(aux=epoch & 0xffff, data=seq)
//     op < 12  : CLAIM(data=bufseq[latest]);
//                (latest,r_work) = (r_work,latest)
//     op == 12 : !revoked: REVOKE(data=epoch); revoked=1
//                else:     ACK(data=epoch)
//     op == 13 : revoked: ACK(data=epoch); revoked=0
//                else:    STALL(data=(u >> 4) % 8)
//     op == 14 : TEAR(data=seq)
//     else     : CANARY_FAIL(data=seq)
//
// The scenario mirror keeps its own shadow (same exchange rules as the
// fold) so CLAIM events carry the seq the model will reconstruct — the
// fold must therefore NEVER disagree; a disagreement is a hard error.
//
// Pinned parity vectors (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (this grammar, seed 0x00C0FFEE)
//
// Build & run (no pub get — the replay module has zero imports):
//   dart run fixtures/xlang-replay/vm/dart/replay_trace.dart [STEPS [SEED]]

import 'dart:io';

import '../../../../core/dart/weft_replay.dart';

int _xorshift32(int x0) {
  var x = x0 & 0xFFFFFFFF;
  x = (x ^ ((x << 13) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  x = (x ^ (x >>> 17)) & 0xFFFFFFFF;
  x = (x ^ ((x << 5) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  return x & 0xFFFFFFFF;
}

void main(List<String> args) {
  final steps = args.isNotEmpty ? int.parse(args[0]) : 10000;
  var seed = 0x00C0FFEE;
  if (args.length > 1) {
    seed = args[1].startsWith('0x')
        ? int.parse(args[1].substring(2), radix: 16)
        : int.parse(args[1]);
  }

  // scenario shadow (RFC-0019 fixture grammar — normative table above).
  // u32 values are kept masked non-negative; the fold receives the exact
  // 32-bit patterns.
  var latest = 0, wWork = 1, rWork = 2;
  var epoch = 0;
  var revoked = false;
  var seq = 0;
  final bufseq = [0, 0, 0];

  final s = WeftReplayState();
  weftReplayInit(s);
  final sb = StringBuffer();
  for (var i = 0; i < steps; i++) {
    seed = _xorshift32(seed);
    final u = seed; // u32 value (non-negative after the masks above)
    final op = u & 15;
    var kind = 0, aux = 0, data = 0;
    if (op < 7) {
      seq = (seq + 1) & 0xFFFFFFFF;
      final len = (u >>> 4) % 1024;
      if (!revoked) {
        bufseq[wWork] = seq;
        final old = latest;
        latest = wWork;
        wWork = old;
        kind = WeftTraceKind.publish;
        aux = len;
        data = seq;
      } else {
        epoch = (epoch + 1) & 0xFFFFFFFF;
        kind = WeftTraceKind.drop;
        aux = epoch & 0xFFFF;
        data = seq;
      }
    } else if (op < 12) {
      data = bufseq[latest];
      final mine = latest;
      latest = rWork;
      rWork = mine;
      kind = WeftTraceKind.claim;
    } else if (op == 12) {
      if (!revoked) {
        revoked = true;
        kind = WeftTraceKind.revoke;
        data = epoch;
      } else {
        kind = WeftTraceKind.ack;
        data = epoch;
      }
    } else if (op == 13) {
      if (revoked) {
        revoked = false;
        kind = WeftTraceKind.ack;
        data = epoch;
      } else {
        kind = WeftTraceKind.stall;
        data = (u >>> 4) % 8;
      }
    } else if (op == 14) {
      kind = WeftTraceKind.tear;
      data = seq;
    } else {
      kind = WeftTraceKind.canaryFail;
      data = seq;
    }
    final rc = weftReplayStep(s, kind, aux, data);
    if (rc != ReplayResult.ok) {
      stderr.write('replay_emitter: fold disagreement at step $i\n');
      exit(1);
    }
    // u64 rendered UNSIGNED — 16 lowercase hex digits (the hash is a
    // BigInt; toRadixString(16) is lowercase, padLeft zero-fills).
    sb.write(s.hash.toRadixString(16).padLeft(16, '0'));
  }
  // One trailing newline — the exact format of the other emitters.
  print(sb.toString());
}
