# DIRECTIVE SERIES 1x — END-TO-END BUILD PROGRAM

Issued by: staff (planning/adjudication side) · Date: 2026-09-15 · Status: ACTIVE
Supersedes sequencing in WO-P5-VERIFICATION.md (R1–R6 batch is folded into D-10; all other WO content remains canon).

---

## 0. Owner pivot (binding)

The owner has ruled: **cross-device testing is ABANDONED. The program is now "build the project end to end."**

Consequences, binding on every directive in this series:

1. **No device-lab tasks exist anywhere in this series.** Phase 6+ device-gated work is withdrawn from the roadmap ledger.
2. **All performance numbers remain environment-tagged** (`x86_64-sandbox`, `node20`, `linux-ci`, `ios-simulator`, …). No number may be presented as a device number. Nothing on hardware was measured; nothing on hardware will be claimed.
3. **Device-shaped work ships as design-only, honestly labeled** — e.g. the ProMotion 120 Hz path is *implemented and simulator/build-verified*, never device-benchmarked.
4. **The R1–R6 integrity batch is NOT device work and is NOT abandoned.** The release ledger's hash contract is load-bearing; those repairs move into D-10 and gate everything else.
5. C2 (soak, World A) and C3 (errata) are already CLOSED per round-5 adjudication — no further soak runs are owed.

## 1. Series map

| ID | Title | Wave | Depends | Effort | Status |
|---|---|---|---|---|---|
| D-10 | Baseline: R1–R6 integrity batch + directory harmonization + CI skeleton | 0 | — | ~4 h | CLOSED-on-G1 (round-7) |
| D-11 | npm: @weft/core, @weft/react, @weft/vue, @weft/svelte, @weft/react-native | 1 | D-10 | ~8 h | CLOSED (round-7) |
| D-12 | Android: dev.weft AAR, JNI libweft.so (3 ABIs), Compose lib, R8 rules | 1 | D-10 | ~12 h | CLOSED (round-7) |
| D-13 | iOS: Swift Package Manager package, dual-path Heddle (60/120 Hz) | 1 | D-10 | ~8 h | CLOSED (round-7) |
| D-14 | Flutter: dart:ffi production bridge into libweft.so | 1–2 | D-12 (.so), D-10 | ~8 h | CLOSED (round-7; G-3 wording in D-20) |
| D-15 | Showcase demos: W1–W5 with A/B/C/D mode toggles (web) | 2 | D-11 | ~10 h | CLOSED (round-7) |
| D-16 | DevTools: telemetry inspector + .weftrec playback | 2 | D-11 | ~10 h | CLOSED (round-6) |
| D-17 | Protocol RFC spikes Q1–Q5 (timeboxed; kernel FROZEN) | bg | D-10 | ≤ 8 d total | CLOSED (round-7: 0003 design-accepted/HW-deferred) |
| D-18 | Release engineering: tag → artifacts → GitHub Releases | 3 | D-10…D-14 | ~6 h | CLOSED (round-7; G-2/G-4/G-5 in D-20) |
| D-19 | End-to-end integration gate → coordinated v0.1.0 | 4 | all above | ~6 h | CLOSED (round-7) |
| D-20 | Final closure batch: v1.0.3 restore + label/hygiene fixes | 5 | round-7 | ≤ 1 h | ISSUED |

## 2. Sequencing rationale (what ahead, in order)

- **Wave 0 — D-10 first, before any new code.** The repo currently carries five open integrity findings (historical mutation, false deviation narrative, canonical-bundle stamp, report clipping, dual-tree duplication). Building packages on top of that bakes the rot into every artifact. D-10 is ~4 h and mechanical.
- **Wave 1 — the four packaging tracks start together.** D-12 (Android/NDK 3-ABI matrix) is the longest pole and must start immediately. D-11 (npm) is the fastest ship and unblocks both demo tracks. D-13 needs a macOS runner — if runner quota is the constraint, it starts but gates later. D-14 consumes D-12's libweft.so artifacts.
- **Wave 2 — demos and DevTools** sit on top of the packages. They are the user-facing proof that the kernel model survives real UI frameworks.
- **Background — D-17 RFC spikes** run strictly timeboxed and never block a wave. The kernel stays FROZEN; spikes produce decision memos, not kernel diffs.
- **Closure — D-18 then D-19.** Release automation precedes the integration gate so the coordinated v0.1.0 ships through the pipeline it will forever after use.

## 3. Standing laws (verbatim, apply to every directive)

1. **Law 4 — no false statements.** Every claim maps to evidence in the returned bundle. A verification claim must name **scan scope + numeric threshold**, and the scope must be TRUE.
2. **Failure protocol:** if a gate fails, either fix and re-run, or return an explicit RED report naming the failing check. Silence and rewrites of scope are both violations.
3. **Deviations field is mandatory** in every report: any quantitative miss (time, count, size, duration) is declared, not absorbed.
4. **Telemetry is advisory, never a correctness reference.**
5. **Kernel FROZEN.** `core/c/weft.{c,h}` and the Rust mirror receive ZERO diffs in this entire series. The one sanctioned extension remains the read-only debug-view accessor (semver minor). RFC spikes live in `rfcs/` + `spikes/` only.
6. **Environment tagging:** every number carries the environment that produced it.
7. **Historical artifacts are immutable.** Any write to an adjudicated artifact is a HIGH finding (P5-W1 precedent). Rebuilds produce new versions, never overwrite.

## 4. Report format (every directive)

Return per directive:
- `reports/D-<nn>-REPORT.md` with: per-acceptance-criterion PASS/FAIL table (scope + threshold stated per line), deviations field, environment tags, and the evidence file list.
- Evidence under `evidence/D-<nn>/` (logs, hashes, fixtures). Hashes as `sha256sum` output, one canonical `SHA256SUMS` delta per directive.

## 5. Ledger

- Phase 2/3/4/5: CLOSE on D-10 completion (R1–R6 executed and round-6 verified).
- This series: OPEN. Status column above is updated by staff on each adjudication round.
