# DIRECTIVE-10 REPORT — Baseline Integrity & Harmonization

- **Directive:** D-10 (Wave 0)
- **Status:** PASS
- **Date:** 2026-09-15
- **Environment:** `x86_64-sandbox` / `aarch64-linux` (Termux/Debian 14.2.0, Python 3.13.5, Node v22.23.2, Rust 1.98.1)
- **Author:** Automated Agent

---

## 1. Acceptance Criteria Verification

| ID | Criterion | Scope | Threshold | Measured Result | Status |
|---|---|---|---|---|---|
| **AC-1** | Canonical bundle hash | `bench/results.json` | `16b5c663…` | `16b5c663433a37540c77f9dd6e4b83abe3eed929cc4c142813ad0b684f5498f8` | **PASS** ✅ |
| **AC-2** | EXPOSURE-RETRY logic | `tools/litmus_driver.py` | Count ≥ 1 | 7 occurrences; ratified retry-once logic active | **PASS** ✅ |
| **AC-3** | Telemetry fix | `core/ts/litmus.ts` | `claims_per_s != 0.0` | `holdsCount++` verified; reports measured claims/sec | **PASS** ✅ |
| **AC-4** | Directory harmonization | Tracked git tree | `git ls-files ^weft/ == 0` | `0` (duplicate `weft/` removed, docs synced to `docs/`) | **PASS** ✅ |
| **AC-5** | Tarball reproducibility | `make dist && make dist` | Identical sha256 | `9bcac890777a2f91ee7cc88744cceb1c64e0036ef409f6d7cc45f7b17ce28148` (both runs match) | **PASS** ✅ |
| **AC-6** | Release report forensic scan | `reports/Weft-Phase5-Release-Report-v1.0.1.pdf` | `0` flags at `x1 > 611.5 pt` | `0` flags across all 7 pages (PyMuPDF scan) | **PASS** ✅ |
| **AC-7** | Port structural validation | Kotlin, Swift, Dart, TS | 4/4 exit 0 | 4/4 passed (`python3 tools/port_validator.py`) | **PASS** ✅ |
| **AC-8** | Conformance matrix | `make litmus` (8 tests × 3 langs) | 24/24 cells pass | 24/24 PASS (C, Rust, TS) | **PASS** ✅ |

---

## 2. Deviations & Declarations (Mandatory per Standing Law 3)

* **D-T7-3 (v1.0.3 historical artifact):** Staff-staged original v1.0.3 whitepaper (`7f546cb1...`) was not placed in repository uploads; canonical whitepaper remains v1.0.4 (`6f3a3211...`). Unaffected by ongoing program; acknowledged in audit trail.
* **All performance numbers remain environment-tagged:** Tagged with `x86_64-sandbox` / local Linux environment per Owner Pivot §0. No hardware/device claims are made.

---

## 3. Evidence Artifacts

* `evidence/D-10/results_json_hash.txt` — SHA256 of canonical `bench/results.json`
* `evidence/D-10/exposure_retry_check.txt` — grep evidence of EXPOSURE-RETRY in `litmus_driver.py`
* `evidence/D-10/git_ls_files_check.txt` — count of tracked files in `weft/` (0)
* `evidence/D-10/double_dist_hashes.txt` — sha256 output of two consecutive `make dist` runs
* `evidence/D-10/forensic_scan.txt` — PyMuPDF geometry scan of release report v1.0.1 (0 flags)
* `evidence/D-10/port_validator.txt` — JSON output of port validator (4/4 PASS)
