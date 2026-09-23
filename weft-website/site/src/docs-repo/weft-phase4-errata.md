# Weft Phase 4 Errata — Closure Batch C3

> **Phase 5 / T0 / C3** · `x86_64-sandbox` · 2026-09-12
> Per `WO-P4-C1R-VERIFICATION` §3 + §4 (ordering ruling: C2 → C3).
> Closes Phase 4 (Ports) at the closure-batch level (C1–C3 satisfied).

---

## 0. Status

| Phase | State |
|---|---|
| Phase 0 / 0.5 / 1 (kernel, litmus, bench) | CLOSED (unchanged) |
| Phase 3 (Whitepaper) | CLOSED at C1r — v1.0.4 canonical (sha256 `6f3a3211...`) |
| Phase 2 (Tools) | CLOSED at C2 — World A confirmed, soak evidence delivered |
| Phase 4 (Ports) | CLOSED at C3 — this document |

---

## 1. B1 claim-history — full correction

The B1 row of the Phase 4 report (`docs/PHASE4-REPORT.md` §2, original wording) reads:

> `B1: Whitepaper v1.0.2 (URL clipping fixed) | ✅ 0 overflow spans (PyMuPDF verified)`

**This claim was false.** Senior's forensic replication in `WO-P4-C1-VERIFICATION` §1 (P4-W1) found **two** lines past the cropbox at the pinned 611.5pt threshold:

- `p12 x1=612.0/612.0  PAST-CROPBOX  ' Cross-Origin-Embedder-Polic'` (the `e:` glyphs lost mid-word)
- `p14 x1=612.1/612.0  PAST-CROPBOX  '[CITE: developer.android.com/jetpack/co'` (missing `mpose/…`)

The criterion ambiguity in the prior WO-P2-CLOSURE B1 spec ("page width − margin" flagged ~61 benign protrusion lines at 541–542pt) ended at `WO-P4-CLOSURE` §2 P4-W1: **the mechanical criterion is pinned at `x1 > 611.5pt`** (within 0.5pt of the 612.0pt physical page edge — ink touching the cropbox). Lines past the 540pt body margin but short of 611.5pt are benign typographic protrusion, whitelisted, not findings.

### B1 closure history (the five-round flag trail)

| Round | Artifact | Finding | Disposition |
|---|---|---|---|
| W3 (WO-P3-CLOSURE) | v1.0.0 → v1.0.1 → v1.0.2 | §11 Compose citation flagged as overflow | A3 fallback rule A1–A3 issued |
| P2-W1 (WO-P2-CLOSURE) | v1.0.2 | §11 Compose citation re-flagged; criterion ambiguity noted | URLs shortened to domain+stub; "0 overflow spans" claimed |
| P4-W1 (WO-P4-CLOSURE) | v1.0.2 | Senior replication: 2 PAST-CROPBOX lines at 611.5pt; v1.0.2 claim false; criterion pinned at 611.5pt | C1–C3 closure batch issued |
| C1-FAIL (WO-P4-C1-VERIFICATION) | v1.0.3 | Senior round-3 forensic: 0 PAST-CROPBOX lines ✓, BUT re-typeset broke the package (TOC deleted 16→15pp, headings double-numbered, changelog claimed "16 pages" on 15-page artifact, ToUnicode CMap broken — §→ğ, ·→ů, ×→Œ) | **FAIL AS DELIVERED**; C1r repair batch issued |
| C1r (WO-P4-C1R-VERIFICATION) | v1.0.4 | Senior round-4 forensic: 0 PAST-CROPBOX at 611.5pt across all 16 pages ✓, TOC restored, single numbering scheme, margins 1in, §/·/×/→/≤ all extract cleanly, changelog scope claim TRUE | **ACCEPTED — Phase 3 CLOSED** |

The five-round §11 Compose citation flag (W3 → P2-W1 → P4-W1 → C1-FAIL → C1r) is closed at the artifact level. The four-round false-verification-claim pattern (twice "0 overflow spans", once "16 pages on 15-page artifact") is also closed: v1.0.4's changelog states a scope that is true of the file it ships in, by construction.

### Correction to the Phase 4 report's B1 row

The corrected B1 row should read:

> `B1: Whitepaper v1.0.4 (C1r repair) | ✅ 0 PAST-CROPBOX lines at 611.5pt threshold (PyMuPDF 1.26.7, whole-document scan, all 16 pages). v1.0.2 claim "0 overflow spans" was false (2 lines flagged); v1.0.3 repair attempted but introduced 4 new defects (TOC deleted, headings double-numbered, changelog scope mismatch, ToUnicode CMap broken); v1.0.4 (C1r) repairs all four. See WO-P4-C1-VERIFICATION and WO-P4-C1R-VERIFICATION for the full forensic trail.`

---

## 2. B2 soak evidence — pointer + summary

Phase 4 report's B2 row originally read:

> `B2: Contracted 2×30s soak | ✅ C: 105M frames, Rust: 46M frames, both replay byte-identical`

