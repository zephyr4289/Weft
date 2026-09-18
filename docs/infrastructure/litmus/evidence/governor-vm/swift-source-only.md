# Series 7 — RFC-0009 governor + cadence policies, Swift (source-only declaration)

env: x86_64 Linux sandbox — NO Swift toolchain (declared; the repo's
per-port honesty culture, PORTS.md §9). date: 2026-09-17T13:10:00Z

## What exists

- `core/swift/Governor.swift` — the ladder + the three cadence policies,
  arithmetic-identical to governor.ts/c/rs + cadence.ts and the
  Kotlin/Dart twins (same thresholds, same cooldown, same Q12 ladder,
  same non-negative division split).
- `Tests/WeftTests/GovernorTests.swift` — the full G-series + PC-series
  battery (the GovernorTest.kt / governor_test.dart twin), including:
  - G5/PC3 LOCAL trace-hash pins: the canonical xorshift32 traces hashed
    FNV-1a-64 from the TS reference (ladder 0x3c33156204c7cfdf over
    10,000 bytes; cadence 0x6f654c298cbcc9f4 over 60,000 bytes —
    scripts/gen_trace_refs.mjs). Any arithmetic drift in this port fails
    the battery the moment a toolchain runs it — no other port needed.
  - G4/PC4 identity audits (`===` on the decision record) — Swift has no
    portable allocation counter; the JVM allocated-bytes audit is the
    Kotlin leg's proof, the trace-hash pins are this leg's (instruments
    on device is the deferred road).
- `fixtures/xlang-governor/vm/swift/GovernorTrace.swift` +
  `fixtures/xlang-cadence/swift/CadenceTrace.swift` — the G5/PC3 emitters
  (standalone `swiftc -O core/swift/Governor.swift <emitter>`; pure
  Foundation, no swift-atomics).

## Where it runs

- `swift test` (apple-packages CI) picks the battery up via the WeftCore
  target (Package.swift already compiles core/swift).
- The emitters byte-compare against the TS reference inside
  `fixtures/xlang-governor/run.sh` / `fixtures/xlang-cadence/run.sh` when
  a Swift toolchain is present (best-effort, declared skip otherwise —
  the same shard pattern as the verifiedweft Swift leg).

## Local verification status

- Line-by-line arithmetic review against Governor.kt (committed, 22/22
  JVM) and cadence.ts (committed, TS pin source) — same packing, same
  clamps, same truncation preconditions.
- NOT compiled in this sandbox (no swiftc). First compile gate:
  apple-packages CI on this branch.
