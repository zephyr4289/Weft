# tests/test_alloc.py — Law 1 heap probe (Python lane): 1,000,000 continuous
# metric cycles with tracemalloc retained-growth gate + biting negative control.
import gc
import tracemalloc
import unittest

from weft_spectrum.wire import ProfileView, ProfileFlyweight
from weft_spectrum.governor import CadenceState, GovernorInput, cadence_tick

GATE_BYTES = 64 * 1024
CYCLES = 1_000_000
WARMUP = 50_000


class TestMillionCycleTelemetry(unittest.TestCase):
    def test_one_million_cycles_retained_growth_under_gate(self):
        profile = bytes(range(192))
        # synthesize a VALID flagship record is overkill here: reuse golden via
        # parity suite; for the probe any 192B buffer works because validate()
        # only needs the bytes (E codes are part of the cycle cost either way).
        view = ProfileView(profile)
        fw = ProfileFlyweight()
        gov = CadenceState()
        inp = GovernorInput()

        def cycle(i):
            code = view.validate()  # integrity gate (E_* codes are data here)
            inp.thermalState = i % 5
            fw.thermalState = inp.thermalState
            fw.batteryPermille = 1000 - (i % 1000)
            fw.visibility = 0 if (i % 600) < 540 else 1
            inp.batteryPermille = fw.batteryPermille
            inp.visibility = fw.visibility
            cadence_tick(gov, inp)
            return gov.capHz + code

        sink = 0
        for i in range(WARMUP):
            sink ^= cycle(i)
        gc.collect()

        tracemalloc.start()
        for i in range(CYCLES):
            sink ^= cycle(i)
        current, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()

        self.assertLess(current, GATE_BYTES,
            f"retained growth {current} B over {CYCLES} cycles exceeds "
            f"{GATE_BYTES} B gate (Law 1)")
        self.assertTrue(sink >= 0)  # consume, defeat DCE

    def test_negative_control_bites(self):
        """A deliberately-allocating loop MUST blow the gate (probe sensitivity)."""
        profile = bytes(192)
        view = ProfileView(profile)
        tracemalloc.start()
        junk = []
        for i in range(100_000):
            view.validate()
            junk.append({"seq": i})  # retained per-cycle object
        current, _ = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        del junk
        gc.collect()
        self.assertGreater(current, GATE_BYTES,
            "negative control did NOT allocate — probe is insensitive "
            "(silent-green violation)")


if __name__ == "__main__":
    unittest.main()
