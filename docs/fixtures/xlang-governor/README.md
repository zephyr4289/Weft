# fixtures/xlang-governor — G5 cross-language parity gate (RFC-0009)

The FreshnessGovernor's G-series conformance (RFC-0009 §"V-series-style
conformance") closes with **G5: identical action sequences for identical
`behind` traces across C/Rust/TS** — the same xlang discipline the fan-out
ring used for its byte-compatibility proof (`fixtures/xlang-fanout/`).

## Mechanism

One deterministic trace, three emitters, byte-compare:

```
state = 0x00C0FFEE                      (xorshift32 — 04-LITMUS §0.2)
for i in 0..10_000:
    state   = xorshift32(state)
    behind  = state % 128               (spikes well past snapshotBehind=16)
    now_ms  = i                         (1 ms per step — cooldowns fire)
    action  = governor.step(behind, now_ms)
    emit    (kind << 6) | min(skip_n, 63)   — one byte, hex-encoded
```

| Emitter | Command |
|---|---|
| TS | `node gov_trace.mjs [STEPS [SEED]]` (imports `@weft/core` dist) |
| C | `core/c/governor-test xlang-dump [STEPS [SEED]]` |
| Rust | `core/rust/target/release/governor_xlang [STEPS [SEED]]` |

`run.sh` runs all three and `cmp`s them pairwise, plus an action-class
coverage check (all four kinds must appear — a trace that never leaves
FastPath would make the comparison vacuous).

## Why time injection matters

`step(behind, nowMs)` takes the clock as a PARAMETER. The cooldown is the
governor's only stateful behavior, and clock reads are inherently
machine-local — so G5 injects the trace's own timestamps, making the action
sequence a pure function of the trace and therefore byte-comparable across
languages.

## Run

```
pnpm --filter @weft/core build
make -C core/c governor-test
cargo build --release --manifest-path core/rust/Cargo.toml --bin governor_xlang
bash fixtures/xlang-governor/run.sh            # STEPS=10000 SEED=0x00C0FFEE
```

Wired into CI via `ci/scripts/run_fanout_native_shard.sh` (the native
driver-layer shard).
