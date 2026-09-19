# PATCHES — TIER 4 + TIER 5 (Issues #19 & #20)

**Branch:** `contrib/tier45-security-observability` (base: main @ `cdf2b0c`)
**Issues:** [#19 — Tier 4: Security & Hardening](https://github.com/zephyr4289/Weft/issues/19) · [#20 — Tier 5: Observability & Tooling](https://github.com/zephyr4289/Weft/issues/20)
**Result:** 11 independent patches, every acceptance criterion of both
issues met with in-tree, reproducible evidence. Zero guardrail regressions:
binding parity 27/27, heddles 4/4, litmus C 8/8 + TS 8/8 (+ Rust in CI),
chaos-parity byte-for-byte, guardian wire 52 fields, silent-green audit
clean.

---

## Series A — Issue #19: Security & Hardening

### Patch 1 — XOR boundary canary (all 6 ports) `8f7618c`

**Fixes:** the plain `canary = seq` tail word was structurally blind to
`version` and `payload_len` corruption — a flipped bit in either field left
the canary equality intact while the reader trusted a frame that was never
whole.

- `canary = (u64)seq ^ WEFT_MAGIC ^ (u64)version ^ (u64)payload_len`,
  single-sourced (`weft_canary_compute`) and mirrored byte-for-byte in C,
  TS, Kotlin, Swift, Dart, Rust. All components are zero-extended u32 ⇒
  the u64's hi half is always 0 (the wire contract the guardian manifest
  now documents and the wire probe observes — 52-field check PASS).
- `weft_r_canary_check()` (+ ports) recomputes from the LIVE held envelope;
  fail-closed on null buffer / wrong magic.
- Null frames carry the REAL boundary canary (04-LITMUS §0.6 invariant
  preserved: the null frame is a valid initial state, not a sentinel).
- Litmus runners (C/TS/Rust) verify the recomputed boundary AND pin against
  the expected frame.
- **`canary_test.c` (C-series, 456 checks): 176 single-bit fault
  injections across {magic, version, seq, payload_len, canary} — ZERO
  false negatives**; multi-bit flips detected; declared-scope honesty
  (header_size is the decoder's job — asserted, not hidden) and the
  flip-and-compensate boundary (keyed-MAC problem — VerifiedWeft's HMAC
  path; the compensated frame is still caught by the payload-pattern
  layer).
- New CI shard `canary-hardening` (fault matrix + guardian wire agreement).

### Patch 2 — Input validation wall + fuzz corpus `dc31862`

- `weft_init`: NULL `w`, `payload_max == 0`, `payload_max > 1 MiB`
  (`WEFT_PAYLOAD_MAX_LIMIT`) refused BEFORE any allocation.
- `weft_publish`: `payload_len > payload_max` refused WHOLE
  (`WEFT_PUB_INVALID` + `t_invalid`) before any byte write; the normative
  02 §6 revoked-first ACK ordering preserved.
- `weft_w_write_payload`: NULL src with nonzero len refused; len==0
  memcpy elided. `weft_destroy`: NULL guard.
- `weft_fanout_claim`: defensive geometry revalidation — corrupt or
  hand-rolled reader structs (FFI hosts build these from foreign memory)
  fail CLOSED, never index OOB. VM ports declare the divergence honestly
  (their reader objects cannot corrupt without VM UB).
- Ports: constructor-level refusal (TS `RangeError` / Kotlin `require` /
  Swift `precondition` / Dart `ArgumentError` / Rust `None`) + `Invalid`
  publish verdict + `t_invalid` in all five.
- **`fuzz_runner.c`: deterministic seeded adversary** over init geometry,
  publish bounds, payload writes, slice reads, canary checks, fanout
  geometry — 5 declared post-conditions (P1–P5) after every op. Under
  ASan+UBSan: **1,496,728 ops / 5,408,704 post-checks / zero findings.**
  libFuzzer entry via `-DWEFT_FUZZ_LIBFUZZER`. The harness's own first
  draft was caught by its own ASan run (read-slice caller obligation) —
  the fuzz process eats its own dog food.

### Patch 3 — Bounded revocation `dba4ae1`

- `weft_reclaim` returns within **min(caller bound, ceiling)**; ceiling
  default 1000 ms, runtime-configurable (`weft_set_max_reclaim_timeout`;
  0 disables — declared).
- Named verdicts `WEFT_RECLAIM_ACK / TIMEOUT / INVALID` (legacy `rc == 0`
  contract preserved). TIMEOUT ⇒ caller MUST NOT poison/free (the A1
  hazard is live). Env-gated warning + `t_reclaim_timeouts` — never
  silent. Bound honesty: 100 µs poll granularity declared.
- Ports mirror ceiling + counting; Dart's async path (where waiting is
  real) gains the clamp + counter; the sync path keeps its documented
  instant-return semantics (single-isolate busy-polls can never observe
  the ACK — the zero-width bound is the honest bounded wait).
