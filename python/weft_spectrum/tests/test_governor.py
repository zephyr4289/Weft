# tests/test_governor.py — frozen 130-tick vector + tier vector (Python lane).
import json
import unittest
from pathlib import Path

from weft_spectrum.wire import E_TIER_EXHAUSTED, E_HEAP_PRESSURE
from weft_spectrum.governor import (
    CADENCE_LADDER, SUSTAINED_TICKS, RECOVERY_TICKS, BACKGROUND_CAP,
    LOW_BATTERY_PERMILLE, MAX_TIER_STAGES, CadenceState, GovernorInput,
    cadence_tick, tier_tick, effective_budget_bytes,
)

ROOT = Path(__file__).resolve().parents[3]
FIX = ROOT / "tests" / "spectrum" / "managed" / "fixtures"
VEC = json.loads((FIX / "governor_vector.json").read_text())
TIER_VEC = json.loads((FIX / "tier_vector.json").read_text())


class TestConstants(unittest.TestCase):
    def test_normative_table(self):
        self.assertEqual(list(CADENCE_LADDER), VEC["constants"]["CADENCE_LADDER"])
        self.assertEqual(SUSTAINED_TICKS, VEC["constants"]["SUSTAINED_TICKS"])
        self.assertEqual(RECOVERY_TICKS, VEC["constants"]["RECOVERY_TICKS"])
        self.assertEqual(BACKGROUND_CAP, VEC["constants"]["BACKGROUND_CAP"])
        self.assertEqual(LOW_BATTERY_PERMILLE, VEC["constants"]["LOW_BATTERY_PERMILLE"])
        self.assertEqual(MAX_TIER_STAGES, VEC["constants"]["MAX_TIER_STAGES"])


class TestFrozenVector(unittest.TestCase):
    def test_130_tick_identical_sequence(self):
        st = CadenceState()
        inp = GovernorInput()
        inp.tierMaxHz = VEC["profileMaxHz"]
        for i, t in enumerate(VEC["ticks"]):
            inp.thermalState = t["thermal"]
            inp.batteryPermille = t["batteryPermille"]
            inp.batteryCharging = t["batteryCharging"]
            inp.visibility = t["visibility"]
            inp.heapPressure = t["heapPressure"]
            cadence_tick(st, inp)
            e = VEC["expected"][i]
            self.assertEqual(st.capHz, e["cap"], f"tick {i} cap")
            self.assertEqual(st.rung, e["rung"], f"tick {i} rung")
            self.assertEqual(st.tierStage, e["tierStage"], f"tick {i} tierStage")
        transitions = [VEC["expected"][0]["cap"]]
        for i in range(1, len(VEC["expected"])):
            if VEC["expected"][i]["cap"] != VEC["expected"][i - 1]["cap"]:
                transitions.append(VEC["expected"][i]["cap"])
        self.assertEqual(transitions, VEC["expectedCapTransitions"])


class TestEdgeMatrix(unittest.TestCase):
    def test_bottom_rung_clamp(self):
        st = CadenceState()
        inp = GovernorInput(thermalState=3, batteryPermille=900,
                            batteryCharging=1, visibility=0, heapPressure=0)
        for _ in range(200):
            cadence_tick(st, inp)
        self.assertEqual(st.capHz, 30)
        self.assertEqual(st.rung, len(CADENCE_LADDER) - 1)

    def test_recovery_capped_at_rung0(self):
        st = CadenceState()
        hot = GovernorInput(thermalState=3, batteryPermille=900,
                            batteryCharging=1, visibility=0, heapPressure=0)
        cool = GovernorInput(thermalState=0, batteryPermille=900,
                             batteryCharging=1, visibility=0, heapPressure=0)
        for _ in range(100):
            cadence_tick(st, hot)
        for _ in range(RECOVERY_TICKS * 3 + 100):
            cadence_tick(st, cool)
        self.assertEqual(st.capHz, 240)

    def test_hidden_freezes_streaks(self):
        st = CadenceState()
        hidden = GovernorInput(thermalState=3, batteryPermille=100,
                               batteryCharging=0, visibility=1, heapPressure=0)
        for _ in range(50):
            cadence_tick(st, hidden)
        self.assertEqual(st.capHz, BACKGROUND_CAP)
        self.assertEqual(st.rung, 0)

    def test_tier_ceiling(self):
        st = CadenceState()
        inp = GovernorInput(thermalState=0, batteryPermille=900,
                            batteryCharging=1, visibility=0, heapPressure=0)
        inp.tierMaxHz = 60
        cadence_tick(st, inp)
        self.assertEqual(st.capHz, 60)


class TestTierStaging(unittest.TestCase):
    def test_vector(self):
        st = CadenceState()
        inp = GovernorInput(profileBudgetBytes=8589934592)
        for i, t in enumerate(TIER_VEC["ticks"]):
            inp.heapPressure = t["heapPressure"]
            prev = st.tierStage
            code = tier_tick(st, inp)
            e = TIER_VEC["expected"][i]
            self.assertEqual(st.tierStage, e["tierStage"], f"tick {i}")
            self.assertEqual(st.effTier, e["effectiveTier"], f"tick {i}")
            self.assertEqual(st.budgetBytes, e["budgetBytes"], f"tick {i}")
            if t["heapPressure"] == 1:
                self.assertEqual(
                    code,
                    E_TIER_EXHAUSTED if prev >= MAX_TIER_STAGES else E_HEAP_PRESSURE,
                    f"tick {i} code")
            else:
                self.assertEqual(code, 0, f"tick {i} idle")

    def test_budget_halving(self):
        self.assertEqual(effective_budget_bytes(1024, 0), 1024)
        self.assertEqual(effective_budget_bytes(1024, 2), 256)
        self.assertEqual(effective_budget_bytes(1023, 1), 511)


if __name__ == "__main__":
    unittest.main()
