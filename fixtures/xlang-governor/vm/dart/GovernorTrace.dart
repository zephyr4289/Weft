// GovernorTrace.dart — G5 ladder-trace emitter (RFC-0009), Dart side.
//
// Emits the packed action log for the deterministic (behind, now_ms) trace
// shared with fixtures/xlang-governor/gov_trace.mjs (TS),
// core/c/governor-test xlang-dump (C), core/rust governor_xlang (Rust),
// and the Kotlin/Swift VM emitters beside this one:
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N: state = xorshift32(state);
//                  behind = state % 128; now_ms = i;
//                  action = governor.step(behind, now_ms)
//                  emit byte (kind << 6) | min(skip_n, 63)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — the exact
// format every other emitter produces. run.sh byte-compares them all.
//
// Build & run (no pub get — the governor module has zero imports):
//   dart fixtures/xlang-governor/vm/dart/GovernorTrace.dart [STEPS [SEED]]

import '../../../../core/dart/governor.dart';

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

  final gov = FreshnessGovernor();
  final sb = StringBuffer();
  var state = seed;
  for (var i = 0; i < steps; i++) {
    state = _xorshift32(state);
    final behind = state % 128;
    final a = gov.step(behind, i);
    final packed = (a.kind << 6) | (a.skipN < 63 ? a.skipN : 63);
    sb.write(packed.toRadixString(16).padLeft(2, '0'));
  }
  // One trailing newline — the exact format of the other emitters.
  print(sb.toString());
}
