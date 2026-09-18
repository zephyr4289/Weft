# chaos-fixtures — RFC 0011 cross-language parity anchors

- `pinned-vectors.json` — the PRNG/pattern vectors every port's selftest
  asserts (C, TS, JVM, Dart, Swift). Regenerate ONLY with a `v` bump and a
  simultaneous port update (a vector change is a breaking contract change).
- `stepped-golden-200k.json` — the byte-exact stepped verdict of the C
  reference oracle for config `steps=200000 slots=4 words=4 readers=2
  frames=200 chaosRate=200 seed=1337`. Every port's CI test (Swift via
  XCTest here; the others live-diff against a fresh C run in
  ci/scripts/run_chaos_parity.sh) must reproduce it exactly.

Regenerate the golden with:

    make -C core/c fanout-chaos
    ./core/c/fanout-chaos stepped 200000 4 4 2 200 200 1337 \
        > tools/chaos-fixtures/stepped-golden-200k.json
