# ROUND-7 STAFF ADJUDICATION — Repair Batch E1–E5 (Series 1x closure gate)

- Adjudicator: staff · Date: 2026-09-16 · Input: repo `github.com/zephyr4289/Weft` @ `958aa53` (commit "execute repair batch E1-E5"), tag `v0.1.0-rc1`, branch `ci/release-status`, D-report.zip @ `90bf9150…`
- **Verdict: E2/E3/E4/E5 ACCEPTED. E1 NOT EXECUTED — correctly declared BLOCKED on staff. Phase 5 and Series 1x close on one mechanical item (directive-20 / G-1).**
- Headline: the executor's failure protocol worked exactly as designed. Unable to restore v1.0.3 because the staff-provided artifact was never actually shipped (round-6 package contained no `staff-provided/` — staff packaging miss, confirmed by zip listing), the executor **did not fake a restore, did not rewrite history, kept the R4 Correction Slip, and declared D-T7-3 OPEN/BLOCKED in the deviations field**. That is Law-4-compliant behavior and is credited on the record.

## 1. E-batch scoreboard (staff-replicated on `x86_64-sandbox`, not trusted from reports)

| Item | Verdict | Staff method | Result |
|---|---|---|---|
| **E2** RFC0003 simulation honesty | **PASS** | text reads + evidence-log reconciliation | RFC §Motivation retitled "Analytical Model (Simulation-Only)" with formal **Epistemic Disclosure (SIMULATION-ONLY)** block: "No physical GPU or WebGPU backend was exercised… HARDWARE-DEFERRED per the Owner Binding Pivot" (5 keyword hits). D-17 report table row self-labels `python-sim / SIMULATION-ONLY`, status **NOT ACCEPTED** (pending hardware-backed spike). Numbers reconcile: re-run log shows 85.609 µs vs 0.457 µs = 187.4x; report prints 0.46 µs / 187x — matches log. Spike `.py` re-run declared. |
| **E3** D-19 citations + Android harness | **PASS** | `ls`/`grep` + source read | Cited paths now resolve (2/2, zero missing). `android/weft-core/src/test/kotlin/dev/weft/WeftTest.kt` is a REAL 101-line JUnit harness: 1,000-frame loop asserting I1 (embedded-seq == claimed seq, no torn read), I2/I3 (step bounds), I4 (monotonicity), I5 (staleness ≤ 1), I6 (revoke+reclaim epoch ack), publish/claim totals. `parity_harness_output.log` genuine multi-surface run (Node 1000/1000 I1–I6, 0 alloc steady-state; Chromium…). `parity_matrix.json` carries the android row. |
| **E4** minisign + tag pipeline | **PASS** (with G-2/G-4/G-5 residuals) | independent Ed25519 verify + `git ls-remote` + CI branch read | `minisign.pub` real (keyID 81FA8EA048E4D43A), **signature verifies INDEPENDENTLY of the repo's own code** (staff `cryptography` Ed25519 over file bytes vs sig[10:74]: VALID). No secret material tracked. Tag `v0.1.0-rc1` pushed → `958aa53`. Tag-triggered release workflow **executed: `ci/release-status` branch shows github-actions[bot] "release status [success] for 958aa53"** (F-7 closed: pipeline now tag-exercised). |
| **E5** labels | **PASS** (with G-3 residual) | source reads | `weft_reference.dart` STATUS now "SOURCE-ONLY REFERENCE IMPLEMENTATION. Verification is build/unit/CI-only per owner pivot; cross-device hardware verification permanently withdrawn" — pivot-consistent (G-5a closed). W2 single truthful scale everywhere: title "W2: Particle System — Demo Scale (500 particles / 3000 floats @ 120Hz)", code 500×6-DOF, D-15 report "500 particles × 6-DOF (3,000 floats)" (F-6 closed via the label route). |
| **E1** v1.0.3 restore | **NOT EXECUTED — BLOCKED on staff** | zip listing + hash + executor worklog read | Repo `reports/Weft-Whitepaper-v1.0.3.pdf` still `4db9b508…` (impostor). Staff confirmed root cause: `weft-directives-1x.zip` (round-6 delivery) contained **no** `staff-provided/` entry and no PDF — the round-6 promise "staff-staged copy ships in the next directive package" was written but not executed. The adjudicated artifact (`7f546cb1…`, 93,076 B, 15 pp) exists on staff side and **is physically included in the round-7 package this time** (`staff-provided/Weft-Whitepaper-v1.0.3.pdf`). R4 Correction Slip retained in tree (`reports/Weft-Phase4-Errata-R4-Correction-Slip.{md,pdf}`). |

