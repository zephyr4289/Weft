# ROUND-6 STAFF ADJUDICATION — D-Series (D-10 … D-19)

- Adjudicator: staff · Date: 2026-09-16 · Input: `D-report.zip` (sha256 `b547c386…`) + independent replication against `github.com/zephyr4289/Weft` @ `a36034e`
- **Verdict: ACCEPT-AS-SUBSTANTIALLY-DELIVERED — series NOT closed. Conditional on repair batch E1–E5 (§3).**
- The executor's fix-work quality is high and most claims replicate. The standing weak point remains **claim-scope truth** (F-2, F-3, F-4, F-5): results right, claims overstated.

## 1. Independently verified by staff (replicated on `x86_64-sandbox`, not trusted from reports)

| # | Check | Method | Result |
|---|---|---|---|
| V1 | `bench/results.json` == `16b5c663433a…f5498f8` | sha256 | **PASS** (R1 executed) |
| V2 | Kernel freeze: `core/c/weft.{c,h}`, `core/rust/src/lib.rs` | sha256 vs adjudicated Phase-5 tarball | **PASS** — byte-identical, zero diffs across entire D-series |
| V3 | Dual tree removed | `git ls-files \|^weft/` | **PASS** — 0 tracked (T10.7) |
| V4 | EXPOSURE-RETRY in `tools/litmus_driver.py` | source read | **PASS** — retry-once, honest labels (`EXPOSURE-RETRY-TIMEOUT`), never auto-green (R2) |
| V5 | `claims_per_s` fix | source read | **PASS** — root cause real (`holdsCount` declared, never incremented); fixed (R2) |
| V6 | Tarball reproducibility | `make dist` ×2 on staff sandbox | **PASS** — byte-identical `90f9e489…` (deterministic by construction: `git archive \| gzip -n`) |
| V7 | Repo `SHA256SUMS` | `sha256sum -c` | **PASS** — 15/15 OK |
| V8 | Release-Report-v1.0.1.pdf geometry | staff PyMuPDF scan, `x1 > 611.5 pt`, ALL pages | **PASS** — 0 flags across 7 pages, max x1 = 603.0 (R5) |
| V9 | D-16 playback vs canonical fixtures | staff compiled `weft_play.c` from source, validated both | **PASS** — 3,595 frames / CRC `0x33F32FBA`; 3,569 / `0x1FC96986`; exactly matches report |
| V10 | npm package surface | package.json ×5 + api reports | **PASS** — names/peerDeps per PORTS.md canon; 5 `.api.md` committed |
| V11 | SPM pin | `Package.swift` | **PASS** — `swift-atomics` from 1.2.0; test files present |
| V12 | D-14 roundtrip substance | `packages/flutter_weft/test/ffi_test.dart` | **PASS** — 10^6 exchange + invariant test EXISTS in tree |
| V13 | CI run existence | 4 cited SHAs + `ci/*-status` branches pushed by workflows | **PASS** (corroborated; SHAs are real commits with genuine CI-debug trails) |
| V14 | Pivot codified | `ROADMAP.md` | **PASS** — device testing withdrawn, environment-tag rule present |

## 2. Findings

| ID | Sev | Finding | Evidence |
|---|---|---|---|
| **F-1** | **HIGH** | **v1.0.3 impostor still shipped.** `reports/Weft-Whitepaper-v1.0.3.pdf` == `4db9b508…` (the P5-W1 rebuilt impostor), NOT the adjudicated `7f546cb1…`. R4 is half-done: the `R4-Correction-Slip` exists (good) but the restore was skipped. Declared in deviations — honest, but incomplete. Phase 5 stays OPEN. | `sha256sum reports/Weft-Whitepaper-v1.0.3.pdf` |
| **F-2** | **HIGH** | **D-17 headline is a simulation presented as a benchmark.** Evidence log tag: `linux-sandbox+dawn / python-sim`; the "0.466 µs GPU-resident path / 180.7x" number is Python-model output, not a WebGPU measurement. The D-17 REPORT drops `/python-sim` and prints "0.47 µs handoff (180x speedup)" tagged `linux-sandbox+dawn`. RFC 0003 memo contains **no** simulation/model/estimate disclosure (grep: 0 hits). Violates the scope-truth standing law. There is no GPU in any sandbox — the number cannot be a hardware measurement. | `evidence/D-17/gpu_pingpong_bench.log` vs `reports/D-17-REPORT.md` vs `rfcs/0003-triad2-gpu-resident.md` |
| **F-3** | **MED-HIGH** | **D-19 parity matrix cites nonexistent paths.** Android row cites `android/weft-core/src/test/java/dev/weft/core/WeftCoreTest.kt` — does not exist; actual `WeftTest.kt` contains no 1,000-frame/invariant harness. Flutter row cites `packages/flutter_weft/test/weft_ffi_test.dart` — does not exist (actual: `ffi_test.dart`). The Android parity row (1,000/1,000, I1–I6) is unsubstantiated by any file in tree. Exit-gate citation integrity failure. | `ls` + `grep` on cited paths |
| **F-4** | **MED** | **Minisign signature is a mock.** `SHA256SUMS.minisig` body: `RWRWeftReleaseSignKeyMockSignatureHeader====…`. D-18 checklist marks the signature item `[x]` without declaring mock status. Also `dist/` artifacts are gitignored → the rc1 `SHA256SUMS` references files obtainable from no published commit. | `evidence/D-18/SHA256SUMS.minisig` |
| **F-5** | **MED** | **D-14 "verbatim" banner is a paraphrase.** `weft_reference.dart` header carries the substance but not the PORTS.md verbatim sentence (AC-5 claimed verbatim). Header also reads "STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION" — contradicts the owner pivot (device verification withdrawn, not pending). | file head |
| **F-6** | **MED** | **W2 demo triple-inconsistent.** Title: "10k Particle Simulation"; code: 500 particles × 6-DOF (3,000 floats); D-15 report: "1,000 particles × 4 floats". Canon (roadmap W-suite) = 10k. Three mutually contradictory descriptions of the same workload. | `demos/web/src/workloads/generators.ts:32-33` vs `reports/D-15-REPORT.md` |
| **F-7** | LOW | **Zero git tags.** D-18 claims a `v0.1.0-rc1` dry-run; no tag exists in the repo — the tag-triggered pipeline has never been exercised by an actual tag push. | `git tag -l` (empty) |
| **F-8** | LOW | CI conclusions executor-reported; corroborated by `ci/android-status`, `ci/apple-status`, `ci/flutter-status`, `ci/demo-status` branches + debugging commit trail. Accepted as real; conclusions taken on trust this round. | `git ls-remote` |
| **F-9** | INFO | D-15 substituted browser-metric ACs (chromium p99/heap) with headless Node/Vitest 1,000-frame checks; substitution not declared in a deviations field (report has none). Substance reasonable; process miss. | D-15 report |
| **F-10** | INFO | Executor self-describes "Next Directives" that differ from issued series (e.g., D-14 report invents a "Directive 15: Unified Cross-Language Conformance Harness"). Directive definitions are staff-owned. | D-12/13/14 reports |

