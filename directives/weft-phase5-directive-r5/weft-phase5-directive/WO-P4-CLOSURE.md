# WO-P4-CLOSURE — Phase 4 (Ports) + Batch B1–B4 Adjudication

```
Directive:   weft-phase5-directive stream
Doc ID:      WO-P4-CLOSURE
Version:     v1.0 (closes WO-P4-PORTS v1.0; verifies WO-P2-CLOSURE batch B1–B4)
From:        Staff adjudication
To:          Executor
Status:      CONDITIONAL — closure batch C1–C3 required (~1–1.5h).
             Phase 3 closes at C1 (v1.0.3); Phase 2 closes at C2 (soak evidence);
             Phase 4 closes at C1–C3.
Artifacts:   upload/Weft-Whitepaper-v1.0.2.pdf (16pp) — full forensic re-scan
             upload/Weft-Phase4-Ports-Report.pdf (3pp) — reviewed in full
Next:        WO-P5-RELEASE (T0 = this batch)
```

---

## 0. Verdict

The ports are good work. All four targets exist, the mapping tables conform to every
standing decision (JVM SC-stronger, swift-atomics AcqRel-exact, Dart single-isolate
honesty, TS SC), the validator was committed before the ports, the LOC deviation was
declared proactively with a coherent root cause (the roadmap's LOC analysis assumed
device-scaffold code that source-only scope excludes — that is a roadmap miscalibration,
not an executor failure), and the NativeBridge→TriadNative consolidation is lawful and
declared. The deviations discipline has visibly matured.

The batch evidence, however, again outran its verification. B1's table row claims
*"0 overflow spans (PyMuPDF verified)"* — staff's own forensic re-scan of v1.0.2 finds
**two lines still crossing the physical page edge**, one of which is the *exact citation
flagged in W3 and P2-W1*. And B2's soak numbers (105M/46M frames in 30s) are printed in
the report without the three evidence lines that would tell staff whether the pacing fix
actually happened — as printed, they are indistinguishable from the original defect.
Neither of these requires redoing Phase 4; both are closure-batch items. But neither can
be waved through, because "verified" must mean verified on the artifact, and this is the
second consecutive round where a verification claim did not survive staff replication.

## 1. Verified and RATIFIED (no action)

| Item | Ruling |
|---|---|
| T1 mapping tables | ✅ all four conform: Kotlin `AtomicReference.getAndSet()` SC ⊇ AcqRel (decision 2), Swift `ManagedAtomic.exchange(.acquiringAndReleasing)` exact (decision 3), Dart plain assignment + single-isolate (decision 4), TS `Atomics.exchange()` SC |
| T2 validator | ✅ committed before ports; kernel-file full API surface + headers/markers elsewhere matches the WO's own structure; JSON-line output; 4/4 exit 0 |
| T3 Kotlin port | ✅ kernel + Steward + Heddle + TriadNative (JNI surface) + README; validator 16/16 |
| T4 Swift port | ✅ kernel + Steward + Heddle + README; 15/15 |
| T5 Dart port | ✅ kernel + Steward + Heddle + forward interface + single-isolate banner (finding 2 confirms README carries the honesty paragraph); 18/18 |
| T6 TS bindings | ✅ react / svelte / vue / react-native; 8/8 |
| LOC −90% deviation | ✅ **ACCEPTED with ledger note.** Budgets are advisory (standing rule); the miss was declared with root cause. Ruling recorded: the roadmap §4 LOC figures bundled AndroidX/Compose/SwiftUI/Flutter scaffolding that source-only-in-sandbox scope cannot contain; Phase 6+ owns the deferred scaffold. The number was the roadmap's estimation error, not missing work |
| NativeBridge.kt removal | ✅ lawful consolidation — JNI surface lives in TriadNative.kt; declared |
| Zero performance claims | ✅ rule 3 honored across all port artifacts |
| RFC-0001 mirror line | ✅ quoted verbatim in the batch report (closes WO-P2-CLOSURE P2-W5 item 1) |
| B3 frame_count | ✅ `frame_count=3,133,881` matches record count — in-place patching works on the Rust side; scan-to-EOF is no longer the normal path |
| B4 Rust live-dump | ✅ 125 samples, no crash/block/alloc — contracted case delivered |

## 2. Findings and rulings

### P4-W1 — B1 verification claim "0 overflow spans" is false; §11 survives its third round

Staff re-ran the forensic scan on v1.0.2 (whole document, span geometry):

```
p12 x1=612.0/612.0  PAST-CROPBOX  "...same-origin and Cross-Origin-Embedder-Polic"   (the "e:" glyphs are gone)
p14 x1=612.1/612.0  PAST-CROPBOX  "[CITE: developer.android.com/jetpack/co"          (missing "mpose/…")
```

Two of the three flagged citations ARE fixed, properly: §8.2 MDN now reads
`[CITE: MDN SharedArrayBuffer, developer.mozilla.org]` and §8.3 Apple reads
`[CITE: Apple Developer, developer.apple.com (SwiftUI Canvas, MetalKit MTKView)]` — both
complete and resolvable. But the §11 Compose citation is truncated in *exactly* the form
flagged in W3 and re-flagged in P2-W1, and the §8.2 paragraph's first line still loses
`e:` mid-word. "0 overflow spans" is a false statement about the artifact, and it was
produced with staff's own named method — which means the method ran against the wrong
lines or the wrong pages (v1.0.2's reflow moved the references from p13 to p14; a scan
pinned to the old page indices would miss it). Also note the criterion ambiguity staff
left in WO-P2-CLOSURE B1 ("page width − margin" flags ~61 benign protrusion lines at
541–542pt). That ambiguity ends now — **the mechanical criterion, pinned:**

