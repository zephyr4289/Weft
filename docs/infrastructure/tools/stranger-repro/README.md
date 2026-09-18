# tools/stranger-repro — the weft.dev stranger-repro bundle

One command, any machine, no trust required:

    bash run.sh

Builds the C kernel from source (needs only a C compiler), runs the full
conformance battery — kernel litmus L1–L8, fan-out F-series, the F10
100k-frame 3-reader torture, the .weftrec flight-recorder roundtrip,
VerifiedWeft gen/validate/tamper-rejection, and the cross-port
parity-of-contract gates — and writes:

- `STRANGER-REPRO-REPORT.txt`  human verdict, every leg RUN/PASS/FAIL
- `STRANGER-REPRO-REPORT.json` machine-readable results
- a **verification stamp** (sha256 of the report) to paste into issues/PRs

Honesty contract: a leg that cannot run (missing toolchain) is recorded as
SKIPPED with a reason — never a pass. The stamp is only issued when every
executed leg PASSED. Per-leg logs land in `build/leg-*.log`.

STATUS: CI-PROVEN (this battery IS the provenance story of every run).
