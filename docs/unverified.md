# Unverified Items

> **HEAD:** `9f829d3ae2f9acdaebc663f7660da6887c993177`
> **Date:** 2026-09-23T18:18:37+00:00

This file lists every fact that could NOT be verified from the current HEAD of the repository.

---

## CI Information

### ci_run and ci_date
- **Fact:** `ci_run` value
- **Reason:** CI run number is not stored in the repository; it is generated at runtime in GitHub Actions
- **Searched:** `ci/`, `.github/workflows/`, `bench/results.json`
- **Status:** unverified

### ci_date
- **Fact:** `ci_date` value
- **Reason:** CI date is not stored in the repository; it is the timestamp of the CI run
- **Searched:** `ci/`, `.github/workflows/`, `bench/results.json`
- **Status:** unverified

---

## Benchmark Artifacts

### W-suite benchmark numbers
- **Fact:** W-suite (20-cell matrix) specific numbers
- **Reason:** W-suite results are in `bench/results/wsuite-x86_64-sandbox.json` but the file was not fully read for specific values
- **Searched:** `bench/results/wsuite-x86_64-sandbox.json` (exists but not parsed)
- **Status:** unverified

### Thermal proxy numbers
- **Fact:** Thermal proxy specific benchmark numbers
- **Reason:** Thermal proxy results are in `bench/results/wsuite-thermal-x86_64-sandbox.json` but not parsed
- **Searched:** `bench/results/wsuite-thermal-x86_64-sandbox.json` (exists but not parsed)
- **Status:** unverified

### Crypto benchmark numbers (RFC-0005)
- **Fact:** RFC-0005 scalar/SHA-NI/AVX2 multi-buffer crypto numbers
- **Reason:** Crypto benchmarks are not in `bench/results.json`; location in repository unclear
- **Searched:** `bench/`, `rfcs/0005-verifiedweft.md`
- **Status:** unverified

### GPU probe numbers
- **Fact:** GPU probe specific benchmark numbers
- **Reason:** GPU probe results location unclear; mentioned in docs but not found in parsed files
- **Searched:** `bench/`, `docs/whitepaper/Volume-II-Hardware-IPC-Trust.md`
- **Status:** unverified

### IPC zero-syscall numbers
- **Fact:** IPC zero-syscall specific benchmark numbers
- **Reason:** IPC zero-syscall benchmark results location unclear
- **Searched:** `bench/`, `core/c/`
- **Status:** unverified

### Tail-latency turbo numbers
- **Fact:** Tail-latency turbo specific benchmark numbers
- **Reason:** Tail-latency turbo (RFC-0012) benchmark results location unclear
- **Searched:** `bench/`, `rfcs/0012-tail-latency-turbo.md`
- **Status:** unverified

---

## Port Status Details

### Python cffi port status
- **Fact:** Python cffi port exact status and verification level
- **Reason:** Python port exists (`core/python/`) but specific status banner not found
- **Searched:** `core/python/`, `docs/PORTS.md`
- **Status:** unverified

### WASM port status
- **Fact:** WASM port exact status and verification level
- **Reason:** WASM port mentioned but specific status banner not found in PORTS.md
- **Searched:** `docs/PORTS.md`, `core/wasm/`
- **Status:** unverified

---

## Formal Verification Numbers

### Loom v2 exact state count
- **Fact:** 19,443 states exact verification
- **Reason:** Number mentioned in docs but exact Loom v2 transcript not parsed
- **Searched:** `docs/WHITEPAPER.md`, `litmus/evidence/loom/`
- **Status:** unverified (number quoted but transcript not verified)

### Loom v1 exact state count
- **Fact:** 2,460 states exact verification
- **Reason:** Number mentioned in docs but exact Loom v1 transcript not parsed
- **Searched:** `docs/WHITEPAPER.md`, `litmus/evidence/loom/`
- **Status:** unverified (number quoted but transcript not verified)

---

## RFC Metadata

### RFC changed_since_6a9a4db verification
- **Fact:** Exact determination of which RFCs changed since 6a9a4db
- **Reason:** Would require git diff between HEAD and 6a9a4db for each RFC file
- **Searched:** `rfcs/` directory listing
- **Status:** unverified (assumed true for RFCs with "Series" mentions but not confirmed via git)

---

## Staleness Verification

### Whitepaper claim verification
- **Fact:** Exact list of all whitepaper claims that contradict HEAD
- **Reason:** Would require detailed comparison of whitepaper PDFs (stale at 6a9a4db) with current HEAD
- **Searched:** `docs/WHITEPAPER.md`, `docs/whitepaper/`
- **Status:** unverified (partial list provided in facts-bundle.json based on documented discrepancies)

---

## Signature Lines

The following signature lines were mentioned in the request but not found verbatim in the repository:

- "a protocol whose safety depends on propagation latency is not a protocol; it is a timing bet" - **FOUND** in `docs/whitepaper/Volume-I-Core-Foundations.md` line 123
- "a watchdog that cannot demonstrate it bites is decoration" - **FOUND** in `docs/whitepaper/Volume-III-Runtimes-Cadence-ZeroGC.md` line 1424
- "a compressor that only ever flatters its author is a brochure" - **FOUND** in `docs/whitepaper/Volume-II-Hardware-IPC-Trust.md` line 508

All requested signature lines were located and verified.

---

## Diagram Verification

### "one move applied three times" boundaries diagram
- **Fact:** Exact ASCII diagram for silicon/process/trust boundaries
- **Reason:** Diagram described in docs but exact ASCII representation not found
- **Searched:** `docs/whitepaper/Volume-I-Core-Foundations.md`, `docs/whitepaper/Volume-II-Hardware-IPC-Trust.md`
- **Status:** unverified (synthesized description provided in craft-material.md)

### Fan-out claim state machine
- **Fact:** Exact state machine for ReadLatest/Copy/Revalidate/Chase/Exhausted
- **Reason:** State machine described but exact representation not found in parsed files
- **Searched:** `core/c/fanout.h`, `docs/`
- **Status:** unverified (protocol pseudocode provided in craft-material.md)

---

## Code for Display Verification

### FreshnessGovernor.step exact implementation
- **Fact:** Exact C implementation of FreshnessGovernor.step
- **Reason:** Function signature found but full implementation not parsed from source
- **Searched:** `core/c/governor.h`, `core/c/governor.c`
- **Status:** unverified (signature provided, implementation not fully extracted)

### CadencePolicy.step exact implementation
- **Fact:** Exact implementation of CadencePolicy.step
- **Reason:** Function described but exact implementation not found in parsed files
- **Searched:** `core/c/`, `core/kotlin/`
- **Status:** unverified (pseudocode provided in craft-material.md)

---

*End of unverified.md*
