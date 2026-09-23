# weft_spectrum/governor.py — Reactive Cadence Governor (P5, normative §4).
#
# Mirrors packages/spectrum-managed/src/governor.js EXACTLY — the frozen
# 130-tick vector (tests/spectrum/managed/fixtures/governor_vector.json) and
# tier_vector.json are the cross-language parity contract. Any divergence
# fails tests in BOTH languages plus the CI parity stage.
#
# Law 1: CadenceState is a __slots__ flyweight; cadence_tick() mutates it in
# place with pure integer arithmetic. No dicts, lists, strings, closures.

from .wire import (
    VIS_VISIBLE, THERMAL_SEVERE, THERMAL_MODERATE, THERMAL_LIGHT,
    CHARGING_NO, E_HEAP_PRESSURE, E_TIER_EXHAUSTED,
)

CADENCE_LADDER = (240, 120, 60, 30)  # Hz rungs, index 0 = profile max
SUSTAINED_TICKS = 10
RECOVERY_TICKS = 50
BACKGROUND_CAP = 30
LOW_BATTERY_PERMILLE = 150
MAX_TIER_STAGES = 2
TIER_MAX_HZ = {1: 240, 2: 120, 3: 60}


class CadenceState:
    __slots__ = ("rung", "tierStage", "severeStreak", "moderateStreak",
                 "coolStreak", "capHz", "effTier", "budgetBytes")

    def __init__(self):
        self.rung = 0
        self.tierStage = 0
        self.severeStreak = 0
        self.moderateStreak = 0
        self.coolStreak = 0
        self.capHz = 240
        self.effTier = 1
        self.budgetBytes = 0


class GovernorInput:
    """Preallocated per-tick input flyweight (mutate fields, never recreate)."""
    __slots__ = ("thermalState", "batteryPermille", "batteryCharging",
                 "visibility", "heapPressure", "tierMaxHz",
                 "profileBudgetBytes")

    def __init__(self, **kw):
        self.thermalState = 0
        self.batteryPermille = 900
        self.batteryCharging = 1
        self.visibility = VIS_VISIBLE
        self.heapPressure = 0
        self.tierMaxHz = 240
        self.profileBudgetBytes = 0
        # init-time kw convenience (the hot path mutates fields in place)
        for k, v in kw.items():
            if k not in self.__slots__:
                raise TypeError(f"unknown input field: {k}")
            setattr(self, k, v)


def _ceiling_hz(eff_tier, tier_max_hz):
    if tier_max_hz:
        return tier_max_hz
    return TIER_MAX_HZ.get(eff_tier, 60)


def cadence_tick(st, inp):
    """One telemetry tick. Mutates `st` in place; returns st.capHz (int)."""
    # Rule 5: heap pressure -> tier staging (independent of ladder rules)
    if inp.heapPressure == 1 and st.tierStage < MAX_TIER_STAGES:
        st.tierStage += 1  # exhausted -> hold (E_TIER_EXHAUSTED via tier_tick)
    st.effTier = min(1 + st.tierStage, 3)

    if inp.visibility != VIS_VISIBLE:
        # Rule 1: background — cap frozen, streaks reset (no ladder movement)
        st.severeStreak = 0
        st.moderateStreak = 0
        st.coolStreak = 0
        st.capHz = BACKGROUND_CAP
        return st.capHz

    if inp.thermalState >= THERMAL_SEVERE:
        st.severeStreak += 1
        st.moderateStreak = 0
        st.coolStreak = 0
    elif inp.thermalState == THERMAL_MODERATE:
        st.moderateStreak += 1
        st.severeStreak = 0
        st.coolStreak = 0
    elif inp.thermalState <= THERMAL_LIGHT:
        st.coolStreak += 1
        st.severeStreak = 0
        st.moderateStreak = 0
    else:
        st.severeStreak = 0
        st.moderateStreak = 0
        st.coolStreak = 0

    # Rules 2/3: sustained heat steps DOWN (ladder-bounded)
    if st.severeStreak >= SUSTAINED_TICKS:
        if st.rung < len(CADENCE_LADDER) - 1:
            st.rung += 1
        st.severeStreak = 0
    if st.moderateStreak >= 2 * SUSTAINED_TICKS:
        if st.rung < len(CADENCE_LADDER) - 1:
            st.rung += 1
        st.moderateStreak = 0
    # Rule 6: sustained cool steps UP (never above rung 0)
    if st.coolStreak >= RECOVERY_TICKS:
        if st.rung > 0:
            st.rung -= 1
        st.coolStreak = 0

    cap = CADENCE_LADDER[st.rung]
    ceiling = _ceiling_hz(st.effTier, inp.tierMaxHz)
    if cap > ceiling:
        cap = ceiling
    # Rule 4: low battery forces <= 60 Hz (ladder untouched)
    if inp.batteryPermille <= LOW_BATTERY_PERMILLE and inp.batteryCharging == CHARGING_NO:
        if cap > 60:
            cap = 60
    st.capHz = cap
    return st.capHz


def effective_budget_bytes(profile_budget, tier_stage):
    b = profile_budget
    for _ in range(tier_stage):
        b //= 2
    return b


def tier_tick(st, inp):
    """Rule 5 bookkeeping incl. budget halving. Returns int code."""
    if inp.heapPressure == 1:
        if st.tierStage < MAX_TIER_STAGES:
            st.tierStage += 1
        else:
            return E_TIER_EXHAUSTED
    st.effTier = min(1 + st.tierStage, 3)
    st.budgetBytes = effective_budget_bytes(inp.profileBudgetBytes, st.tierStage)
    return E_HEAP_PRESSURE if inp.heapPressure == 1 else 0
