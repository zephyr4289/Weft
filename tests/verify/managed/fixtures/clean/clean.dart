// clean.dart — clean Dart fixture: ZERO findings expected.

@weftHot
void onSignal(double x, Float64List out, Float64List scratch) {
  var acc = 0.0;
  for (var i = 0; i < 64; i++) {
    scratch[i] = x * i;
    acc += scratch[i];
  }
  out[0] = acc;
  if (acc > 1e9) {
    out[1] = -1.0;
    return;
  }
}
