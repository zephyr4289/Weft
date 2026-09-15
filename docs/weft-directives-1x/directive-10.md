# DIRECTIVE-10 — Baseline: R1–R6 integrity batch + directory harmonization + CI skeleton

- Wave: 0 (gates the whole series) · Depends: — · Effort: ~4 h · Status: ISSUED

## 1. Context

Round-5 adjudication (WO-P5-VERIFICATION.md) left Phase 2/3/4/5 conditional on batch R1–R6, and the repo carries a dual-tree problem (top-level source tree + embedded `weft/` from tarball packaging). The owner pivot makes this the mandatory first move: build end-to-end **from a clean baseline**. Nothing else in the series starts until this closes.

## 2. Tasks

- **T10.1 (R1) Canonical bundle restore.** Delete the added `sha256` self-hash stamp line from `bench/results.json`; verify `sha256(results.json)` == `16b5c663…` (full hash to be recorded in evidence). The stamp belongs in `SHA256SUMS`, never inside the file it hashes.
- **T10.2 (R2) Litmus driver honesty fixes.** Implement the ratified EXPOSURE-RETRY rule (WO-P1-CLOSURE T3) in `litmus_driver.py`; make `claims_per_s` emit real telemetry (measured claims/second over the run window), replacing the constant `0.0`.
- **T10.3 (R3) Clean-tree re-run.** Full clean-tree validation from ONE working directory (mixed provenance was P5-W8); ship the complete log.
- **T10.4 (R4) Historical restore.** Replace the rebuilt impostor `reports/Weft-Whitepaper-v1.0.3.pdf` (4db9b508) with the adjudicated FAIL artifact `7f546cb1…` (staff-staged at `download/staff-provided/Weft-Whitepaper-v1.0.3.pdf`). Record the correction as an errata slip in `RELEASE-NOTES.md` (do NOT edit the whitepaper or the release report's history).
- **T10.5 (R5) Release report v1.0.1.** Re-typeset `Weft-Phase5-Release-Report.pdf`: (a) 0 PAST-CROPBOX spans under the pinned criterion `x1 > 611.5 pt`, ALL pages, PyMuPDF — the §6 table hashes get breakable formatting (wrap or externalize); (b) all hashes externalized to `SHA256SUMS` (report cites the file, prints only short prefixes); (c) "pending C5r" label removed; (d) absorb the P5-W3 correction (C3 errata §3 misattributed the C2 Rust soak capture as "the B3 evidence") and the E-1 typo (C1-W2, not C2-W2).
- **T10.6 (R6) Re-tar.** Rebuild `weft-sandbox-v0.1.tar.gz` from the restored tree; ship new sha256 + updated `SHA256SUMS`; tarball self-hash lives only in the ledger/SHA256SUMS (never inside the artifact).
- **T10.7 Directory harmonization.** Canonical tree = top-level. Remove embedded `weft/` from version control (generated output only). `make dist` builds the release tarball on the fly from the canonical tree; two consecutive runs produce byte-identical sha256. `.gitignore` gains the generated paths.
- **T10.8 CI skeleton.** GitHub Actions: `build-linux` (gcc/clang + make litmus + make bench smoke), `build-ts` (node 20/22 placeholder job), `dist-drift` (assert `make dist` output matches `SHA256SUMS`). Workflows pass on the sandbox-equivalent container.

## 3. Non-goals

No kernel diffs. No whitepaper content edits (v1.0.4, `6f3a3211…` is canonical). No new features. No device work.

## 4. Acceptance criteria (mechanical)

1. `sha256(bench/results.json)` == `16b5c663…` (scope: file as shipped in tarball).
2. `grep -c "EXPOSURE-RETRY" litmus_driver.py` ≥ 1 and 5×L1 runs return claims ≥ catalog floor with retry log lines present, or a lawful RED per run — both declared in the report.
3. Clean-tree log: single working-directory provenance, all sections present.
4. `sha256(reports/Weft-Whitepaper-v1.0.3.pdf)` == `7f546cb1…`.
5. Pinned scan: 0 spans `x1 > 611.5 pt` across ALL pages of the release report v1.0.1 (method + threshold named in the report itself).
6. `make dist && make dist` → identical tarball sha256 (both printed).
7. `git ls-files | grep -c "^weft/"` == 0 (embedded tree untracked).
8. CI: all three workflows green on a clean checkout.

## 5. Evidence to return

`evidence/D-10/`: results.json hash before/after; 5×L1 logs; clean-tree log; whitepaper hash; report span-scan output (script + raw numbers); double-dist hash pair; `git ls-files` excerpt; CI run URLs.

## 6. Report

`reports/D-10-REPORT.md` per index §4. On staff PASS: Phase 2/3/4/5 all CLOSED.
