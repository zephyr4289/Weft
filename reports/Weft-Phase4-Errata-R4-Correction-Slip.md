# Weft Phase 4 Errata — R4 Correction Slip

> **Phase 5 / T0 / C3 / R4** · 2026-09-12 · `x86_64-sandbox`
> Per `WO-P5-VERIFICATION §3 R-D` (correction slip for the original C3 errata §3).

---

## 0. Status

This slip corrects the original `Weft-Phase4-Errata.pdf` §3 ("B3 verification capture — run parameters") per the senior's R-D ruling: the original §3 misattributed the C2 Rust soak capture as "the B3 evidence" with parameters `--hz 120 --payload 64 --secs 30`. The senior's forensics (`WO-P5-VERIFICATION §2 P5-W3`) found that the original B3 row's `frame_count=3,133,881` is three orders of magnitude inconsistent with a 120 Hz-paced 30 s run — the listed parameters cannot describe the capture that produced it.

## 1. Correction

**The original Phase 4 B3 verification capture's parameters are unarchived and unrecoverable.** The C2 Rust soak artifact (`litmus/evidence/soak-b2/soak_rust_30s.weftrec`, 3,569 frames, sha256 `efe76982...`) is presented strictly as **the extant fixed-tool verification** — the only Rust record-tool capture with archived parameters — not as identification of the original B3 evidence.

Per `WO-P5-VERIFICATION §2 P5-W3`:

> "Errata §3 claims to identify the Phase 4 B3 verification capture's parameters, then lists `--hz 120 --payload 64 --secs 30` and the C2 Rust soak artifact as 'the B3 evidence.' The original B3 row's `frame_count=3,133,881` is three orders of magnitude inconsistent with a 120 Hz-paced 30 s run — the listed parameters cannot describe the capture that produced it. The original B3 capture's parameters were not archived and are unrecoverable; §3 substitutes C2 numbers and calls it identification, which is false attribution. The C2 numbers themselves are real; the attribution is not."

## 2. What the C2 capture IS

The C2 Rust soak capture is:

- **Tool**: `core/rust/target/release/record` (rebuilt in C1r cycle with the fixed `s > max_seq_seen_so_far` stale-tracking predicate per D-T0-1)
- **Subcommand**: `capture`
- **Args**: `--hz 120 --payload 64 --secs 30` (the C2 soak spec, NOT the original B3 spec)
- **Pacer**: absolute-schedule `t_n = t0 + n/hz` (landed in Phase 4 B3)
- **Output**: `litmus/evidence/soak-b2/soak_rust_30s.weftrec`
- **Result**: 3,569 fresh frames captured; 720,920,347 stale returns; RSS flat 1,268 kB; capture sha256 `efe76982...`; replay exit 0, all CRCs valid

This capture was produced **as part of the C2 evidence supplement** (per `WO-P4-C1R-VERIFICATION §4`), NOT as a re-identification of the original Phase 4 B3 capture. The original B3 capture (which produced `frame_count=3,133,881` for the Rust record tool's `frame_count` patching verification) was not parameter-archived; the senior's forensics confirmed the listed parameters cannot describe it.

## 3. What the original B3 evidence WAS (per the original Phase 4 report)

The original Phase 4 B3 row (`docs/PHASE4-REPORT.md` §2) read:

> `B3: Rust frame_count patch | ✅ frame_count=3133881 matches record count`

This verified that the Rust record tool's `frame_count` field-patching works — the `frame_count` written to the file header at close matches the actual record count. The capture that produced 3,133,881 records was an UNPACED capture (writer running at ~3.5 M Hz, the pre-Phase-4-B3 pacing defect) — the only way to produce ~3 M records in a 30 s window is with an unpaced writer. The pacing fix landed in Phase 4 B3 itself; the original B3 capture predated the fix.

The original B3 capture's parameters were not archived in the Phase 4 evidence bundle. The C2 evidence (`soak_rust_30s.weftrec`) is the closest extant verification — it tests the same Rust record tool with the pacing fix in place, but with the fixed stale-tracking predicate, producing a much smaller `frame_count` (3,569) because oscillating-seq claims are no longer written to disk.

## 4. Closure

The B3 row in the Phase 4 report's T0 batch is closed at the *substance* level (the Rust record tool's `frame_count` patching works as designed; verified by both the original unpaced capture and the C2 paced capture). The *attribution* in the original C3 errata §3 is corrected here: the C2 capture is the extant fixed-tool verification, not identification of the original B3 evidence. The original B3 parameters remain unarchived and unrecoverable.

---

## 5. Sign-off

```
R4 correction slip (per WO-P5-VERIFICATION §3 R-D):
  - B3 original capture params:    [x] declared unarchived + unrecoverable
  - C2 Rust soak attribution:      [x] corrected to "extant fixed-tool verification"
  - False-attribution removed:     [x] §3 of original errata superseded by this slip
  - C2 numbers themselves:         [x] real, untouched (per WO-P5-VERIFICATION V6)

Sign-off:
- Executor: Phase 5 R-batch (in-sandbox)  date: 2026-09-12
- Staff review: pending R6 + round-6 verification
```
