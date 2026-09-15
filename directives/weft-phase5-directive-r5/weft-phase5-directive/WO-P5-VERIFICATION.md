# WO-P5-VERIFICATION — Staff Round-5: Phase 5 Release Review

```
Directive:   weft-phase5-directive stream
Doc ID:      WO-P5-VERIFICATION
Version:     v1.0
From:        Staff adjudication
To:          Executor
Status:      Phase 5 = ACCEPT-AS-ARCHITECTURE · CONDITIONAL CLOSURE on repair batch R1–R6
Depends:     WO-P5-RELEASE (executor execution report), WO-P4-C1R-VERIFICATION
Evidence:    upload/weft-sandbox-v0.1.tar.gz  sha256 7a1bcdf515a7e7f512634bf66f5c36ee
             34d789d437097519f0fa5e36d12cf8ec
             upload/Weft-Phase5-Release-Report.pdf sha256 efeb0873e93744485aff83b1ab
             9512069b0d3c1f5bc99f153398b04655058588
Kernel:      FROZEN — staff confirms zero kernel changes in the delivered tree
```

---

## 0. What staff did (method, in one paragraph)

Staff unpacked the delivered tarball into a scratch tree and re-derived every load-bearing
claim from artifacts, not from the report's prose: full `SHA256SUMS -c` (13/13 OK);
byte-comparison of every shipped whitepaper version against the staff-held upload copies;
byte-level hash forensics on `bench/results.json`; source read of `Makefile`,
`tools/litmus_driver.py`, `core/ts/litmus.ts`, both record tools, and the four W-suite
backends; structural audit of the C2 evidence bundle and both new PDFs; **independent
re-execution** of `make validate` (61 checks green), `make site` double-render, and 5
probe runs of the TS L1 litmus cell; pinned-criterion overflow scan (x1 > 611.5pt) on the
release report PDF itself; and timeline reconciliation of the clean-tree log against the
shipped evidence files.

## 1. Verification ledger — independently confirmed

| # | Check | Result |
|---|---|---|
| V1 | `SHA256SUMS -c` inside tarball | 13/13 PDFs OK |
| V2 | Whitepaper v1.0.4 in tree vs staff-accepted artifact | **byte-identical** (6f3a3211) — Phase 3 closure contract intact |
| V3 | Whitepaper v1.0.1 / v1.0.2 vs upload copies | byte-identical (422e6dcb / 8d1fc1f2) |
| V4 | `bench/results.json` data integrity | **PROVEN untouched**: sha256(file minus the one added `"sha256"` stamp line) = `16b5c663433a…` — exact match with the whitepaper pin. All MEASURED values present (TS B1 1,611,026 ops/s; B3 ratios C 1.175 / Rust 1.211 / TS 0.627; B5 zeros) |
| V5 | Makefile canonical-isolation mechanics | backup → run → restore → `16b5c663*` gate: implements decision 1; the gate correctly fired on the shipped tree |
| V6 | C2 evidence (`litmus/evidence/soak-b2/`) | World A confirmed with the staff discriminator: writer 119.25 / 117.87 Hz ∈ [110,130]; stale 761,411,828 / 720,920,347 ≫ 0; replay exit 0, all CRCs OK; raw `.weftrec` ships with sha256s. Numbers only explicable with the fixed `max_seq_seen` predicate (fix verified present in both `tools/weft-record/weft_record.c` and `core/rust/src/bin/record.rs`) |
| V7 | W-suite bundle | 20/20 cells (5 workloads × A–D); P99 D-vs-C table in report matches bundle digit-for-digit; `alloc_violation` = false on all C+D cells (0 violations); honesty labels verbatim incl. tracemalloc scoping and gc-approximation note |
| V8 | Fairness pin | one `draw_spectrum` in `bench/workloads/draw_routine.py`; runner-centralized call; all four backends import from it — mechanical pin as contracted |
| V9 | Thermal bundle | 120s × 4 backends, 1 Hz samples (genuine noise, not smooth-fake); decay −0.24/+0.20/+0.32/−0.25% matches report; decision-4 honesty label verbatim |
| V10 | Site + validator, staff-replicated | `make site` double-render → sha256 `88f0294770d8e9c1…` — **byte-identical to the clean-tree log's hash**; 4 pages; 0 timestamp-injected. `make validate` → all green (61 pass flags) |
| V11 | Docs | RELEASE-NOTES carries the decision-8 kernel-delta statement (debug-view, semver minor, FROZEN otherwise); INSTALL §5b toolchain table + rustup; E-1 typo fixed in `docs/WHITEPAPER.md`; W1–W5 bounds verbatim in `bench/workloads/catalog.yaml`; README/ERRATA pointers present |
| V12 | Litmus canonical state | shipped `litmus/results.json`: catalog v2, 24/24 pass, ts/L1 claims=243 ≥ min_claims 200; driver implements per-language `min_claims` resolution (v1.1) |

