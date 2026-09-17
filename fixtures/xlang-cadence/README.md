# fixtures/xlang-cadence — PC3 cross-language cadence parity gate (RFC-0009 §cadence)

The cadence policies' PC-series conformance (RFC-0009 §"Cadence
conformance") closes with **PC3: identical packed decision traces for
identical arrival traces across TS/Kotlin/Swift/Dart** — the same xlang
discipline the governor ladder used for G5 (`fixtures/xlang-governor/`),
extended to the four VM ports (Series 7).

## Mechanism

One deterministic arrival trace, one emitter per port, byte-compare
against the TS reference:

```
state = 0x00C0FFEE                      (xorshift32 — 04-LITMUS §0.2)
for i in 0..10_000:
    state   = xorshift32(state)
    latest += state % 5                 (volatile arrivals: 0..4 per tick)
    for policy in [LATEST_WINS, PACED_INTERPOLATE, BURST_COALESCE]:
        d = policy.step(latest)
        emit byte1 = (present<<7) | (interp<<6) | (alphaQ12 >> 7)
        emit byte2 = min(coalesced, 255)   — hex-encoded, 6 bytes/tick
```

The packing is deliberately lossy-but-sufficient (alphaQ12's high 5 bits,
coalesced capped at 255): a byte-identical log still proves the alpha
ladder, the elision key, the gap EWMA, the reassess hysteresis, and the
Law-4 counters agree — while the per-port batteries pin the FULL decision
record via the FNV-1a-64 trace hashes (G5/PC3 local pins —
`scripts/gen_trace_refs.mjs`).

| Emitter | Command |
|---|---|
| TS (reference) | `node cadence_trace.mjs [STEPS [SEED]]` (imports `@weft/core` dist) |
| Kotlin | `kotlinc core/kotlin/Governor.kt kotlin/CadenceTrace.kt -include-runtime -d <jar> && java -jar <jar>` |
| Swift | `swiftc -O core/swift/Governor.swift swift/CadenceTrace.swift -o <bin>` |
| Dart | `dart dart/CadenceTrace.dart` (zero-dependency script) |

`run.sh` runs the TS reference plus every VM emitter whose toolchain is
present (kotlinc+java / swiftc / dart) and `cmp`s each against the TS
log; a missing toolchain is a DECLARED skip (the workflow that owns the
toolchain runs the same gate with it present — android-packages,
apple-packages, flutter-packages). A policy-coverage check requires all
three policies to actually present (a trace that never left the elide
path would make the comparison vacuous).

## Why ticks-not-clocks matters

`step(latestSeq)` takes no clock: ticks are counted internally. Display
timing is machine-local, so PC3 makes the decision sequence a pure
function of the arrival trace — byte-comparable across languages, exactly
like G5's injected `nowMs`.

## Run

```
pnpm --filter @weft/core build
bash fixtures/xlang-cadence/run.sh            # STEPS=10000 SEED=0x00C0FFEE
```

Wired into CI via `ci/scripts/run_fanout_native_shard.sh` (the native
driver-layer shard, gate 5's cadence leg).
