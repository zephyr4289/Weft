# directive-20 — FINAL CLOSURE BATCH (Series 1x)

- Priority: **P0 — sole remaining gate for Phase 5 + Series 1x closure**
- Budget: **≤ 1 h** (mechanical)
- Blocked-by resolution: round-7 adjudication confirmed E1 was blocked on **staff** (the round-6 package shipped without the promised artifact — staff packaging miss, on the record). The artifact is physically included in this package:
  - `staff-provided/Weft-Whitepaper-v1.0.3.pdf`
  - sha256 `7f546cb17c404bab1fe5de08b17b881f2bb40c20822a52690ec758481e8283ce`
  - size 93,076 bytes, 15 pages (adjudicated FAIL artifact, P5-W1 restoration target)
- Reference: `round-7-adjudication.md` (this package) — findings G-1…G-6.
- Standing laws all apply. Kernel FROZEN — **no file under `core/` may be touched in this directive.**

## Tasks

### T20.1 — v1.0.3 historical restore (G-1, closes D-T7-3, R4, Phase 5)
1. Verify the provided file BEFORE copying: `sha256sum staff-provided/Weft-Whitepaper-v1.0.3.pdf` must print `7f546cb17c404bab1fe5de08b17b881f2bb40c20822a52690ec758481e8283ce`. If it does not match, STOP and report — do not substitute anything.
2. Copy byte-for-byte over `reports/Weft-Whitepaper-v1.0.3.pdf`.
3. Update the v1.0.3 row in root `SHA256SUMS` to the new hash (one row change; no other rows).
4. Keep `reports/Weft-Phase4-Errata-R4-Correction-Slip.{md,pdf}` untouched (it stays accurate: the impostor existed historically; the slip documents it).
5. Add one line to the repo worklog: "D-T7-3 CLOSED: adjudicated artifact restored from staff-provided package; hash verified before copy."

### T20.2 — Banner claim-scope fix (G-3)
In `packages/flutter_weft/lib/src/weft_reference.dart`, EITHER:
- (a) embed the PORTS.md divergence note **byte-verbatim** (full text, including the semicolon in "Single-isolate reference;" and the final sentence "This is the honesty load-bearing wall of the Dart port."), OR
- (b) keep the current banner and change D-14 report AC-5 wording from "verbatim" to "substantive (non-verbatim) PORTS.md honesty banner".
Pick one. Do not leave AC-5 claiming "verbatim" unless (a) is byte-exact.

### T20.3 — Minisign declaration + hygiene (G-4, G-5)
1. `reports/D-18-REPORT.md` (and its `D-report/` copy): append one deviation-style line — "Prior mock signature (`MockSignatureHeader`) superseded by genuine Ed25519 keypair in repair commit 958aa53; mock never used for any published artifact."
2. Add `minisign/` to `.gitignore` (secret-key output dir of `tools/minisign_tool.py`).
3. In `tools/minisign_tool.py`, guard the keygen behind an explicit flag: running `python3 tools/minisign_tool.py` with no arguments must print usage and exit non-zero; keypair generation only under `--generate`.

### T20.4 — Release-signing gate rule (G-2, codified)
Add a "Release signing gate" subsection to `evidence/D-18/RELEASE-NOTES.md` (or the release README):
> For the real `v0.1.0` (not rc1): artifacts are built by the tag-triggered workflow; `SHA256SUMS` is generated **inside CI from the CI-built artifacts** and signed **inside CI** with the release secret key. The published files and the signed manifest must come from the same build. Locally-built hashes are never shipped as canonical (non-reproducible formats differ: xcframework/AAR/tools/rc1-tarball embed timestamps — rc1 showed 4/10 row drift between local and CI builds).

## Acceptance criteria (staff round-8 = 3 mechanical checks)

| # | Criterion | Threshold |
|---|---|---|
| AC-20.1 | `sha256sum reports/Weft-Whitepaper-v1.0.3.pdf` | `7f546cb1…` exact |
| AC-20.2 | `sha256sum -c SHA256SUMS` | 15/15 OK, v1.0.3 row == `7f546cb1…` |
| AC-20.3 | Correction slip present | both `.md` and `.pdf` in tree, untouched |
| AC-20.4 | `git grep -c "verbatim" packages/flutter_weft/lib/src/weft_reference.dart` OR D-14 AC-5 text | consistent with chosen T20.2 option |
| AC-20.5 | `.gitignore` contains `minisign/`; bare tool run exits non-zero | observable in CI or log |
| AC-20.6 | T20.3 declaration line present in both report copies | text match |

## Report format
`reports/D-20-REPORT.md` — PASS/FAIL per AC with scope + threshold, deviations field mandatory, environment tags on all numbers. Evidence under `evidence/D-20/` (hash output, gitignore diff, usage-exit log).
