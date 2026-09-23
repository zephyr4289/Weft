// lib/src/spectrum_governor.dart — Reactive Cadence Governor (Dart lane).
//
// Mirrors packages/spectrum-managed/src/governor.js and
// python/weft_spectrum/weft_spectrum/governor.py EXACTLY. The frozen
// 130-tick vector + tier vector are the cross-language parity contract.
//
// Law 1: CadenceState/GovernorInput are preallocated mutable flyweights;
// cadenceTick() mutates them in place with pure integer arithmetic —
// zero object churn per tick.

import 'spectrum_wire.dart';

// §4.1 frozen constants
const List<int> cadenceLadder = [240, 120, 60, 30]; // Hz rungs, 0 = max
const int sustainedTicks = 10;
const int recoveryTicks = 50;
const int backgroundCap = 30;
const int lowBatteryPermille = 150;
const int maxTierStages = 2;
const Map<int, int> tierMaxHz = {1: 240, 2: 120, 3: 60};

class CadenceState {
  int rung = 0;
  int tierStage = 0;
  int severeStreak = 0;
  int moderateStreak = 0;
  int coolStreak = 0;
  int capHz = 240;
  int effTier = 1;
  int budgetBytes = 0;
}

class GovernorInput {
  int thermalState = thermalNominal;
  int batteryPermille = 900;
  int batteryCharging = chargingYes;
  int visibility = visVisible;
  int heapPressure = 0;
  int tierMaxHzCap = 240; // max Hz for the CURRENT effective tier
  int profileBudgetBytes = 0;
}

int _ceilingHz(int effTier, int capOverride) =>
    capOverride != 0 ? capOverride : (tierMaxHz[effTier] ?? 60);

/// One telemetry tick. Mutates [st] in place; returns st.capHz (an int).
int cadenceTick(CadenceState st, GovernorInput inp) {
  // Rule 5: heap pressure -> tier staging (independent of ladder rules)
  if (inp.heapPressure == 1 && st.tierStage < maxTierStages) {
    st.tierStage++;
  }
  st.effTier = (1 + st.tierStage) < 3 ? (1 + st.tierStage) : 3;

  if (inp.visibility != visVisible) {
    // Rule 1: background — cap frozen, streaks reset (no ladder movement)
    st.severeStreak = 0;
    st.moderateStreak = 0;
    st.coolStreak = 0;
    st.capHz = backgroundCap;
    return st.capHz;
  }

  if (inp.thermalState >= thermalSevere) {
    st.severeStreak++;
    st.moderateStreak = 0;
    st.coolStreak = 0;
  } else if (inp.thermalState == thermalModerate) {
    st.moderateStreak++;
    st.severeStreak = 0;
    st.coolStreak = 0;
  } else if (inp.thermalState <= thermalLight) {
    st.coolStreak++;
    st.severeStreak = 0;
    st.moderateStreak = 0;
  } else {
    st.severeStreak = 0;
    st.moderateStreak = 0;
    st.coolStreak = 0;
  }

  // Rules 2/3: sustained heat steps DOWN (ladder-bounded)
  if (st.severeStreak >= sustainedTicks) {
    if (st.rung < cadenceLadder.length - 1) st.rung++;
    st.severeStreak = 0;
  }
  if (st.moderateStreak >= 2 * sustainedTicks) {
    if (st.rung < cadenceLadder.length - 1) st.rung++;
    st.moderateStreak = 0;
  }
  // Rule 6: sustained cool steps UP (never above rung 0)
  if (st.coolStreak >= recoveryTicks) {
    if (st.rung > 0) st.rung--;
    st.coolStreak = 0;
  }

  var cap = cadenceLadder[st.rung];
  final ceiling = _ceilingHz(st.effTier, inp.tierMaxHzCap);
  if (cap > ceiling) cap = ceiling;

  // Rule 4: low battery forces <= 60 Hz (ladder untouched)
  if (inp.batteryPermille <= lowBatteryPermille &&
      inp.batteryCharging == chargingNo) {
    if (cap > 60) cap = 60;
  }
  st.capHz = cap;
  return st.capHz;
}

int effectiveBudgetBytes(int profileBudget, int tierStage) {
  var b = profileBudget;
  for (var i = 0; i < tierStage; i++) {
    b = b >> 1; // integer halving (Dart: floor for non-negative ints)
  }
  return b;
}

/// Rule 5 bookkeeping incl. budget halving. Returns an int code.
int tierTick(CadenceState st, GovernorInput inp) {
  if (inp.heapPressure == 1) {
    if (st.tierStage < maxTierStages) {
      st.tierStage++;
    } else {
      return eTierExhausted;
    }
  }
  st.effTier = (1 + st.tierStage) < 3 ? (1 + st.tierStage) : 3;
  st.budgetBytes = effectiveBudgetBytes(inp.profileBudgetBytes, st.tierStage);
  return inp.heapPressure == 1 ? eHeapPressure : 0;
}