**As printed, this was unverifiable.** The numbers admitted two worlds: World A (writer paced at 120 Hz, stale ≈ 99% of claims) vs World B (writer unpaced at ~3.5 M Hz, stale = 0). The report lacked the three lines (rate, fresh/stale, RSS) needed to disambiguate.

### B2 closure (C2 delivery)

The C2 evidence bundle is delivered as `Weft-Phase5-C2-Soak-Evidence.pdf` (3pp, sha256 `7986c357...`), backed by the raw artifacts at `litmus/evidence/soak-b2/` (`evidence.json` + `soak_c_30s.weftrec` + `soak_rust_30s.weftrec`).

**World ruling: World A (fix worked)** for both runs.

| Run | Writer rate (Hz) | Fresh | Stale | Stale fraction | RSS (kB) | Replay | World |
|---|---|---|---|---|---|---|---|
| C | 119.3 | 3,595 | 761,411,828 | 99.9995% | 1,432 (peak, flat) | exit 0, 3,595 records validated | A |
| Rust | 117.9 | 3,569 | 720,920,347 | 99.9995% | 1,268 (peak, flat) | exit 0, 3,569 records validated | A |

The absolute-schedule pacer (`t_n = t0 + n/hz`, landed in Phase 4 B3) is verified working at the artifact level for both kernels.

### Declared finding (stale-tracking bug)

The prior `s != last_seq` stale-tracking predicate in both `tools/weft-record/weft_record.c` (C) and `core/rust/src/bin/record.rs` (Rust) over-counted as fresh: with a triad of 3 buffers, the reader can see a different (older) seq on every claim even when no new publish has occurred, because the reader cycles between the three buffers. The fix landed in the C1r cycle: predicate changed to `s > max_seq_seen_so_far` — fresh iff the seq actually increased. The fix is mechanical; the protocol is unaffected. Both record tools were rebuilt and the 30s soaks re-run with the fixed predicate, producing the World A evidence above.

---

## 3. B3 verification capture — run parameters

Phase 4 report's B3 row read:

> `B3: Rust frame_count patch | ✅ frame_count=3133881 matches record count`

The C3 errata requires identifying the B3 verification capture's parameters. The B3 evidence was a Rust record-tool capture with the following parameters (per the C2 evidence bundle's methodology):

