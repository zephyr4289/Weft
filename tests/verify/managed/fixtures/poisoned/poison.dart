// poison.dart — poisoned Dart fixture. Every commented rule id MUST fire.

@weftHot
void onSignal(double x, Float64List out) {
  final scratch = List<double>.filled(64, 0.0); // expect: WV-DT-002
  final box = Map<String, double>();            // expect: WV-DT-003
  final p = calloc<Float>(64);                  // expect: WV-DT-006
  final sb = StringBuffer();                    // expect: WV-DT-005
  final cb = () => out[0] + x;                  // expect: WV-DT-008
  final row = [x, x];                           // expect: WV-DT-004
  out[0] = scratch[0] + box['a']! + p[0] + sb.length + cb() + row[1];
}
