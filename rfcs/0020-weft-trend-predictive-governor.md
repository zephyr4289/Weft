---
RFC: 0020
Title: weft_trend — Predictive Lag-Trend Estimator for the Adaptive Governor
Status: Draft
Authors: Engineer 3 (Observability Fabric & Reactive Dataflow)
Created: 2026-09-20
Supersedes / Superseded-by: None
Requires: 0009 (FreshnessGovernor — the ladder this feeds), 0012 (integer-filter precedent)
---

# RFC 0020 — weft_trend: predictive lag-trend estimation

## Summary

RFC-0009's governor is REACTIVE by design: it sees `framesBehind` and acts.
By the time behind hits the Snapshot rung, the backlog already exists — the
consumer is already in trouble. This RFC adds the missing half: a tiny
integer estimator that watches the SAME staleness signal and predicts where
it is HEADING, early enough to act before the ladder's expensive rungs.

The estimator is a classic 1-D alpha-beta filter in pure fixed-point Q16
arithmetic: an EWMA `level` of the staleness and an EWMA `slope` of its
per-step change. From the two numbers it projects the staleness `HORIZON`
steps ahead and emits a CLOSED verdict set:

| verdict | meaning | recommended action |
|---|---|---|
| `STABLE` | projected level below the rising rung | none (FastPath) |
| `RISING` | projected level at/over the rising rung | proactive `Skip(pred)` |
| `FALLING` | projected level under the falling rung, slope ≤ 0 | recover to FastPath |
| `BURST` | single-step jump ≥ burst threshold | skip hard (`Skip(pred)`) |

The governor ladder stays EXACTLY as RFC-0009 declared it (closed action
set, no fifth action); the trend estimator is a driver-layer SENSOR that
feeds the consumer's choice of ladder INPUT — mechanism, not policy (Law
3): apps may ignore it entirely.

## Specification

### §1 State and arithmetic (all integer — no float anywhere)

```
level_q16 : EWMA of behind, Q16
slope_q16 : EWMA of per-step delta, Q16 (units: behind-steps per step)
last_raw  : previous raw sample (deltas)
alpha_q8, beta_q8 : smoothing gains as fractions of 256
```

Update per `observe(behind)` (first sample seeds level, slope 0):

```
raw_q16   = behind << 16
delta_raw = behind - last_raw                    (signed)
level_q16 += (alpha_q8 * (raw_q16 - level_q16)) >> 8
slope_q16 += (beta_q8 * ((delta_raw << 16) - slope_q16)) >> 8
```

Defaults: `alpha_q8 = 48` (0.1875), `beta_q8 = 96` (0.375) — a
fast-trend pair: the level stays smooth (settles a constant in ~20
samples) while the slope settles a unit ramp in ~5 steps, which is what
gives the projection its lead over the raw signal.

### §2 Projection and verdicts

```
pred_raw = saturate((level_q16 + slope_q16 * HORIZON) >> 16)   // HORIZON = 8
```

Verdict (first match wins):

1. `delta_raw >= burst_delta` (default 32) → **BURST**, `skip_n = min(pred_raw, 63)`
2. `pred_raw >= rising_behind` (default 4 — the governor's Skip rung) AND
   `slope_q16 >= 0.5` → **RISING**, `skip_n = min(pred_raw - 1, 63)` — the
   proactive skip: land the consumer at the FastPath rung BEFORE the
   backlog matures (the slope guard keeps a plateau at STABLE — the EWMA
   slope stalls at a tiny nonzero residue on constant signals)
3. `pred_raw <= falling_behind` (default 1) and `slope_q16 <= -0.5` →
   **FALLING**, skip 0
4. otherwise **STABLE**, skip 0

The predictive lead, concretely: a consumer plateauing at behind 2 then
ramping +1/step sees RISING (skip 6, projecting 7.4) at raw behind 4 —
exactly at the Skip rung, with the projection already 3.4 steps beyond
it and ~12 steps before Snapshot territory (16). The trend turns
Snapshot-class reactions into Skip-class actions (the D-series pins the
exact deterministic sequence).

`skip_n = 0` on every non-RISING/BURST verdict. All counters are advisory
telemetry (AXIOM T); the verdict stream is PROTOCOL (G5 packing
`verdict << 6 | min(skip_n, 63)` — do not renumber).

### §3 Laws

- **Law 1**: three comparisons and two multiplies per observe — no loops.
- **Law 2**: the estimator is one stack struct; observe allocates nothing.
- **Law 3**: lives in the driver layer; the governor and kernel are
  untouched; consumers opt in.
- **Law 4**: observe is a pure function of (behind, internal state); the
  seeded verdict-stream fixture byte-compares C, Rust, TS, Kotlin, Swift,
  and Dart (`fixtures/xlang-trend/`).

## Test plan

`core/c/weft_trend_test.c` (D-series): convergence to a constant, the
predictive lead property (RISING fires while the raw signal is still
below the rung), recovery FALLING, BURST emergency, hand-computed Q16
vectors, custom configuration, and a pinned xorshift verdict stream.
