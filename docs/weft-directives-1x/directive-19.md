# DIRECTIVE-19 — End-to-end integration gate → coordinated v0.1.0

- Wave: 4 (final) · Depends: D-10…D-18 · Effort: ~6 h · Status: ISSUED

## 1. Context

The program's exit gate: prove the same kernel contract survives every shipped surface, sync the docs to reality, and cut coordinated v0.1.0 through the D-18 pipeline. This is the "end to end" in the owner's directive — every package consumes the frozen kernel and passes the same invariants.

## 2. Tasks

- **T19.1 Cross-package parity matrix.** Run the W2 workload through every surface and fill the parity table (per row: environment tag, frame count, invariants, steady-state allocs):
  - `@weft/core` on node (worker path) and chromium (SAB path);
  - `weft-android` `weft-core` on JVM/Robolectric;
  - `weft_flutter` on linux desktop (FFI into real `libweft.so`);
  - `WeftCore` on macOS (swift test harness);
  - python ctypes model C + C kernel directly (the W-suite reference cells).
  Parity requirement: identical frame counts given identical seeds/params where the surface allows; I1–I6 invariants hold everywhere; zero steady-state allocations in C/D paths. Where a surface cannot match params exactly, the divergence is declared with its cause — honest divergence beats forced equality.
- **T19.2 Docs sync.** `PORTS.md` status column: spec → shipped per row (with version). `ARCHITECTURE.md`: RFC Q1–Q5 statuses point at D-17 memos. `ROADMAP.md`: Phase 6+ device gates marked WITHDRAWN (owner pivot), replaced by series-1x ledger. Every package README: install, minimal example, honesty footer (what is and isn't verified without hardware).
- **T19.3 Version + tag.** changesets → npm `0.1.0`; gradle/SPM/pub versions aligned `0.1.0`; git tag `v0.1.0` → D-18 workflow → full (non-rc) release with registry publishes ONLY where credentials exist (others stay attached artifacts, declared).
- **T19.4 Clean-tree gate.** After full build + release: `git status` clean; no generated files committed; `SHA256SUMS` complete; series ledger updated.

## 3. Non-goals

No new features. No perf comparisons across packages (parity is correctness, not speed). No whitepaper edits (v1.0.4 stays canonical; a v1.0.5 is only in scope if staff orders one with a written changelog).

## 4. Acceptance criteria (mechanical)

1. Parity table complete: 6 rows × {env, frames, invariants, allocs} with evidence links; any divergence cell carries cause + declaration.
2. I1–I6 asserted programmatically in every row's harness (not eyeballed) — assertion code path named per row.
3. Docs diffs limited to status/banner updates (no normative content changes without staff review).
4. `v0.1.0` release exists with full artifact matrix + signed `SHA256SUMS`.
5. Clean-tree: `git status --porcelain` empty post-pipeline.

## 5. Evidence to return

`evidence/D-19/`: parity harness logs ×6, assertion sources, docs diff, release URL + SHA256SUMS, git status output.

## 6. Report

`reports/D-19-REPORT.md` per index §4. On staff PASS: series 1x CLOSED, program reaches the end-to-end endpoint.
