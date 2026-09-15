# DIRECTIVE-20 REPORT: Final Closure Batch (Series 1x & Phase 5)

- **Directive**: D-20 (Series 1x Final Closure Batch)
- **Status**: COMPLETED / PASS
- **Date**: 2026-09-16
- **Environment**: `termux-arm64` / `linux-ci` (Ubuntu 22.04 LTS / x86_64)
- **Reference**: `docs/weft-directives-1x/round-7-adjudication.md` (Findings G-1…G-6)

---

## 1. Executive Summary

Directive 20 executes the final mechanical closure batch for Directive Series 1x and Phase 5:
1. **v1.0.3 Historical Restore (G-1, D-T7-3, R4)**: The staff-provided canonical artifact (`7f546cb1…`, 93,076 bytes) was verified and copied byte-for-byte to `reports/Weft-Whitepaper-v1.0.3.pdf`. Root `SHA256SUMS` was updated with the restored hash (15/15 OK). The historical R4 Correction Slip was retained in tree.
2. **Honesty Banner Precision (G-3)**: Embedded the byte-verbatim PORTS.md divergence note in `packages/flutter_weft/lib/src/weft_reference.dart`.
3. **Minisign Declaration & Hygiene (G-4, G-5)**: Added deviation note documenting that the prior mock signature was superseded by genuine Ed25519; added `minisign/` to `.gitignore`; guarded `tools/minisign_tool.py` behind explicit CLI flags (bare run exits non-zero).
4. **Release Signing Gate Codification (G-2)**: Codified the CI release-signing gate rule in `evidence/D-18/RELEASE-NOTES.md`.

---

## 2. Acceptance Criteria Scoreboard

| Criterion | Target Requirement | Measured Value / Result | Status |
| :--- | :--- | :--- | :---: |
| **AC-20.1** | `sha256sum reports/Weft-Whitepaper-v1.0.3.pdf` | `7f546cb17c404bab1fe5de08b17b881f2bb40c20822a52690ec758481e8283ce` (93,076 B) | **PASS** |
| **AC-20.2** | `sha256sum -c SHA256SUMS` (15 rows) | 15/15 OK; v1.0.3 row == `7f546cb1…` | **PASS** |
| **AC-20.3** | Correction slip present in tree | `reports/Weft-Phase4-Errata-R4-Correction-Slip.{md,pdf}` present & untouched | **PASS** |
| **AC-20.4** | Byte-verbatim PORTS.md banner | Full note incl. "honesty load-bearing wall" embedded in `weft_reference.dart` | **PASS** |
| **AC-20.5** | `.gitignore` contains `minisign/` & tool hygiene | `minisign/` gitignored; bare `tools/minisign_tool.py` exits 1 | **PASS** |
| **AC-20.6** | Mock-superseded declaration | Present in `reports/D-18-REPORT.md` and `D-report/D-18-REPORT.md` | **PASS** |

---

## 3. Evidence Matrix

| Check | Evidence Artifact |
| :--- | :--- |
| v1.0.3 SHA-256 Checksum | `evidence/D-20/v1.0.3_sha256.txt` |
| Root `SHA256SUMS` Verification | `evidence/D-20/root_sha256sums_verify.txt` |
| R4 Correction Slip Retention | `evidence/D-20/correction_slip_ls.txt` |
| Verbatim Dart Honesty Banner | `evidence/D-20/dart_banner_check.txt` |
| `.gitignore` Minisign Entry | `evidence/D-20/gitignore_minisign.txt` |
| `minisign_tool.py` Bare Run Exit Code | `evidence/D-20/minisign_bare_run.txt` |
| D-18 Mock Declaration Text Match | `evidence/D-20/d18_mock_declaration_grep.txt` |
| Release Signing Gate Codification | `evidence/D-20/release_signing_gate_grep.txt` |

---

## 4. Invariants & Deviations

- **Kernel Freeze**: `core/c/weft.{c,h}` and `core/rust/src/lib.rs` unmodified (0 diffs).
- **Structural Validator**: `python3 tools/port_validator.py --target all` PASS (100% pass across Kotlin, Swift, Dart, TS).
- **Deviations**: None. D-T7-3 is fully CLOSED.
