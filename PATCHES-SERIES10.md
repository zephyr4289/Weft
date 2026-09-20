# PATCHES — Series 10: The Observability Fabric & Zero-Alloc Flow

**Branch:** `contrib/series10-observability` (base: main @ `db24254`, post PR #35/#37)
**Mandate:** Observability Fabric, Time-Travel Engine & Zero-Alloc Declarative
Stream Graph — full architectural authority over the developer-experience,
observability, and reactive-dataflow frontier.
**Result:** 12 independent patches across 5 new RFCs (0016–0020). Every
primary mandate delivered with in-tree, reproducible evidence. Zero
guardrail regressions: the frozen kernel (`weft.c`/`weft.h`) untouched
byte-for-byte, the 4/4 heddle contract intact, all prior suites green
(litmus, canary, chaos-parity, binding parity).

---

## The mission, mapped to what landed

| Mandate deliverable | RFC | Patches | Evidence |
|---|---|---|---|
| Continuous lock-free flight recorder | 0016 | P1 | T-series 42 checks (release/debug/ASan); wait-free emit (no CAS, no loop, no alloc); concurrency closure `emitted == drained + lapped` exact over 200k events |
| Perfetto / Chrome-tracing integration | 0016 §7 | P2, P11 | Golden-hash gate 4/4; byte-identical output pinned cross-process; 1037-event flight demo renders to ui.perfetto.dev |
| Deterministic time-travel replay & state reconstruction | 0019 | P3, P7, P8, P9 | R-series 28 checks; **all six runtimes fold byte-identically** — pinned vectors: init `8a769a0111cf3af3`, 100k soak `26beb484733ecde0` |
| Zero-alloc declarative stream operators (`weft_flow`) | 0017 | P4, P7 | O-series 37 checks; pipeline checksum pinned `16a50e3abf678f35`; grep-proof: no malloc, no clock, no unbounded loop |
| Multi-stream temporal synchronization (sensor fusion) | 0018 | P5, P7 | Y-series 36 checks; the mission scenario — 60 Hz video + 100 Hz audio + 200 Hz IMU over 10 s → 599 coherent tuples, zero gaps, checksum pinned `144dd8920bcd3fb0` |
| Real-Time Visual HUD / DevTools overlay | 0016 §9 | P10 | 5 binding tests; zero-GC render (preallocated Float32 rings, no React state per frame); fail-safe sampler contract |
| Predictive Cadence & Adaptive Governor AI | 0020 | P6, P7, P8, P9 | D-series 23 checks; predictive lead: RISING (skip 6, projecting 7.4) fires at raw behind 4 — ~12 steps before Snapshot territory; verdict stream pinned `11187b9a02b378ef` |
| Cross-Language Ergonomic DSLs | 0017 §Port surface | P4 §docs, P7–P9 | The operator core + fluent builders per runtime; parity via the same fixtures (no separate truth) |

---

## Patch ledger

### Patch 1 — RFC 0016: `weft_trace`, the continuous lock-free flight recorder `d38fe94`

**The design that makes "zero lock contention" structural, not statistical:**
every producer thread owns a private lossy SPSC shard; emission writes the
slot payload with relaxed atomics and publishes with ONE Release store of
the slot's seqlock-parity stamp. The producer never reads shared mutable
state — there is no shared cursor to CAS. Worst-case emit ≈ one dirty cache
line. The kernel's forbidden patterns are all trivially absent: the recorder
contains zero atomics that participate in any ownership decision.

- Overwrite-oldest backlog policy (a flight recorder keeps the newest
  history), lap accounting EXACT via newest-scan resync — T3 pins
  `t_lap == N - cap` to the event.
- K-way merge drain (bounded, caller-buffered, single-retry torn-read
  resync, lossy-skip counted — Law 1 on the consumer side too).
- TWO export surfaces per the RFC-0014 sidecar provision: the `.weftrec`
  v4 container stays a pure function of the kernel scenario (byte-identity
  preserved — T4 proves `memcmp ==` against direct RFC-0014 encoding);
  runtime facts (timestamps, producer lanes, governor/trend/vsync kinds)
  live in the new `.wsid` v1 sidecar. Runtime kinds `[16, 0x8000)` —
  disjoint from kernel kinds 1..8, the gap is a tested invariant.
- `weft_trace_init_at`: the placement/mmap path — crash-tolerant
  post-mortem with zero allocation (T6).
- Shard-binding discipline (cross-thread emit = caller error): debug build
  counts violations, release build compiles the probe out entirely.
- **`weft_trace_test.c` (T-series, 42 checks):** order, merge, lap
  exactness, v4 byte-identity, sidecar CRC + back-refs, placement init,
  binding, and T8 — the concurrency closure
  `emitted == drained + lapped` holds exactly over 200k events with a
  concurrent drainer, plus the no-unbounded-stall probe (emit p_max
  bounded; the single-store design cannot block).

### Patch 2 — RFC 0016 §7: the Perfetto bridge `5ba2531`

`tools/perfetto/weftrec2perfetto.mjs` — zero-dependency Node 18+:

- Strict parsers: version gates (v1/v2/v3 REFUSED — the RFC-0014 rule
  works both directions), header + per-record CRC verification.
- Lane model: writer / reader / governor / display / host / user /
  counters, fixed tid assignment (part of the determinism contract).
- Async flow links `publish(seq) → claim|drop|ack(seq)` — the VSYNC-to-
  latency story ui.perfetto.dev renders interactively.
- Counter tracks: `ring_depth`, `freshness`, `claim_latency`, `drops`.
- **Determinism (Law 4):** integer-only arithmetic, fixed key order,
  stable `(ts, ph-rank, lane, name)` sort — byte-identical JSON, pinned by
  `golden/trace.json` AND a cross-process sha256. Event-order time
  fallback (1 position = 1 ms) when no sidecar is present: a declared
  visualization convention, never a timing claim.

### Patch 3 — RFC 0019: deterministic time-travel replay `66c0a7c`

The black-box complaint, answered: a **pure fold** over a `.weftrec` v4
stream reconstructs the full shadow kernel state after every event —
`latest`, `epoch`, `w_work`, `r_work`, the per-buffer `(seq, len, ver)`
triple, all telemetry counters — reduced to a 64-bit FNV-1a hash over a
111-byte canonical serialization.

- The modeling contract (RFC-0019): null-frame payload modeled 0; a
  successful PUBLISH implies an unrevoked writer; claim validation is
  load-bearing — a trace/model DISAGREEMENT is a hard, inspectable error,
  never silently ignored.
- Checkpoint jumps: struct-copy checkpoints every 64 steps; `jump(k)` =
  restore + bounded refold (R7 sweeps the equivalence across 2000 events).
- `replay_runner.c`: the G5-style hash-log emitter + the `--file/--jump`
  post-mortem debugger (replay a production capture, print the shadow
  state at any step).
- **Pinned cross-port vectors:** init `8a769a0111cf3af3`, 100k soak
  `26beb484733ecde0` — every runtime MUST land on exactly these.

### Patch 4 — RFC 0017: `weft_flow`, zero-alloc declarative operators `8b2c4eb`

map / filter / window (tumbling + sliding + flush) / demux (K ≤ 8 sinks) /
zip (tolerance + latest-wins coalescing) / the filter→map→window pipeline —
over 24-byte BORROWED views, every buffer caller-provided at setup.

- Refuse-whole overflow (the TIER4 refusal precedent: no partial frames).
- Decided-drop vs overflow accounting split (the governor's honesty rule).
- The scratch RING: a window following a map no longer aliases the scratch
  it holds — zero-alloc preserved, correctness restored.
- **O-series 37 checks**, 10k-view pipeline checksum pinned
  `16a50e3abf678f35`. Reviewable Law 2: grep the file — no malloc, no
  clock, no unbounded loop.

### Patch 5 — RFC 0018: `weft_sync`, sensor-fusion synchronizer `99d98aa`

N ≤ 8 independent rings → coherent, timestamp-aligned tuples with O(N)
latest-wins state — ONE unconsumed sample per stream. No queues, no locks,
no allocator, no clock reads.

- Pivot policies (closed set): `MAX_TS` / `MIN_TS` / `ANCHOR` — anchor
  ticks rule the cadence (the mission config: video rules; audio and IMU
  follow).
- Decided-drop honesty: laggard releases (MAX/MIN), out-of-window follower
  releases (ANCHOR) → `t_gap`; out-of-order refused → `t_stale`;
  unconsumed replacements coalesced → `t_coalesced`. Tuples preserve the
  ORIGINAL t_ns — alignment is not rewriting.
- **Y-series 36 checks:** the 10-second mission scenario (60 Hz video +
  100 Hz audio + 200 Hz IMU) → 599 coherent tuples (the tick-0 arming
  boundary), zero gaps, coalescing exact, fusion checksum pinned
  `144dd8920bcd3fb0`.

### Patch 6 — RFC 0020: `weft_trend`, the predictive lag-trend AI `d57cad7`

RFC-0009's governor is reactive by design; the trend estimator adds the
missing half — a 1-D alpha-beta filter in pure integer Q16 (level = EWMA of
staleness, slope = EWMA of its change) projecting 8 steps ahead.

- CLOSED verdict set: STABLE / RISING / FALLING / BURST with a proactive
  `skip_n`. Slope guards (|slope| ≥ 0.5/step) keep plateaus STABLE — the
  EWMA slope stalls at a tiny nonzero residue on constant signals, and the
  guard is what separates "constant" from "trending".
- The predictive lead, pinned: plateau 2 → ramp +1/step gives RISING
  (skip 6, projecting 7.4) at raw behind 4 — at the Skip rung, ~12 steps
  before Snapshot territory. Snapshot-class reactions become Skip-class.
- The governor ladder is untouched (Law 3): the trend is a sensor the
  consumer MAY consult; the trace sidecar carries verdicts (kind 18).
- **D-series 23 checks** + hand-computed Q16 vectors + the pinned verdict
  stream `11187b9a02b378ef`.

### Patch 7 — TS ports with pinned parity `f14982a`

`packages/core`: `replay.ts` (BigInt FNV-1a — u64 exact; DataView LE
serialization; speculative-copy semantics) and `trend.ts` (double-exact
integer arithmetic with `Math.floor` matching C's arithmetic shift).
**The C pins reproduce exactly in TS** — R-series and D-series green
inside the package suite (160/160). Also fixes the latent `./weft.ts`
import in `trace.ts` that the DTS bundle surfaced when replay first
pulled it in.

### Patch 8 — the cross-language parity fixtures `0bf5951`

`fixtures/xlang-replay` + `fixtures/xlang-trend` — the G5 pattern applied
to state reconstruction and prediction:

- The normative scenario grammar is IN the run.sh header — no port can
  drift and claim compliance.
- C + TS legs byte-identical over 10k folds (160,001 bytes) and 10k
  verdict samples (20,001 bytes). Rust/VM legs are DECLARED skips when
  their toolchains are absent — the workflows owning those toolchains run
  the same gates with them present.

### Patch 9 — Rust / Kotlin / Swift / Dart: all six runtimes fold identically `4dd9317`

- **Rust** (`core/rust/src/{replay,trend}.rs` + `replay_xlang` /
  `trend_xlang` bins + pinned-vector test suites): zero-dep, no unsafe,
  speculative-clone semantics; CI-compiled.
- **Kotlin / Swift / Dart** (core ports + VM emitters under
  `fixtures/xlang-{replay,trend}/vm/`): mirror the GovernorTrace emitter
  patterns exactly; the arithmetic was PROVEN pre-translation by an
  op-for-op Node simulation against the C binaries — 5/5 pinned vectors
  and a 60/60 seed×steps sweep — then transcribed. Compile surface is
  CI-gated (android/apple/flutter shards), honestly marked
  `STATUS: SOURCE-ONLY` in every header.

### Patch 10 — `WeftHud`: the fail-safe zero-GC DevTools overlay `407b0cb`

A drop-in React overlay rendering DEPTH / BEHIND / DROP-per-frame
sparklines plus the trend verdict badge, on top of the app.

- **Zero-GC render:** preallocated Float32Array rings, imperative canvas
  repaint, no React state in the hot path, no reconciliation per frame.
- **Law 3 honored:** the HUD takes a host `sample()` callback — it never
  touches a Weft; the host decides where numbers come from (debug view,
  trace shards, trend estimator).
- **Fail-safe by contract:** a throwing sampler is counted (SAMP! flag)
  and the loop keeps running — a DevTools overlay must never take the app
  down.
- Latest-ref hardening (the WeftCanvas pattern): a new sample closure
  never restarts the loop. 5 binding tests; 16/16 package suite green.

### Patch 11 — the Series 10 CI shard + full-fabric flight demo `e0162c9`, `1978ce3`

- `ci/scripts/run_series10_shard.sh` — one gate for the whole fabric:
  5 C series suites (release + ASan + debug), the TS pinned-parity suite,
  the Perfetto golden gate, both xlang parity fixtures, and the flight
  demo. Registered as shard `series10-observability` in the extreme-test
  matrix.
- `flight_runner.c` — the fabric in one process: the frozen kernel, the
  recorder, the governor, and the trend estimator over ONE deterministic
  600-frame scenario (healthy → lag → stall → recovery, injected time),
  exhaustively drained to **1037 kernel events + 2753 observations**,
  exported to a validated `.weftrec` + `.wsid`, rendered to Perfetto JSON.
  This is the evidence bundle a developer loads into ui.perfetto.dev to
  SEE the lag phase, the trend verdicts escalating, the governor ladder
  reacting, and the recovery — the black box, opened.
- `weftrec2perfetto` header-parse fix (u16 version @4 + u16 header_size
  @6, per the codec's `writer_open`); the selftest encoder shared the
  same wrong guess; golden output byte-identical (same sha256).

---

## Guardrail compliance (the constitution)

- **Law 1 (no unbounded spins):** emit has no loop; drain/window/demux/
  sync/trend are bounded by declared constants; overflow and laps are
  counted, never spun on.
- **Law 2 (zero hot-path allocation):** emit/drain/export/operators/
  synchronizer/estimator allocate nothing — init is the only allocator
  (kernel precedent); the HUD's only allocations are at mount.
- **Law 3 (mechanism, not policy):** `weft.c`/`weft.h` are byte-frozen and
  byte-identical to upstream — verified by the binding-parity gate. All
  new code lives in the driver/runtime layers (`core/c/weft_trace_*`,
  `weft_flow_*`, `weft_sync_*`, `weft_replay_*`, `weft_trend_*`,
  `tools/perfetto`, `packages/*`, `fixtures/*`).
- **Law 4 (deterministic testing):** no wall-clock reads anywhere in the
  new code (t_ns injected; the flight demo's 60 Hz is a constant). Every
  behavior is pinned by counted checks, golden hashes, or cross-runtime
  byte-compare.

## Verification summary

| Gate | Result |
|---|---|
| T-series (flight recorder) | 42/42, release + debug + ASan |
| R-series (time-travel fold) | 28/28, release + ASan |
| O-series (flow operators) | 37/37, release + ASan |
| Y-series (sensor fusion) | 36/36, release + ASan |
| D-series (trend estimator) | 23/23, release + ASan |
| @weft/core suite | 160/160 |
| @weft/react suite | 16/16 |
| Perfetto golden gate | 4/4 (pinned sha256) |
| xlang-replay parity (C ≡ TS) | PASS — 160,001 bytes identical |
| xlang-trend parity (C ≡ TS) | PASS — 20,001 bytes identical |
| Port arithmetic proof (Node sim vs C) | 5/5 pinned vectors, 60/60 sweep |
| Full-fabric flight demo | 1037 events → validated container + sidecar → 404 KB Perfetto JSON |
| Frozen kernel | untouched — byte-identical upstream |
