// CadenceTrace.dart — PC3 cadence-trace emitter (RFC-0009 §cadence), Dart side.
//
// Emits the packed decision log for the deterministic arrival trace shared
// with the TS/Kotlin/Swift emitters (fixtures/xlang-cadence/):
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N:
//     state = xorshift32(state); arrivals = state % 5; latest += arrivals
//     for policy in [LATEST_WINS, PACED_INTERPOLATE, BURST_COALESCE]:
//       d = policy.step(latest)
//       emit byte1 = (present<<7) | (interp<<6) | (alphaQ12 >> 7)
//       emit byte2 = min(coalesced, 255)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — 6 bytes
// per tick, three policies in kind order. run.sh byte-compares all ports.
//
// Build & run (no pub get — the governor module has zero imports):
//   dart fixtures/xlang-cadence/dart/CadenceTrace.dart [STEPS [SEED]]

import '../../../core/dart/governor.dart';

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

  final pols = [
    CadencePolicy(CadenceConfig(CadencePolicyKind.latestWins)),
    CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate)),
    CadencePolicy(CadenceConfig(CadencePolicyKind.burstCoalesce)),
  ];
  final sb = StringBuffer();
  var state = seed;
  var latest = 0;
  for (var i = 0; i < steps; i++) {
    state = _xorshift32(state);
    latest += state % 5;
    for (final p in pols) {
      final a = p.step(latest);
      final b1 = (a.present ? 1 : 0) << 7 |
          (a.interp ? 1 : 0) << 6 |
          (a.alphaQ12 >> 7);
      final b2 = a.coalesced < 255 ? a.coalesced : 255;
      sb.write(b1.toRadixString(16).padLeft(2, '0'));
      sb.write(b2.toRadixString(16).padLeft(2, '0'));
    }
  }
  print(sb.toString());
}
