// SpectrumGovernor.swift — Reactive Cadence Governor (Swift lane).
//
// Mirrors governor.js / governor.py / spectrum_governor.dart EXACTLY — the
// frozen 130-tick vector + tier vector are the cross-language contract.
//
// Law 1: CadenceState is a final class of mutable Int fields; tick() mutates
// in place with pure integer arithmetic. No arrays, strings, or closures on
// the tick path.

import Foundation

// §4.1 frozen constants
public let cadenceLadder: [Int] = [240, 120, 60, 30] // Hz rungs, 0 = max
public let sustainedTicks: Int = 10
public let recoveryTicks: Int = 50
public let backgroundCap: Int = 30
public let lowBatteryPermille: Int = 150
public let maxTierStages: Int = 2
public let tierMaxHz: [UInt32: Int] = [tierFlagship: 240, tierMid: 120, tierBudget: 60]

public final class CadenceState {
    public var rung: Int = 0
    public var tierStage: Int = 0
    public var severeStreak: Int = 0
    public var moderateStreak: Int = 0
    public var coolStreak: Int = 0
    public var capHz: Int = 240
    public var effTier: UInt32 = tierFlagship
    public var budgetBytes: Int = 0

    public init() {}
}

public final class GovernorInput {
    public var thermalState: UInt32 = thermalNominal
    public var batteryPermille: Int = 900
    public var batteryCharging: UInt32 = chargingYes
    public var visibility: UInt32 = visVisible
    public var heapPressure: Int = 0
    public var tierMaxHzCap: Int = 240 // max Hz for the CURRENT effective tier
    public var profileBudgetBytes: Int = 0

    public init() {}
}

func ceilingHz(_ effTier: UInt32, _ capOverride: Int) -> Int {
    capOverride != 0 ? capOverride : (tierMaxHz[effTier] ?? 60)
}

/// One telemetry tick. Mutates `st` in place; returns st.capHz (an Int).
@discardableResult
public func cadenceTick(_ st: CadenceState, _ inp: GovernorInput) -> Int {
    // Rule 5: heap pressure -> tier staging (independent of ladder rules)
    if inp.heapPressure == 1 && st.tierStage < maxTierStages {
        st.tierStage += 1
    }
    st.effTier = min(UInt32(1 + st.tierStage), tierBudget)

    if inp.visibility != visVisible {
        // Rule 1: background — cap frozen, streaks reset (no ladder movement)
        st.severeStreak = 0
        st.moderateStreak = 0
        st.coolStreak = 0
        st.capHz = backgroundCap
        return st.capHz
    }

    if inp.thermalState >= thermalSevere {
        st.severeStreak += 1
        st.moderateStreak = 0
        st.coolStreak = 0
    } else if inp.thermalState == thermalModerate {
        st.moderateStreak += 1
        st.severeStreak = 0
        st.coolStreak = 0
    } else if inp.thermalState <= thermalLight {
        st.coolStreak += 1
        st.severeStreak = 0
        st.moderateStreak = 0
    } else {
        st.severeStreak = 0
        st.moderateStreak = 0
        st.coolStreak = 0
    }

    // Rules 2/3: sustained heat steps DOWN (ladder-bounded)
    if st.severeStreak >= sustainedTicks {
        if st.rung < cadenceLadder.count - 1 { st.rung += 1 }
        st.severeStreak = 0
    }
    if st.moderateStreak >= 2 * sustainedTicks {
        if st.rung < cadenceLadder.count - 1 { st.rung += 1 }
        st.moderateStreak = 0
    }
    // Rule 6: sustained cool steps UP (never above rung 0)
    if st.coolStreak >= recoveryTicks {
        if st.rung > 0 { st.rung -= 1 }
        st.coolStreak = 0
    }

    var cap = cadenceLadder[st.rung]
    let ceiling = ceilingHz(st.effTier, inp.tierMaxHzCap)
    if cap > ceiling { cap = ceiling }

    // Rule 4: low battery forces <= 60 Hz (ladder untouched)
    if inp.batteryPermille <= lowBatteryPermille && inp.batteryCharging == chargingNo && cap > 60 {
        cap = 60
    }
    st.capHz = cap
    return st.capHz
}

public func effectiveBudgetBytes(_ profileBudget: Int, _ tierStage: Int) -> Int {
    var b = profileBudget
    for _ in 0..<tierStage { b = b >> 1 }
    return b
}

/// Rule 5 bookkeeping incl. budget halving. Returns an Int32 code.
@discardableResult
public func tierTick(_ st: CadenceState, _ inp: GovernorInput) -> Int32 {
    if inp.heapPressure == 1 {
        if st.tierStage < maxTierStages {
            st.tierStage += 1
        } else {
            return eTierExhausted
        }
    }
    st.effTier = min(UInt32(1 + st.tierStage), tierBudget)
    st.budgetBytes = effectiveBudgetBytes(inp.profileBudgetBytes, st.tierStage)
    return inp.heapPressure == 1 ? eHeapPressure : 0
}