Also staff-verified this round: kernel freeze holds through `958aa53` (no `core/` file touched — diffstat review); re-zipped `D-report.zip` inner copies byte-sync with `reports/` (D-18/D-19 spot-checked SYNC); root `SHA256SUMS` 15/15 OK.

## 2. Residual findings (round-7)

| ID | Sev | Finding | Disposition |
|---|---|---|---|
| **G-1** | **HIGH (sole closure gate)** | v1.0.3 restore not executed — blocked on staff artifact delivery (see E1). | Staff resolves its own miss in this package. Executor: copy `staff-provided/Weft-Whitepaper-v1.0.3.pdf` → `reports/`, update root `SHA256SUMS` row to `7f546cb1…`, keep slip. See directive-20 T20.1. |
| **G-2** | MED | **Signed dist manifest describes unobtainable artifacts.** `evidence/D-18/SHA256SUMS` (signed) == RELEASE-NOTES matrix, but `dist/*` is gitignored — those exact bytes exist in no published location. CI's own tag-build produced **different hashes on 4/10 rows** (xcframework, android, tools, rc1 tarball — timestamp/host-embedding formats); 6/10 npm tgz rows identical. Acceptable for an rc1 dry-run; NOT acceptable for the real v0.1.0. | Encoded as permanent release-gate rule (directive-20 T20.4): the real release **signs the CI-produced manifest inside CI and publishes exactly those artifacts**. P5-W5 lesson, generalized. |
| **G-3** | LOW | D-14 AC-5 still claims "verbatim PORTS.md honesty banner" while the banner is near-verbatim: PORTS.md "Single-isolate reference**;** no…" → dart "Single-isolate reference**:** no…", first sentence only, load-bearing final sentence omitted. Claim-scope precision (F-5 pattern, micro scale). | directive-20 T20.2: make it byte-verbatim (full note incl. "honesty load-bearing wall" sentence) or change AC-5 wording to "substantive (non-verbatim)". |
| **G-4** | LOW | Prior mock signature never explicitly declared superseded; D-18 report now says "genuine Ed25519 keypair" without a "previously mock, replaced" line. | directive-20 T20.3: one-line deviation note in D-18 report. |
| **G-5** | LOW | `tools/minisign_tool.py` `__main__` **regenerates a fresh keypair on every run** and writes the secret to `minisign/`, which is **not gitignored** — one stray run at repo root risks committing a secret key. | directive-20 T20.3: gitignore `minisign/`, and guard `__main__` (require `--generate` explicitly). |
| **G-6** | INFO | Root `SHA256SUMS` (15 rows) does not cover `D-report.zip` (delivery archive convenience). No stale row; 15/15 OK. | Accepted as-is; manifest covers content files. No action. |

## 3. Ledger after round-7

- Phase 2/3/4: **CLOSED** (permanent; kernel canonical, bundle `16b5c663…` intact).
- Phase 5: **OPEN on G-1 only** — closes on a single mechanical check.
- Series 1x: D-10 **CLOSED-on-G1** · D-11 **CLOSED** · D-12 **CLOSED** · D-13 **CLOSED** · D-14 **CLOSED** (G-3 wording folded into directive-20) · D-15 **CLOSED** · D-16 **CLOSED** · D-17 **CLOSED** (RFC 0003 = design-accepted/hardware-deferred with honest disclosure; 0004/0005/0006/0007 accepted per round-6 §4) · D-18 **CLOSED** (G-2/G-4/G-5 folded into directive-20) · D-19 **CLOSED**.
- **Series 1x: CLOSED-PENDING-G1.** After directive-20 + staff round-8 (3 mechanical checks), the entire roadmap §5 endpoint is reached: sandbox-buildable, packaged, release-pipelined, parity-gated.

## 4. What is ahead (program after closure)

1. **directive-20 (this package)** — final closure batch, ~30–45 min executor work: T20.1 restore + manifest row, T20.2 banner/AC-5 wording, T20.3 mock-declaration + minisign hygiene, T20.4 release-signing rule codified in release docs.
2. **Staff round-8** — mechanical: `sha256(reports/Weft-Whitepaper-v1.0.3.pdf) == 7f546cb1…`; `sha256sum -c SHA256SUMS` 15/15 with the updated row; slip retained. Then Phase 5 → CLOSED, Series 1x → CLOSED, ledger final.
3. **Owner actions** (cannot be delegated to executor): registry credentials (npm / Maven Central / pub.dev / GitHub Packages), then cut the **real `v0.1.0`** tag — the tag-triggered pipeline is now proven by the rc1 run.
4. **Post-closure backlog** (staff-issued only on request): registry publishing directives; RFC 0003 implementation directive **only** if a GPU backend becomes available; launch assets (demo recording, README badges).