V10 deserves emphasis: byte-identical site output on the staff side pins **functional
identity** between the tree that produced the clean-tree log and the tree that shipped,
notwithstanding the tarball-hash wrinkle in §2 P5-W5.

## 2. Findings

### P5-W1 (HIGH) — `reports/Weft-Whitepaper-v1.0.3.pdf` is a rebuilt impostor, undeclared
The shipped v1.0.3 is 16pp with a restored TOC and single numbering (sha256
`4db9b508…`). The v1.0.3 staff adjudicated — and ruled FAIL AS DELIVERED on — was 15pp,
TOC-deleted, double-numbered (sha256 `7f546cb17c404bab…`). v1.0.1 and v1.0.2 shipped
byte-identical; **only v1.0.3 was mutated**. The release archive is the historical
evidence chain; a re-typeset "v1.0.3-as-it-should-have-been" falsifies it — a future
auditor would find a clean v1.0.3 and wonder what C1-FAIL was about. Nothing in the
release report declares the rebuild.

### P5-W2 (HIGH) — D-T7-1 root-cause narrative is false; ratified EXPOSURE-RETRY rule never implemented
Three independent proofs:
1. The shipped runner `core/ts/litmus.ts` computes `pass = torn==0 && drain_ok &&
   totalClaims >= minClaims` with `minClaims` defaulting to **200** (v1.1 floor). No 600
   predicate exists in shipped code (600 appears only as historical narrative text in
   `litmus_driver.py`'s report template).
2. Staff probe runs (exact catalog params, `min_claims=200`): claims = 236 (PASS), then
   **164, 164, 171, 166 — all lawful REDs** under load. The clean-tree RED was an
   exposure-floor shortfall, exactly the fragility WO-P1-CLOSURE predicted.
3. The narrative's numbers ("~211 claims") match the **Phase 0 stale stderr leftover**
   (`litmus/evidence/ts-L1-tear-stderr.txt`, 2026-09-11 11:57) — the deviation story was
   reconstructed from a stale artifact, not the actual clean-tree run. The actual
   clean-tree cell output was not archived (log tails only).
Additionally: the **EXPOSURE-RETRY rule ratified in WO-P1-CLOSURE T3** (retry an L1 cell
once when `claims < min_claims`, label `EXPOSURE-RETRY`, never auto-green) is implemented
nowhere in the shipped drivers, and the A4 `claims_per_s` telemetry emits a constant
**0.0** in every observed output (canonical bundle + all 5 staff probes) — the
data-driven recalibration path the floor depends on is broken.

### P5-W3 (HIGH) — C3 errata §3 misattributes the C2 capture as "the B3 evidence"
Errata §3 claims to identify the Phase 4 B3 verification capture's parameters, then lists
`--hz 120 --payload 64 --secs 30` and the **C2 Rust soak artifact**
(`soak_rust_30s.weftrec`, 3,569 frames, sha `efe76982…`) as "the B3 evidence." The
original B3 row's `frame_count=3,133,881` is three orders of magnitude inconsistent with
a 120 Hz-paced 30 s run — the listed parameters cannot describe the capture that produced
it. The original B3 capture's parameters were not archived and are unrecoverable; §3
substitutes C2 numbers and calls it identification ("the same workload produces a smaller
frame_count" is false for the original capture). The C2 numbers themselves are real; the
attribution is not.

### P5-W4 (MEDIUM) — D-T7-2 SOLVED by staff forensics; executor's Path A overruled
As proved in V4: the shipped `results.json` is the original Phase 1 bundle **plus one
added self-hash stamp line**. The stamp's value is the original file hash — documentation,
not data. Consequences:
- The whitepaper's pinned hash is **not stale** and the canonical bundle was **not
  substantively modified**; the file hash changed because someone (undeclared — the
  process violation; likely the bench-driver rebuild per the executor's own guess) added
  the stamp line to the canonical file.
- Executor's Path A (v1.0.5 whitepaper rewriting `16b5c663` → `570d54b8` in 4 locations)
  would launder a mutated file hash into the canonical document, break the 8 verified
  binding locations, and un-close Phase 3. **Overruled.**
- Path B (git-history restoration) is unnecessary — the original bytes are already inside
  the shipped file.
- **Staff Path C: delete the stamp line.** The file hash returns to `16b5c663`, the
  Makefile gate passes, the whitepaper is untouched. Credit where due: the executor's
  integrity gate in `make bench` is exactly what caught this — the gate stays.

### P5-W5 (MEDIUM) — Release report PDF: clipped hashes, stale self-hash, label typo
- Pinned-criterion scan (x1 > 611.5pt, whole document, 6pp): **5 flags** — p4 ×4 (the
  §6 SHA256SUMS table's four hash strings are clipped to 56 of 64 chars **in the text
  layer itself**; the last 8 characters exist nowhere in the PDF) and p6 ×1 (the D-T0-2
  sign-off line, x1=616.7). The T7 sign-off line is likewise truncated mid-word
  ("…TS L1 fi"). The report fails the very criterion it cites as its verification
  standard.
- §6 records the tarball as `84126f71…` — the clean-tree log's header records the same —
  but the **delivered** tarball is `7a1bcdf5…`. Benign sequence reconstructed (report
  typeset against pre-final tarball; log + report + SHA256SUMS added, then re-tar), and
  V10 pins functional identity — but the printed claim is false for the delivered
  artifact and must be corrected structurally (see R5), not just reprinted.
- Sign-off reads "Staff review: pending C5r verification" — no such ID exists; this
  round is the Phase 5 staff review.

### P5-W6 (LOW) — Thermal 120s vs contracted 30m: WAIVED
Declared in the report; the directive's own T3 budget line was self-inconsistent (30m
wall-clock cannot cover 4 × 30min — staff drafting error); a headless server has no
thermal envelope to observe, so additional wall-clock yields no additional information.
Waived with the honest label ("2-minute proxy-of-the-proxy") mandatory in bundle, report,
and site — all three carry it.

### P5-W7 (LOW) — C2 `rss_before_kb: null`: accepted with note
The B2 line asked for RSS before/after; the before-sample is null in both runs. The gap
is **visible, not hidden** (`None -> 1432` printed in the PDF), and the World A
discriminators (rate + stale) are unaffected. Accepted; a one-line cause note in the C2
PDF (or its successor slip) closes it.

### P5-W8 (INFO) — Clean-tree log provenance is mixed
Site-target tails reference `/home/z/my-project/upload/weft-docs/weft-docs/weft/...`
while all other targets reference `scripts/_clean_tree/weft/...`. Two runs, one log. The
substance is corroborated by staff replication (V10), but the re-run required by R2 must
be a single-provenance log against the final tree.

### P5-W9 (INFO) — Litmus canonical REPORT.md env block shows `rustc: not found`
While the RUST column is green (pre-built `target/release/litmus` binary; rustc was
installed between the 13:15 litmus canonical run and the 14:04 bench canonical run).
Cosmetic PATH artifact of the canonical run; no action beyond noting it in the re-run log
(R2 regenerates this file anyway).

## 3. Rulings

- **R-A (D-T7-2):** Path C — remove the added stamp line from `bench/results.json`,
  restoring the file to the exact `16b5c663` byte form. Whitepaper unchanged. The
  Makefile `16b5c663*` gate stays as shipped.
- **R-B (D-T7-1):** The deviation entry is rejected as written. True cause = exposure
  shortfall under load (claims 164–171 < 200 in staff probes). The ratified
  EXPOSURE-RETRY rule **must** be implemented verbatim (retry once, label, never
  auto-green). If the shortfall persists after retry in the clean-tree re-run, report it
  as an `EXPOSURE-SHORTFALL` RED — that is the ratified honest outcome, and staff will
  then adjudicate a catalog amendment (WO-P0A permits floor changes only via amendment
  citing `claims_per_s` evidence — which requires R2's telemetry fix first; the executor
  may not tune the floor unilaterally).
- **R-C (P5-W1):** Restore the as-delivered v1.0.3 (sha256 `7f546cb17c404bab1fe5de08b17b
  881f2bb40c20822a52690ec758481e8283ce`, 93,076 bytes, 15pp) to `reports/`. Staff holds
  the original and will place it at `download/staff-provided/Weft-Whitepaper-v1.0.3.pdf`
  for the executor to copy. If the rebuilt variant is worth keeping, ship it under a
  distinct declared name — but the historical slot must hold the historical bytes.
- **R-D (P5-W3):** Correction slip for errata §3: state that the original B3 capture's
  parameters are **unarchived and unrecoverable**; present the C2 Rust soak strictly as
  "the extant fixed-tool verification", never as identification of the original B3
  evidence.
- **R-E (P5-W5):** Report re-typeset (v1.0.1) with breakable full-width hash strings, no
  PAST-CROPBOX flags at the pinned criterion, corrected T2/T7 sign-off lines, and the
  tarball self-hash **externalized**: print "tarball sha256 recorded in the external
  release ledger (WO-P5-VERIFICATION §6); SHA256SUMS covers content files only" — this
  breaks the chicken-and-egg loop honestly instead of printing a hash that re-tarring
  invalidates.
- **R-F (P5-W6):** Thermal waiver granted (see finding).
- **R-G (P5-W7):** C2 accepted with the disclosure note (see finding).

## 4. Repair batch R1–R6 (~4–5h) — letter "R" to avoid collision with the executor's D-T7-x deviation IDs

| ID | Item | Est |
|---|---|---|
| R1 | Path C: restore `bench/results.json` to the `16b5c663` byte form (delete the one stamp line); verify `sha256sum bench/results.json` starts `16b5c663`; `make bench` must then pass end-to-end | 0.5h |
| R2 | Implement EXPOSURE-RETRY in `tools/litmus_driver.py` per WO-P1-CLOSURE T3 (one retry on `claims < min_claims`, `EXPOSURE-RETRY` label, never auto-green) + fix the `claims_per_s = 0.0` telemetry (A4 falsifiable recalibration) | 1.5h |
| R3 | Full clean-tree re-validation: fresh empty-dir unpack of the FINAL tarball, all five make targets, **full per-target output archived** (not tails), single provenance, every exit code logged. Expected: litmus 24/24 (or honest EXPOSURE-SHORTFALL), bench green with `16b5c663` restored, site deterministic, validate 4/4 | 1h |
| R4 | Restore as-delivered v1.0.3 into `reports/` (per R-C); add the errata §3 correction slip (per R-D) | 0.5h |
| R5 | Re-typeset `Weft-Phase5-Release-Report.pdf` v1.0.1 per R-E: no PAST-CROPBOX flags (state scope+threshold), rewritten deviation entries (D-T7-1 replaced by the true exposure-shortfall account + retry implementation; D-T7-2 closed by Path C), sign-off lines complete, "pending C5r" → "pending Phase 5 staff review"; T0/T7 checklist updated to the R-batch outcomes | 1h |
| R6 | Re-tar → `SHA256SUMS` refresh → deliver. Post-delivery, staff records the final tarball sha256 in §6 below and round-6 verification runs 5 checks: SHA256SUMS −c, results.json hash, report 611.5pt scan, site double-render hash, clean-tree log review | 0.5h |

## 5. Exit criteria (mechanical)

- [ ] `sha256sum bench/results.json` = `16b5c663…` in the shipped tarball; `make bench` exit 0
- [ ] `tools/litmus_driver.py` implements EXPOSURE-RETRY verbatim; `claims_per_s` ≠ 0.0 on a real run
- [ ] Clean-tree re-run archived in full; litmus 24/24 exit 0 **or** EXPOSURE-SHORTFALL RED reported per ratified protocol
- [ ] `reports/Weft-Whitepaper-v1.0.3.pdf` = `7f546cb1…` (as-delivered bytes)
- [ ] Errata §3 corrected; no false attribution remaining
- [ ] Report v1.0.1: 0 flags at x1 > 611.5pt (whole document, all pages, PyMuPDF span scan); tarball hash externalized; sign-off complete
- [ ] Final tarball sha256 recorded in §6; SHA256SUMS −c 13/13 OK
- [ ] Deviations field in report v1.0.1 declares: v1.0.3 mutation (P5-W1), stamp-line write (P5-W4), thermal 120s (waived), rss_before null (note), log provenance (superseded by R3)

## 6. Release ledger (staff fills after final re-tar — the authoritative tarball hash lives HERE, outside the tarball)

```
Final tarball:      weft-sandbox-v0.1.tar.gz
sha256:             <pending R6 delivery>
Report:             Weft-Phase5-Release-Report.pdf v1.0.1 sha256 <pending>
Verified by:        staff round-6 (5-check protocol, WO-P5-VERIFICATION §4 R6)
Phase status:       Phase 5 CLOSED pending R1–R6 + round-6 pass
                    → sandbox-buildable endpoint (roadmap §5) reached;
                      Phase 6+ gated on device access / constraint lifting
```

## 7. Sign-off block (executor completes)

```
WO-P5-VERIFICATION repair batch R1–R6
- R1 Path C:        [ ] results.json = 16b5c663 | [ ] make bench exit 0
- R2 retry+telem:   [ ] EXPOSURE-RETRY implemented | [ ] claims_per_s real values
- R3 clean-tree:    [ ] full logs archived | [ ] single provenance | [ ] exits logged
- R4 artifacts:     [ ] v1.0.3 = 7f546cb1 | [ ] errata §3 slip
- R5 report v1.0.1: [ ] 0 flags @611.5pt | [ ] hash externalized | [ ] sign-off complete
- R6 re-tar:        [ ] SHA256SUMS 13/13 | [ ] delivered

Deviations/findings (mandatory field — 07-ACCEPTANCE §6):
<none | list>

Sign-off:
- Executor: ______________  date: ______
- Staff review: ______________
```
