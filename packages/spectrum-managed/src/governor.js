// src/governor.js — Reactive Cadence Governor + Tier staging (Pillar 5).
//
// Implements §4 of docs/spectrum/SPECTRUM-WIRE-V1.md — the NORMATIVE integer
// state machine, mirrored verbatim in Dart, Swift and Python. Any drift from
// tests/spectrum/managed/fixtures/governor_vector.json fails CI (parity law).
//
// Law 1: cadenceTick() / tierTick() read and write plain integer fields of
// caller-owned flyweights. Zero object literals, zero arrays, zero strings,
// zero closures on the tick path.

import {
  VIS_VISIBLE, THERMAL_SEVERE, THERMAL_MODERATE, THERMAL_LIGHT,
  CHARGING_NO, E_HEAP_PRESSURE, E_TIER_EXHAUSTED,
} from './wire.js';

// §4.1 frozen constants
export const CADENCE_LADDER = [240, 120, 60, 30]; // Hz rungs, 0 = profile max
export const SUSTAINED_TICKS = 10;   // ~1 s at 10 Hz telemetry poll
export const RECOVERY_TICKS = 50;    // ~5 s cool before one up-step
export const BACKGROUND_CAP = 30;    // Hz while visibility == HIDDEN
export const LOW_BATTERY_PERMILLE = 150;
export const MAX_TIER_STAGES = 2;

// Frozen tier -> max Hz map (mirrors tier_vector.json)
export const TIER_MAX_HZ = { 1: 240, 2: 120, 3: 60 };

export function createCadenceState() {
  return {
    rung: 0,          // index into CADENCE_LADDER
    tierStage: 0,     // heap-pressure down-tier stage (rule 5)
    severeStreak: 0,  // consecutive ticks at thermal >= SEVERE
    moderateStreak: 0,
    coolStreak: 0,    // consecutive cool ticks (thermal <= LIGHT, visible)
    capHz: 240,       // last emitted cap (output field)
    effTier: 1,       // effective tier after staging (output field)
    budgetBytes: 0,   // effective memory budget after staging (output field)
  };
}

// One telemetry tick. `inp` is a preallocated input flyweight:
//   { thermalState, batteryPermille, batteryCharging, visibility,
//     heapPressure (0/1), tierMaxHz (max Hz for the CURRENT effective tier,
//     i.e. TIER_MAX_HZ[effectiveTier]) }
// Mutates `st` in place; returns st.capHz for convenience (a primitive, not
// a new object). Semantics are the §4.2 table — first match wins.
export function cadenceTick(st, inp) {
  // Rule 5: heap pressure -> tier staging (independent of the ladder rules)
  if (inp.heapPressure === 1) {
    if (st.tierStage < MAX_TIER_STAGES) {
      st.tierStage++;
    } // else: E_TIER_EXHAUSTED territory — hold (surfaced via tierStage clamp)
  }
  st.effTier = Math.min(1 + st.tierStage, 3);

  const visible = inp.visibility === VIS_VISIBLE;
  if (!visible) {
    // Rule 1: background — cap frozen at BACKGROUND_CAP, streaks reset
    // (crisp semantics: no ladder movement, counters never accumulate hidden)
    st.severeStreak = 0; st.moderateStreak = 0; st.coolStreak = 0;
    st.capHz = BACKGROUND_CAP;
    return st.capHz;
  }

  // Streak accounting (mutually exclusive by thermal band)
  if (inp.thermalState >= THERMAL_SEVERE) {
    st.severeStreak++; st.moderateStreak = 0; st.coolStreak = 0;
  } else if (inp.thermalState === THERMAL_MODERATE) {
    st.moderateStreak++; st.severeStreak = 0; st.coolStreak = 0;
  } else if (inp.thermalState <= THERMAL_LIGHT) {
    st.coolStreak++; st.severeStreak = 0; st.moderateStreak = 0;
  } else {
    st.severeStreak = 0; st.moderateStreak = 0; st.coolStreak = 0;
  }

  // Rules 2/3: sustained heat steps the ladder DOWN (ladder-bounded only;
  // tier correctness comes from the ceiling clamp below)
  if (st.severeStreak >= SUSTAINED_TICKS) {
    if (st.rung < CADENCE_LADDER.length - 1) st.rung++;
    st.severeStreak = 0;
  }
  if (st.moderateStreak >= 2 * SUSTAINED_TICKS) {
    if (st.rung < CADENCE_LADDER.length - 1) st.rung++;
    st.moderateStreak = 0;
  }
  // Rule 6: sustained cool steps the ladder UP (never above rung 0)
  if (st.coolStreak >= RECOVERY_TICKS) {
    if (st.rung > 0) st.rung--;
    st.coolStreak = 0;
  }

  // Ladder cap, clamped so a down-tiered device never runs above its
  // effective tier's profile ceiling (the rung is throttle DEPTH; the
  // ceiling is the tier's own maximum).
  let cap = CADENCE_LADDER[st.rung];
  const ceiling = tierCeilingHz(st.effTier, inp);
  if (cap > ceiling) cap = ceiling;

  // Rule 4: low battery forces <= 60 Hz (does not move the ladder)
  if (inp.batteryPermille <= LOW_BATTERY_PERMILLE && inp.batteryCharging === CHARGING_NO) {
    if (cap > 60) cap = 60;
  }

  st.capHz = cap;
  return st.capHz;
}

function tierCeilingHz(effTier, inp) {
  if (inp && inp.tierMaxHz) return inp.tierMaxHz;
  return TIER_MAX_HZ[effTier] || 60;
}

// Rule 5 helper: effective memory budget halves per down-tier stage from the
// profile budget. Pure integer math; callers pass the SNAPSHOT budget.
export function effectiveBudgetBytes(profileBudget, tierStage) {
  let b = profileBudget;
  for (let i = 0; i < tierStage; i++) b = Math.floor(b / 2);
  return b;
}

// Convenience tick used by probes/tests: applies rule 5 bookkeeping including
// budget halving onto the state flyweight. Returns an int code (0 ok,
// E_TIER_EXHAUSTED when already at MAX_TIER_STAGES and pressure persists).
export function tierTick(st, inp) {
  if (inp.heapPressure === 1) {
    if (st.tierStage < MAX_TIER_STAGES) st.tierStage++;
    else return E_TIER_EXHAUSTED;
  }
  st.effTier = Math.min(1 + st.tierStage, 3);
  st.budgetBytes = effectiveBudgetBytes(inp.profileBudgetBytes || 0, st.tierStage);
  return inp.heapPressure === 1 ? E_HEAP_PRESSURE : 0;
}
