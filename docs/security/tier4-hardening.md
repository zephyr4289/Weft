# Tier 4: Security & Hardening — Threat Model and Acceptance Evidence

Issue: [#19 — Tier 4: Security & Hardening](https://github.com/zephyr4289/Weft/issues/19)
Status: **all five tasks implemented, acceptance evidence linked below.**
Scope: memory safety, FFI isolation, input validation — the adversarial-
environment story for the Triad kernel and its driver layer.

---

## 0. Threat model

Weft runs beside hostile or buggy code. The kernel's own invariants
(wait-freedom, no second atomic, no allocation in the hot path) are proven
elsewhere (litmus L1–L8, TLA+/Loom, chaos). Tier 4 addresses the four ways
an adversary or a bug reaches *through* those proofs:

| # | Threat | Class | Where it lives |
|---|--------|-------|----------------|
| T1 | Bit flips / torn writes corrupt an envelope or its tail word; the reader trusts a frame that was never whole | memory corruption | buffer tails + envelope words |
| T2 | Malformed or out-of-range inputs (`payload_len`, `payload_max`, fanout geometry) poison frames or index memory out of bounds | input validation | every public entry point |
| T3 | A buggy writer never ACKs; a teardown waits forever | denial of service | `weft_reclaim` / I6 |
| T4 | Undefined behavior and data races in the C surface that unit tests never provoke | UB / races | whole C build |
| T5 | Foreign guests (JNI, Dart FFI, plugins) panic, hang, or segfault across the FFI seam and take the host down | FFI isolation | `ffi_host` seam |

Everything in this document maps one task to one threat and one gate.

---

## 1. Memory Safety Audit → T4 (sanitizer matrix)

`ci/scripts/run_sanitizers_shard.sh` runs the C surface under
**ASan+UBSan (joint, `-fno-sanitize-recover=all`)** and **TSan**, 20 cells:

- ASan+UBSan: kernel litmus L1–L8 · canary C-series · reclaim R-series ·
  fanout F-series · governor G-series · verified (RFC 4231 HMAC vectors) ·
  blend bit-exactness · fuzz-runner (200k ops) · ffi_host (both modes)
- TSan: kernel litmus L1–L8 · fanout F-series

Acceptance: **zero findings**. No suppression files exist in the tree; none
are allowed. Any sanitizer `WARNING`/`runtime error:` line in the shard log
is RED even if the binary exited 0.

Evidence: `ci/run-artifacts/shard-sanitizers-results.json` (20 cells, all
PASS in this tree).

The TS port's memory safety story is unchanged (SharedArrayBuffer + JS
object semantics); the JVM/Swift/Dart ports are memory-managed by their
runtimes. The C surface is the one where sanitizers can and must bite.

## 2. FFI Boundary Isolation → T5

`core/c/ffi_host.{h,c}` — worker isolate + bounded mailbox:

- Message passing, not shared memory: fixed-size value blobs cross the
  seam; no pointers in either direction.
- **<1 ms overhead**: measured 6–9 µs/call in-process (2000-call loop),
  100×+ inside the acceptance bar. F5 fails the build if violated.
- **Timeouts work**: a hung guest returns `WEFT_FFI_TIMEOUT` at the
  caller's bound; the worker is replaced and the host survives (F2).
- **Zero cross-port crashes**: fork mode runs each guest in a child — an
  `abort()`ing guest is reaped as `WEFT_FFI_CRASH` while the parent keeps
  executing (F3 proves it in-tree).

Evidence: `ffi_host_test.c` F-series — 15 checks in-process + 12 fork mode.

## 3. Canary Hardening → T1

The plain `canary = seq` tail word was structurally blind to `version` and
`payload_len` corruption. The **XOR boundary canary** folds every envelope
word the reader trusts into the tail word:

```
canary = (u64)seq ^ WEFT_MAGIC ^ (u64)version ^ (u64)payload_len
```

- Single-sourced (`weft_canary_compute`) and mirrored byte-for-byte in all
  six ports (C, TS, Kotlin, Swift, Dart, Rust) — binding parity 27/27.
- All components are zero-extended u32, so the u64's hi half is always 0 —
  the wire contract the guardian manifest now documents and the wire probe
  observes (52-field wire check PASS).
- The null frame is a REAL frame and carries the real boundary canary
  (04-LITMUS §0.6 invariant preserved).
- `weft_r_canary_check()` recomputes from the LIVE held envelope; fails
  closed (null buffer, wrong magic ⇒ MISMATCH).

**Zero false negatives measured**: `canary_test.c` C4 injects **176
single-bit faults** — every bit of `{magic, version, seq, payload_len,
canary}` — and every one is detected. Multi-bit flips (C6) likewise.