- **`reclaim_test.c` (R-series, 15 checks): clamp MEASURED** (5000 ms
  caller bound returns at ~1000 ms ceiling), timeout telemetry exact,
  ceiling 0 disables, setter visible, NULL guard.

### Patch 4 — Sanitizer matrix `53eef65`

- `run_sanitizers_shard.sh`: **ASan+UBSan joint
  (`-fno-sanitize-recover=all`) + TSan, 20 cells, zero tolerance** —
  litmus, canary, reclaim, fanout, governor, verified (RFC 4231 vectors),
  blend, fuzz, ffi_host (both modes). Post-run scan: any sanitizer line in
  the log is RED even if the binary exited 0. No suppression files exist;
  none are allowed.

### Patch 5 — FFI boundary isolation host `4b0b690`

- `ffi_host.{h,c}`: worker isolate + bounded mailbox (one request slot +
  one response slot, single-flight backpressure — never a silent queue);
  fixed-size value blobs cross the seam, no pointers either direction.
- **Every call bounded**: hung guest ⇒ `WEFT_FFI_TIMEOUT` at the caller's
  bound (F2: 100 ms bound honored, not 10 s); worker cancelled + REPLACED;
  host survives; restart counted.
- **Fork containment**: guest runs in a forked child; `abort()` ⇒
  `WEFT_FFI_CRASH` and the parent keeps running (F3 proves it in-tree).
  Kill deadline = caller's bound (SIGKILL + reap).
- **Overhead contract measured**: in-process dispatch **6–9 µs/call**
  (2000-call loop) — 100×+ inside the issue's <1 ms bar; F5 fails the
  build if violated. Distinct verdicts + advisory counters (Law 1).

### Patch 6 — docs(security) `add2d30`

- `docs/security/tier4-hardening.md`: threat model (T1–T5), one task →
  one threat → one gate, full acceptance-criteria checklist with evidence
  paths. All 14 boxes checked.

---

## Series B — Issue #20: Observability & Tooling

### Patch 7 — RFC 0014: .weftrec v4 kernel trace events `dec4a76`

- New format version per FORMATS §1.3 (v1–v3 untouched; version gate
  enforced both ways). 32-byte header (TRACE flag, event_count, header
  CRC) + fixed-width 12-byte records (u16 kind | u16 aux | u32 data |
  CRC-32/zlib over the first 8). Kinds: publish, claim, drop, revoke,
  ack, stall, tear, canary_fail.
- **The parity scenario (normative)**: xorshift32-seeded single-threaded
  script over the public kernel API — no scheduler noise, no wall clock —
  so the packed stream is a pure function of (N, seed) and **cross-port
  byte-identity is provable, not statistical**.
- C reference codec `trace_rec.{h,c}` (writer, validator, decoder, JSON
  exporter) + `trace-dump` (hex/file/json/validate modes) + T-series
  battery (47 checks; 96/96 single-bit record corruptions caught; CRC
  canonical vector).
- Ports: TS `trace.ts` (+ packages/core mirror); Kotlin/Dart/Swift
  scenario emitters (Dart kernel gained public `epochVal` + `pat` —
  API parity with `weft_epoch`/`weft_pat`).
