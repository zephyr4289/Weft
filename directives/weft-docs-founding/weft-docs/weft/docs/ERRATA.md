# Errata — Founding Spec v0.1

> **Status:** The founding spec PDF (`Weft-Specification-v0.1.pdf`, September 2026) is **superseded** by `docs/WHITEPAPER.md` (v1.0, 2026-09-11). This document is the supersession instrument. The founding PDF itself is an external artifact — the truth now lives in the whitepaper and the living docs suite.

---

## 1. Founding spec §5 — Two-variable protocol (WITHDRAWN)

**What was wrong:** The founding spec §5 specified a two-variable design (`latest` + `claimed`) where the writer computed its target buffer from two independent Relaxed loads:
```
latest_now  = latest.load(Relaxed)
claimed_now = claimed.load(Relaxed)
next        = min({0,1,2} \ {latest_now, claimed_now})
```

The `claimed.load(Relaxed)` carried no cross-thread visibility guarantee. The writer could legally fail to observe an in-progress claim, creating a window where the writer overwrites the reader's held buffer. This was formally unsound under the C11/Rust memory model.

**How it was found:** Phase 0 adversarial litmus testing (finding F1, L4 freshness false-RED) exposed the telemetry-lag artifact. The loom v1 model independently found the same defect class: a separate telemetry store mistaken for the publish point.

**The correction:** RFC-0001 §4 specifies the corrected single-atomic-exchange design: one shared `latest`, exchanged by both writer (`AcqRel`) and reader (`AcqRel`). No second atomic. No exclusion set. No CAS retry. The founding spec's two-variable design is withdrawn per RFC-0001 §3.

**Where the truth now lives:** `rfcs/0001-triad-exchange-protocol.md` (Status: Accepted). `docs/WHITEPAPER.md` §3.

---

## 2. Founding spec §9.5 — Predicted headline numbers (REPLACED)

**What was wrong:** The founding spec §9.5 published a predicted headline chart:
```
| Impl | P50 FPS | P99 FPS | Bytes/frame | GC pauses/sec |
| A — Reactive naive | ~11 | ~6 | ~2.3 KB | ~47 |
| B — Best practice | ~116 | ~54 | ~380 B | ~3 |
| C — Weft | 120 | 120 | 0 | 0 |
| D — Hand-rolled | 120 | 120 | 0 | 0 |
```

These were **predictions, not measurements**. The spec labeled them as projections, which was honest at the time. But the predicted numbers were calibrated against the Streamify production codebase, not the sandbox litmus/bench suite. The actual measured numbers differ in detail (though the structural gates hold).

**The correction:** Phase 1's benchmark suite (B1–B5) replaced the predictions with measured numbers. The structural gates (B3 ratio < 2.0, B5 alloc == 0) pass in all three languages. The headline throughput numbers are in `docs/WHITEPAPER-TABLES.md`, machine-generated from `bench/results.json` (sha256: `16b5c663…`).

**Where the truth now lives:** `docs/WHITEPAPER.md` §6b. `docs/WHITEPAPER-TABLES.md`. `bench/results.json`.

---

## 3. Phase 1 implementation report — Withdrawn-protocol code (SUPERSEDED)

**What was wrong:** The Phase 1 implementation report (the Rust Android v0.1 code in `weft-android/`) implemented the withdrawn two-variable protocol, not the corrected single-exchange design. The code was structurally valid but used the unsound `latest` + `claimed` design.

**The correction:** Phase 0 re-implemented the kernel in C, Rust, and TypeScript using the corrected single-atomic-exchange design from RFC-0001 §4. The Phase 1 Android code is superseded by the Phase 0 kernels (`core/c/weft.c`, `core/rust/src/lib.rs`, `core/ts/weft.ts`).

**Where the truth now lives:** `core/c/weft.h` + `weft.c`. `core/rust/src/lib.rs`. `core/ts/weft.ts`. All three pass the litmus suite (24/24) and the bench suite (15/15).

---

## 4. 03-ENVELOPE §5 — Negotiation table row 3 (CORRECTED)

**What was wrong:** The §5 L8 hooks table listed row 3 as `(W=2, S={1}) → BIND_INCOMPATIBLE`. The §3 formula `max({v ∈ S : v ≤ W})` produces 1 (since 1 ≤ 2). The table contradicted itself: row 4 `(W=3, S={1,2}) → 2` requires downgrade capability that row 3 denied.

**The correction:** WO-P0A §3 ruled the §3 formula normative; row 3 corrected to `→ 1`. The §5 table was a typo, not a semantic disagreement. All three implementations verify the §3 formula.

**Where the truth now lives:** `03-ENVELOPE.md` v1.1+ (§3 formula normative). `docs/WHITEPAPER.md` §4.3.

---

## Supersession pointers

| Founding spec section | Status | Superseded by |
|---|---|---|
| §1–4 (problem statement) | Still valid | WHITEPAPER §1 (restated, expanded) |
| §5 (two-variable protocol) | **WITHDRAWN** | RFC-0001 §4; WHITEPAPER §3 |
| §6 (platform primitives) | Corrected | WHITEPAPER §8; ARCHITECTURE.md |
| §7 (lifecycle semantics) | Corrected | WHITEPAPER §5 (I6); 02-KERNEL §6 |
| §8 (API surface) | Corrected | 02-KERNEL §4; WHITEPAPER §3 |
| §9 (benchmarks) | Replaced | WHITEPAPER §6b; bench/REPORT.md |
| §9.5 (predicted numbers) | **REPLACED** | WHITEPAPER-TABLES.md (measured) |
| §10 (non-goals) | Still valid | WHITEPAPER §9 (restated) |
| §11 (open questions) | Still valid | WHITEPAPER §10 |
| §15 (references) | Updated | WHITEPAPER §11 |

The founding spec PDF is preserved as a historical artifact. It is not referenced for truth; the whitepaper and the living docs suite are the canonical sources.