Declared scope (Law 4, C5/C7): `header_size` is a layout field bounds-
checked by the decoder, not the canary; a flip-and-compensate adversary
(flip `seq` AND recompute the canary) passes the canary BY DESIGN — that is
a keyed-MAC problem, which VerifiedWeft's HMAC path already solves; the
compensated frame is still caught by the litmus payload-pattern layer.

Evidence: `canary_test.c` — 456 checks, 0 fails; chaos-parity golden
fixture byte-for-byte; `ci/scripts/run_canary_shard.sh`.

## 4. Revocation Timeout Bounds → T3

`weft_reclaim` now **always returns within min(caller bound, ceiling)**:

- `WEFT_RECLAIM_MAX_TIMEOUT_MS_DEFAULT = 1000` ms per instance,
  runtime-configurable (`weft_set_max_reclaim_timeout`; 0 disables the
  ceiling — declared, the caller then owns the bound).
- Named verdicts: `WEFT_RECLAIM_ACK` (0 — the legacy `rc == 0` contract
  preserved), `WEFT_RECLAIM_TIMEOUT` (1 — the caller MUST NOT poison/free:
  no ACK, the A1 hazard is live), `WEFT_RECLAIM_INVALID` (NULL).
- Timeouts are advisory-logged (`WEFT_WARN_RECLAIM` env-gated) and counted
  (`t_reclaim_timeouts`) — never silent, never a correctness signal.
- Bound honesty: 100 µs poll granularity ⇒ return within the effective
  bound + one poll tick (declared, measured in R2).

Ports mirror the ceiling + counting. The Dart sync `reclaim` keeps its
documented instant-return semantics (a single-isolate busy-poll can never
observe the ACK — the zero-width bound is the honest bounded wait); the
async path (where waiting is real) gained the clamp + counter.

Evidence: `reclaim_test.c` R-series — 15 checks: default ceiling, clamp
MEASURED (5000 ms caller bound returns at ~1000 ms), ACK inside bound,
timeout telemetry exact, ceiling 0 disables, setter visible, NULL guard.

## 5. Input Validation → T2

The validation wall, **before any memory operation**:

| Entry point | Wall |
|-------------|------|
| `weft_init` | `w != NULL`; `1 ≤ payload_max ≤ WEFT_PAYLOAD_MAX_LIMIT` (1 MiB) — refused pre-allocation |
| `weft_publish` | `payload_len ≤ payload_max` else `WEFT_PUB_INVALID` + `t_invalid` — frame refused WHOLE, no byte written (revoked-first ordering preserved per 02 §6) |
| `weft_w_write_payload` | `len ≤ payload_max`; NULL src with nonzero len refused |
| `weft_fanout_claim` | defensive geometry revalidation — corrupt/hand-rolled reader structs fail CLOSED, never index OOB |
| fanout `init/attach/reader_init` | geometry validated against `ring_bytes` (existing, retained) |

VM ports map the C `-1` refusal to their native loud failure (TS
`RangeError`, Kotlin `require`, Swift `precondition`, Dart
`ArgumentError`, Rust `None`) plus the `Invalid` publish verdict and
`t_invalid` counter in all five.

**Zero OOB in fuzzing**: `fuzz_runner.c` — a deterministic (seeded,
reproducible) adversary over init geometry, publish bounds, payload
writes, slice reads, canary checks and fanout geometry, asserting five
declared post-conditions after every op. Under ASan+UBSan:
**1,496,728 ops / 5,408,704 post-checks / zero findings**. A libFuzzer
harness entry is compiled by `-DWEFT_FUZZ_LIBFUZZER`.

---

## 6. Acceptance criteria checklist (issue #19)

- [x] **Zero findings** in the full TSan/UBSan/ASan suite — 20 cells green
- [x] **Sanitizer tests added to CI** — `sanitizers` shard in `extreme-test.yml`
- [x] All existing tests pass with sanitizers enabled — litmus/fanout/
      governor/verified/blend all run under ASan+UBSan
- [x] **Zero cross-port crashes** — F3 fork containment proof in-tree
- [x] FFI calls **<1 ms overhead** vs direct calls — measured 6–9 µs (F5)
- [x] **Timeout mechanism works (no infinite hangs)** — F2 measured bound
- [x] **Zero false negatives** in the canary fault matrix — 176/176 detected
- [x] **No regressions in existing canary checks** — litmus 8/8 C+TS+Rust(CI),
      chaos-parity golden fixture byte-for-byte, binding parity 27/27
- [x] `weft_reclaim` **always returns ≤ max timeout** — R2 measured
- [x] **No regressions in revocation tests** — L7 `reclaim_ok=1`, poison intact
- [x] Timeout **configurable at runtime** — R6
- [x] **Zero OOB writes in fuzzing** — 1.5M ops clean
- [x] **Zero UB in UBSan** — `-fno-sanitize-recover=all`, clean
- [x] All inputs validated **before** any memory operation — wall placement
      is asserted by the fuzz corpus post-conditions (P2: a refusal changes
      no observable state)