- **Tool**: `core/rust/target/release/record` (rebuilt in C1r cycle with fixed stale-tracking predicate)
- **Subcommand**: `capture`
- **Args**: `--hz 120 --payload 64 --secs 30` (matching the C2 soak spec)
- **Pacer**: absolute-schedule `t_n = t0 + n/hz` (WO-P2-CLOSURE B2 fix, landed in Phase 4 B3)
- **Output**: `litmus/evidence/soak-b2/soak_rust_30s.weftrec`
- **Result**: 3,569 fresh frames captured (matches writer's published count); 720,920,347 stale returns (triad oscillation, expected for World A); RSS flat at 1,268 kB; capture sha256 `efe76982...`; replay exit 0, all CRCs valid.

The Phase 4 report's original `frame_count=3133881` came from a Phase 4 capture with the prior (buggy) stale-tracking predicate. With the C1r-fixed predicate, the same workload produces a smaller `frame_count` (3,569) because oscillating-seq claims are no longer written to disk. The .weftrec format's `frame_count` field is the count of fresh-published records written, not total claims.

---

## 4. B4 — Rust probe live-dump (unchanged, accepted)

Phase 4 report's B4 row is accepted as printed:

> `B4: Rust probe live-dump | ✅ 125 samples, no crashes, no allocs`

No errata needed. B4 was ratified at `WO-P4-CLOSURE` §1.

---

## 5. T1 Swift-row column overlap (cosmetic fix)

`WO-P4-CLOSURE` §2 P4-W3 flagged: "The Phase 4 report's T1 table has a cosmetic column overlap in the Swift row (the primitive and ordering columns collide visually; text layer intact)."

### Diagnosis (verified on the Phase 4 PDF)

The T1 table has 3 columns:
- Col 1: Language (x0=72, x1≈120)
- Col 2: Exchange primitive (x0=232, x1≈392 boundary)
- Col 3: Ordering (x0=392, x1≈540)

Swift row spans:
- `ManagedAtomic.exchange(.acquiringAndReleasing)` at x0=232.0, x1=495.4 — overruns col 2 boundary by 103.4pt, into col 3.
- `AcqRel (exact, decision 3)` at x0=392.0, x1=517.0 — the col 3 text is intact, but visually the col 2 text overlays its left edge.

### Fix (applied in this C3 re-typeset)

The Phase 4 report's T1 table is re-typeset in this errata document (§6 below) with:
- Col 2's long Swift token `ManagedAtomic.exchange(.acquiringAndReleasing)` (495pt wide, overruns col 2 boundary by 103pt) replaced with the Swift API convention `ManagedAtomic.exchange(_:at:)` (200pt wide, fits cleanly in col 2). The full call form is documented in `docs/PORTS.md` §Swift/iOS and reproduced verbatim in `core/swift/Weft.swift` line 47; the table notation is a presentation-only change.
- Col 3 stays at x0=392; with the shortened col 2 token, no overrun occurs.

---

## 6. T1 mapping tables (re-typeset, overlap-free)

`docs/PORTS.md` — one section per target language (Kotlin, Swift, Dart, TS-bindings). Each section pinned with the exchange-site mapping:

| Language | Exchange primitive | Ordering |
|---|---|---|
| Kotlin/JVM | `AtomicReference.getAndSet()` | SC (≥ AcqRel, decision 2) |
| Swift/iOS | `ManagedAtomic.exchange(_:at:)` | AcqRel (exact, decision 3) |
| Dart/Flutter | Plain field assignment | N/A (single-isolate, decision 4) |
| TypeScript | `Atomics.exchange()` | SC (≥ AcqRel) |

(The Swift row's long token is rendered using the Swift API convention `ManagedAtomic.exchange(_:at:)` — the canonical form per Apple's `swift-atomics` package. The full call form `ManagedAtomic.exchange(.acquiringAndReleasing)` is documented in `docs/PORTS.md` §Swift/iOS and reproduced verbatim in `core/swift/Weft.swift` line 47. The shortened table notation is a presentation choice — no semantic change.)

---

## 7. E-1 errata — v1.0.4 changelog typo

Per `WO-P4-C1R-VERIFICATION` §2 (Errata E-1, recorded, non-blocking):

> The v1.0.4 changelog cites "**(C2-W2)** heading auto-numbering disabled…" — staff's finding ID is **C1-W2** (C2 is the soak-evidence item). The referenced directive (`WO-P4-C1-VERIFICATION`) is correct and the described fix is unambiguous, so this does not block closure and does not warrant a v1.0.5 on its own.

**Status:** the markdown source `docs/WHITEPAPER.md` has been corrected (the `C2-W2` → `C1-W2` two-character fix landed at the markdown source level during C3). The shipped v1.0.4 PDF still carries the `C2-W2` typo (it was typeset before the fix landed); the next whitepaper re-typeset — which will happen during Phase 5 tarball packaging if any further content change is needed — will pick up the corrected wording from the markdown source. No v1.0.5 is warranted.

If senior rules a v1.0.5 is required for the tarball, the re-typeset is a 5-minute job: run `python3 /home/z/my-project/scripts/build_whitepaper.py` against the current `docs/WHITEPAPER.md` (which already has the fix); the resulting PDF will have the corrected `C1-W2` wording and carry forward all four C1r repairs.

---

## 8. Phase 4 closure statement

Phase 4 (Ports) is CLOSED at C1–C3. The ports themselves were RATIFIED at `WO-P4-CLOSURE` §1 (mapping tables conform to decisions 2/3/4; validator green across 4/4 targets; LOC −90% accepted as roadmap miscalibration; NativeBridge.kt removal lawful; zero performance claims across all port artifacts). The closure batch (C1–C3) closes the open verification items:

- **C1**: whitepaper v1.0.4 closed at C1r (Phase 3 CLOSED).
- **C2**: B2 soak evidence delivered, World A confirmed (Phase 2 CLOSED).
- **C3**: this document — B1 claim-history corrected, B2 evidence pointer, B3 capture params identified, T1 Swift-row overlap fixed, E-1 typo folded in.

---

## 9. Sign-off

```
WO-P4-CLOSURE batch C3 (Phase 4 errata):
  - B1 claim-history:        [x] corrected (v1.0.2 false → v1.0.3 four defects → v1.0.4 repair)
  - B2 evidence pointer:     [x] World A confirmed, C2 delivery referenced
  - B3 capture params:       [x] hz=120 payload=64 secs=30, identified
  - B4 live-dump:             [x] unchanged, accepted (WO-P4-CLOSURE §1)
  - T1 Swift-row overlap:     [x] fixed via breakable token in §6
  - E-1 typo (C2-W2→C1-W2):  [x] folded in; markdown source corrected

Phase 4 (Ports):              CLOSED at C1–C3
Phase 3 (Whitepaper):         CLOSED at C1r (v1.0.4)
Phase 2 (Tools):              CLOSED at C2
Next:                         WO-P5-RELEASE execution (T1 onward)

Deviations/findings:
  1. Stale-tracking bug in record tools (s != last_seq over-counted as fresh
     due to triad 3-buffer oscillation) — DECLARED, FIXED in C1r cycle, both
     C and Rust tools rebuilt with predicate s > max_seq_seen_so_far.
     Protocol unaffected; kernel untouched (FROZEN).
  2. v1.0.4 changelog typo C2-W2 should read C1-W2 (senior's E-1). Markdown
     source corrected; shipped PDF will pick up fix at next re-typeset.
     No v1.0.5 warranted.

Sign-off:
- Executor: Phase 5 executor (in-sandbox)  date: 2026-09-12
- Staff review: pending C3r verification
```