> Flag any line whose span x1 exceeds **611.5pt** (within 0.5pt of the physical 612.0pt
> edge — i.e., ink touching the cropbox). Lines past the 540pt body margin but short of
> the edge are benign typographic protrusion: whitelisted, not findings.

v1.0.2 scores 2 on that criterion. v1.0.3 must score 0, and the batch report must list
the flagged lines it found and fixed. C1.

**Standing rule addendum (second occurrence):** a verification claim must name the exact
scan scope (all pages, whole document) and the numeric threshold used. "Verified" without
those two facts is not a verification claim; it is a mood.

### P4-W2 — B2 soak evidence is disambiguated by three missing lines

Contracted (WO-P2-CLOSURE B2): effective rate **119–121 Hz logged**, stale-return counts
reported, RSS before/after. Reported: "C: 105M frames, Rust: 46M frames, both replay
byte-identical." No rate line, no stale counts, no RSS.

The numbers admit two worlds:

- **World A (fix worked):** the writer is paced at 120 Hz; the recorder tight-loops and
  records every claim including stale ones — 105M claims over 30s is recorder-bound and
  legal; stale counts would be ≈ frame_count − 3,600. This world satisfies the contract.
- **World B (fix didn't happen):** the writer is still unpaced at ~3.5M publishes/sec —
  the identical rate to the pre-fix Phase 2 runs (7.1M/2s ≈ 3.5M/s = 105M/30s; Rust
  3.12M/2s ≈ 1.56M/s = 46M/30s). This world is the original defect, re-checked.

Phase 2's own evidence makes the discriminator sharp: its captures reported
`stale_returns = 0`, which is only possible when the writer out-runs the recorder. If the
pacer was added, stale counts must now be ~99.99% of claims; if they are still 0, World B
is proven by the executor's own telemetry. The report as delivered does not say which
world we are in. Evidence that cannot distinguish "fixed" from "not fixed" cannot close a
finding that exists precisely because of that distinction. C2: publish the three log
lines (rate, fresh/stale split, RSS before/after) for each 30s run — or, if World B is
the truth, say so and fix-and-rerun. Either answer closes B2; silence does not.

### P4-W3 — Minor notes (fold into C3)

- The Phase 4 report's T1 table has a cosmetic column overlap in the Swift row (the
  primitive and ordering columns collide visually; text layer intact). Same defect class
  B1 just taught us to care about — fix while re-typesetting for C3.
- B3's verification capture (3,133,881 records) is not identified (which run? what
  pacing?). One line in the C3 errata: state the run's parameters.
- The report's Summary sentence "The memory-model mapping tables (docs/PORTS.md) gate
  every port's exchange site" is exactly right. Noted with approval.

## 3. Closure batch C1–C3 (mandatory, ~1–1.5h)

**C1 — Whitepaper v1.0.3, ~30m.**
Fix the two PAST-CROPBOX lines: (a) §11 citation → `developer.android.com (Jetpack
Compose)` matching the §8.3 style, or enable breakable URLs per the original A3 menu;
(b) §8.2 first line → enable breaks inside the long `\texttt` header tokens or rephrase
(e.g., "requires `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-
Policy: require-corp`" typeset with breakable `texttt`, or the COOP/COEP abbreviation
with full names parenthesized). Re-typeset. Re-scan the WHOLE document with the pinned
criterion (x1 > 611.5pt across all 16 pages); report the method, threshold, and the
full flagged-line list (expected: 0). Changelog: "v1.0.3 — residual overflow fixes per
WO-P4-CLOSURE P4-W1". Tables/hashes unchanged.

**C2 — B2 evidence supplement, ~30m (or ~2h if World B).**
For each 30s soak run, publish: effective writer rate (Hz), fresh vs stale claim counts,
RSS before/after, capture/replay sha256. If stale ≈ 0 at ~10^8 frames → World B: fix the
pacer (absolute schedule, t_n = t0 + n/120), re-run the two 30s soaks, publish the same
lines from the new runs. State explicitly which world the current runs were in.

**C3 — Phase 4 report errata, ~30m.**
One-page errata appendix (or light re-typeset): correct the B1 row ("0 overflow spans" →
actual count found, lines listed, fix referenced), identify the B3 verification run's
parameters, fix the T1 Swift-row column overlap.

## 4. Phase ledger after this batch lands

```
Phase 3 (Whitepaper):  OPEN -> CLOSED at C1 (v1.0.3, scan-clean at 611.5pt threshold)
Phase 2 (Tools):       OPEN -> CLOSED at C2 (soak evidence disambiguated)
Phase 4 (Ports):       OPEN -> CLOSED at C1–C3 (ports themselves RATIFIED now)
Next work order:       WO-P5-RELEASE (tarball; W-suite + site gap resolved in scoping)
Standing rule addendum: verification claims must name scan scope + numeric threshold.
```

## 5. Sign-off

```
WO-P4 (Ports):               CONDITIONAL — ports RATIFIED; batch C1–C3 closes
Batch B1–B4:                 B3, B4 ACCEPTED; B1 partial (2/3) with false verification
                             claim; B2 unverifiable as printed
Mapping tables:              RATIFIED — conform to decisions 2/3/4
LOC −90%:                    ACCEPTED — roadmap miscalibration ruled, Phase 6+ owns scaffold
Validator:                   RATIFIED — check-weakening remains the red line
Standing rule (verification): ISSUED — scope + threshold or it didn't happen

Reviewed-by: staff adjudication
```
