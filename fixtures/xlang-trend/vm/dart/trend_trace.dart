// trend_trace.dart — RFC 0020 cross-language verdict-stream emitter, Dart side.
//
// Drives the trend estimator with the deterministic behind trace
// (behind = xorshift32(state) % 64, seed 0x00C0FFEE) and prints the packed
// verdict stream (verdict << 6 | min(skip_n, 63), hex, one line) —
// byte-identical to core/c/trend_runner.c (the C reference), trend_emitter.mjs
// (TS), and the Kotlin/Swift VM emitters beside this one.
// fixtures/xlang-trend/run.sh byte-compares them all.
//
// Pinned parity vector (from the C reference — do not "fix" it): the
// 2000-sample hex stream hashes (FNV-1a over the stream) to
// 0x11187b9a02b378ef.
//
// Build & run (no pub get — the trend module has zero imports):
//   dart run fixtures/xlang-trend/vm/dart/trend_trace.dart [STEPS [SEED]]

import '../../../../core/dart/weft_trend.dart';

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

  final t = WeftTrend();
  weftTrendInit(t);
  final out = TrendOut();
  final sb = StringBuffer();
  for (var i = 0; i < steps; i++) {
    seed = _xorshift32(seed);
    final behind = seed % 64; // seed is a u32 value (non-negative)
    weftTrendObserve(t, behind, out);
    sb.write(weftTrendPack(out).toRadixString(16).padLeft(2, '0'));
  }
  // One trailing newline — the exact format of the other emitters.
  print(sb.toString());
}
