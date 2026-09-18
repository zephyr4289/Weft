// BlendVerify.dart — Dart verifier for the xlang-blend golden vectors.
//
// Replays the C kernel's deterministic buffer construction (blend_test.c
// fill_random, seed 0x5EEDBEEF), blends with blend_q12.dart's blendWords,
// and FNV-1a-64 digests the output — the digest MUST equal the C kernel's
// for every (size, alpha) row of golden-vectors.csv.
//
// Run (zero-dependency — no pub get):
//   dart fixtures/xlang-blend/dart/BlendVerify.dart [CSV]

import '../../../core/dart/blend_q12.dart';
import 'dart:io';

int _xorshift32(int x0) {
  var x = x0 & 0xFFFFFFFF;
  x = (x ^ ((x << 13) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  x = (x ^ (x >>> 17)) & 0xFFFFFFFF;
  x = (x ^ ((x << 5) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  return x & 0xFFFFFFFF;
}

void main(List<String> args) {
  final csv = args.isNotEmpty ? args[0] : 'golden-vectors.csv';
  final lines = File(csv)
      .readAsLinesSync()
      .where((l) => RegExp(r'^[0-9]+,').hasMatch(l));
  var pass = 0, fail = 0;
  for (final line in lines) {
    final parts = line.trim().split(',');
    final words = int.parse(parts[0]);
    final alpha = int.parse(parts[1]);
    final want = parts[2];
    var state = 0x5EEDBEEF;
    final prev = List<int>.filled(words + 8, 0);
    final newest = List<int>.filled(words + 8, 0);
    for (var i = 0; i < words + 8; i++) {
      state = _xorshift32(state);
      prev[i] = state;
      state = _xorshift32(state);
      newest[i] = state;
    }
    final out = List<int>.filled(words + 8, 0);
    blendWords(prev, newest, alpha, out);
    final got = fnv1a64Words(out.sublist(0, words));
    if (got == want) {
      pass++;
    } else {
      fail++;
      stderr.writeln(
          'MISMATCH size=$words alpha=$alpha: want $want got $got');
    }
  }
  stdout.writeln(
      'xlang-blend Dart verifier: $pass/${pass + fail} golden digests match');
  exit(fail > 0 ? 1 : 0);
}