- **`fixtures/xlang-trace`: C == TS byte-identical at 200 and 20,000
  steps, two seeds (36,730 events)**; VM legs declared-skip without local
  toolchains, CI owns them (the repo's per-port honesty pattern).
- `schemas/weftrec-trace.schema.json`: the human-readable export contract,
  shape-validated in CI. New shard `trace-standard` (matrix → 30 shards).

### Patch 8 — Prometheus / Grafana live metrics `54c4e51`

- `core/c/metrics_dump`: kernel telemetry + LIVE measured publish/claim
  latency quantiles in the Prometheus TEXT format (validated line-by-line;
  11 series). Measured here: claim P50 0.040 µs / P99 0.039 µs @ 256 B.
- `tools/prometheus/weft_exporter.py` (stdlib-only, :9108) +
  `grafana-dashboard.json` (six panels incl. the TIER4 counters as
  first-class production metrics). `WEFT_METRICS_CMD` accepts any app's
  wrapper — "works with any Weft app" honored by contract.

### Patch 9 — Flame graphs + profiling guide `54c4e51`

- `tools/flamegraph/weft-flame.sh`: perf @ 997 Hz over the B1 bench cell
  (frame-pointer build) + FlameGraph SVG when available; folded stacks
  importable into speedscope otherwise; macOS `sample` path.
- `docs/observability/profiling.md`: what the hot path should look like
  (publish → envelope+canary+exchange; claim → exchange — nothing else).

### Patch 10 — 5% perf gate with publish/claim latency `54c4e51`

- Threshold 15% → **5%**; new latency cells vs
  `ci/baselines/publish-claim-p99-baseline.json`. Honesty declared:
  sandbox P99 swings 100× (measured), so P50 carries the strict 5% bar
  (48 ns publish / 40 ns claim @ 256 B) and P99 fails above
  max(baseline×1.05, 20 µs declared cap) — tolerant to scheduler jitter,
  still bites the syscall-in-hot-path class. `p99_bench` uses min-of-5
  reps, declared in the JSON output.

### Patch 11 — Visualizer trace timeline (this patch)

- `demos/web/src/components/TraceTimeline.tsx`: the `.weftrec` v4 lane
  view — publish/claim/drop/revoke/ack/stall/tear/canary_fail lanes,
  revocation spans shaded, pause + zoom + click-to-inspect, drag-and-drop
  of the JSON export. New "Trace Timeline (v4)" tab.
- This document: `PATCHES-TIER45.md`.

---

## Guardrails — all green on this tree

| Gate | Result |
|------|--------|
| Litmus C (L1–L8, 2 build modes) | 8/8 PASS |
| Litmus TS (L1–L8) | 8/8 PASS |
| Binding parity (byte-identical mirrors) | 27/27 PASS |
| Heddles surface (React/Vue/Svelte/RN shims) | 4/4 PASS |
| Chaos parity (C oracle golden fixture) | byte-for-byte PASS |
| Chaos selftest | PASS |
| Guardian wire check (52 fields) | PASS |
| Sanitizer matrix (ASan+UBSan+TSan) | 20/20 cells, zero findings |
| Canary fault matrix | 176/176 detected, zero false negatives |
| Fuzz corpus (ASan+UBSan) | 1.5M ops / 5.4M post-checks clean |
| @weft/core unit suite (vitest) | 145/145 PASS |
| Trace byte-identity (C == TS) | 36,730 events identical |
| Perf gate (W-suite + latency cells) | PASSED |
| xlang-trace gate | PASS (C+TS legs; VM declared-skip) |

## Verification performed (this tree)

- Every patch committed only after its battery ran green locally.
- CI matrix extended (canary-hardening, sanitizers, trace-standard) with
  YAML validated; every new script runs `set -uo pipefail` and fails loud.
- The four VM-port legs that cannot run in this sandbox (Kotlin/Dart/Swift
  compile checks; Rust) follow the repo's declared-skip honesty pattern and
  are owned by the toolchain workflows (android-packages,
  flutter-packages, apple-packages, nightly-deep).
