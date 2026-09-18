# jni-fanout — JVM torture harness for the weft_jni.c fan-out surface

**What this is:** the executable spec for the RFC-0004 fan-out ring's JNI
bridge (`android/weft-core/src/main/cpp/weft_jni.c` over
`core/c/fanout.{h,c}`). It compiles the exact sources the Android build
compiles into a HOST `.so` and drives the full contract through JNI from a
real JVM: the JF-series semantics (geometry, roundtrip, telescoping, graceful
skip, multi-reader independence, foreign-ring attach, fill path) plus a
1-writer x 3-reader threaded torture with the kernel's shared payload
pattern validation and exact telescoping accounting.

**Why it exists:** the repo's Android unit tests run on the host JVM and the
JNI surface had no executable spec outside a device build — and the fan-out
ring had never been exercised through a JNI boundary at all. This fixture
closes that gap the same way `fixtures/xlang-fanout` closed cross-language
byte-layout interop: an independent implementation, runnable anywhere gcc +
a JDK exist (both preinstalled on ubuntu-latest CI runners).

**Relationship to the Kotlin tests:** `FanoutTest.kt`
(android/weft-core/src/test) pins the PURE-JVM ring's semantics through
`gradlew test`; this harness pins the NATIVE path (the C ring + the JNI
wrappers + the direct-ByteBuffer cursor discipline). The two suites mirror
each other's assertions.

**Run** (from the repo root):

```bash
bash fixtures/jni-fanout/run.sh
```

Needs `gcc` and a JDK with `include/jni.h` (`JAVA_HOME` if not on PATH).
Writes the evidence log to `litmus/evidence/fanout/jni-harness.log`.

**Wired into CI:** `ci/scripts/run_fanout_native_shard.sh` runs this fixture
as part of the `fanout-native` shard in `extreme-test.yml`.

**Honesty labels:** the ~300K publishes/s torture line is informative-only
(host JVM, JNI transitions included), not a platform performance claim. The
harness does not run under TSAN with the JVM (the JVM's own memory system
conflicts with sanitizer shadow memory); the C ring's TSAN cleanliness is
proven separately by `core/c/fanout_runner.c` torture evidence.