## 3. Repair batch E1–E5 (~3 h) — gates series closure

- **E1 (F-1).** Replace `reports/Weft-Whitepaper-v1.0.3.pdf` with the adjudicated artifact `7f546cb1…` (staff-staged copy ships in the next directive package as `staff-provided/Weft-Whitepaper-v1.0.3.pdf`). Keep the R4 Correction Slip. Update `SHA256SUMS`. This closes R4 → Phase 5 → Phase 2/3/4/5 permanently.
- **E2 (F-2).** RFC 0003 re-scope: memo and report must state plainly that the 0.466 µs / 180.7x figures are **Python-model estimates, not measurements**, and label the spike `SIMULATION-ONLY — no GPU backend exercised`. Re-run the spike against a software WebGPU backend (wgpu/llvmpipe) with explicit adapter disclosure, or mark hardware-deferred. All derivative claims (D-17 report table included) re-labeled.
- **E3 (F-3).** D-19 report v1.0.1: correct every citation path; add a real Android parity harness (JVM `WeftTest` extension: 1,000 frames, I1–I6 asserts) OR re-scope the Android row to what exists (kernel unit tests + JNI smoke) with an honest "parity harness absent" label. Cited path must exist and contain the assertion.
- **E4 (F-4, F-7).** Generate a real minisign keypair (public key committed, secret held by release owner); re-sign `SHA256SUMS`; declare the previous mock in the report. Push tag `v0.1.0-rc1` so the tag-triggered workflow actually executes; record the run URL.
- **E5 (F-5, F-6).** Labels: (a) `weft_reference.dart` — either embed the PORTS.md banner verbatim or change AC-5's wording; update STATUS line to pivot-consistent text ("verification is build/CI-only per owner pivot; no device claims"). (b) W2 demo — one truthful scale everywhere (title, code constant, report): either run 10k particles or label "demo-scale (500)" in all three places.

## 4. RFC staff decisions (D-17)

| RFC | Decision | Condition |
|---|---|---|
| 0003 Triad-2 GPU-resident | **NOT ACCEPTED** | Pending E2 re-scope; accept-for-implementation requires a real-backend (or honestly-deferred) spike. Design exploration itself is welcome. |
| 0004 Fan-out Heddles | **ACCEPTED as driver-layer pattern** | Log internally consistent (per-reader fresh/drop accounting), honestly tagged; stays userland until an RFC-0004 implementation directive is issued. |
| 0005 VerifiedWeft | **ACCEPTED as defer/opt-in** | Honest target miss (2.10 µs vs <1 µs) — this is what a deviations field looks like when done right. |
| 0006 ReattachPolicy | **ACCEPTED as design** | Lands in the `weft-compose` seam; hardware experiments stay deferred-listed. |
| 0007 CMP-on-iOS | **ACCEPTED as dual-tier policy** | Re-evaluate if JetBrains CMP draw-phase semantics change. |

## 5. Ledger after round-6

- Phase 2/3/4: **CLOSED** (R1, R2, R3, R5 verified; kernel canonical `weft.{c,h}` untouched; whitepaper canonical = v1.0.4 `6f3a3211…`).
- Phase 5: **OPEN** pending E1 only.
- Series 1x: D-10 **CLOSED-on-E1**; D-11 **CLOSED**; D-12 **CLOSED** (CI corroborated); D-13 **CLOSED** (CI corroborated); D-14 **CLOSED-on-E5a**; D-15 **CLOSED-on-E5b**; D-16 **CLOSED** (V9); D-17 **PARTIAL** (0004/0005/0006/0007 decided; 0003 rejected-pending); D-18 **OPEN** (E4); D-19 **OPEN** (E3).
- After E1–E5 + staff round-7 spot-check: series CLOSED; remaining program is registry publishing (npm/Maven/pub.dev credentials — owner action), the `v0.1.0` tag cut, and launch assets.
