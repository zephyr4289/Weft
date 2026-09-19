// trace_events.dart — RFC 0014 kernel-trace emitter (issue #20 task 1), Dart side.
//
// Runs the deterministic scenario shared with the C reference
// (core/c/trace_dump.c) and the TS emitter (trace_emitter.mjs) and prints
// the packed event stream as lowercase hex. run.sh byte-compares all ports.
//
// Toolchain policy (per-port honesty pattern): runs only when dart is
// present; CI owns the leg (flutter-packages workflow).
//
// Build & run (no pub get — the kernel module has zero imports):
//   dart fixtures/xlang-trace/dart/trace_events.dart [N [SEED]]

import '../../../core/dart/weft.dart';

int _seed = 0;

int _xorshift32() {
  var x = _seed;
  if (x == 0) x = 0x9E3779B9;
  x = (x ^ ((x << 13) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  x = (x ^ (x >>> 17)) & 0xFFFFFFFF;
  x = (x ^ ((x << 5)) & 0xFFFFFFFF) & 0xFFFFFFFF;
  _seed = x;
  return x;
}

String _packEvent(int kind, int aux, int data) {
  var hex = '';
  for (final v in [kind, aux, data]) {
    final width = v == kind || v == aux ? 4 : 8;
    final mask = width == 4 ? 0xFFFF : 0xFFFFFFFF;
    final bytes = (v & mask);
    for (var b = 0; b < width ~/ 8; b++) {
      hex += ((bytes >> (8 * b)) & 0xFF).toRadixString(16).padLeft(2, '0');
    }
  }
  return hex;
}

void main(List<String> args) {
  final n = args.isNotEmpty ? int.parse(args[0]) : 2000;
  final seedRaw = args.length > 1 ? args[1] : '0x00C0FFEE';
  final seed = int.parse(seedRaw.startsWith('0x') ? seedRaw.substring(2) : seedRaw, radix: 16);

  final out = StringBuffer();
  final w = Weft(64);
  _seed = seed;
  var seq = 0;
  final revokeStep = n ~/ 2;

  for (var step = 0; step < n; step++) {
    final plen = _xorshift32() % 65;
    seq += 1;
    final cursor = w.wBegin;
    for (var i = 0; i < plen; i++) {
      cursor.setUint8(i, pat(seq, i));
    }
    final r = w.publish(seq, plen);
    if (r == PubResult.droppedRevoked) {
      out.write(_packEvent(3, 0, seq)); // DROP
      out.write(_packEvent(5, 0, w.epochVal)); // ACK
    } else {
      out.write(_packEvent(1, plen, seq)); // PUBLISH
    }
    if (_xorshift32() % 3 == 0) {
      w.claim();
      out.write(_packEvent(2, 0, w.rSeq() & 0xFFFFFFFF)); // CLAIM
      final canary = w.buffers[w.rWork].getInt64(w.bufSize - 8, Endian.little);
      if (canary != w.rSeq()) {
        throw StateError('canary check FAILED at step $step');
      }
    }
    if (step == revokeStep) {
      final e0 = w.epochVal;
      w.revoke();
      out.write(_packEvent(4, 0, e0)); // REVOKE
    }
  }
  print(out.toString());
}
