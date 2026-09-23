# tests/test_hud.py — WeftSpectrumHud terminal lane: paint + fail-safe.
import unittest

from weft_spectrum.wire import ProfileFlyweight, E_HUD_CONTEXT_LOST, E_HUD_RECOVERED
from weft_spectrum.governor import CadenceState
from weft_spectrum.hud import SpectrumHudTerminal


class FakeMetrics:
    __slots__ = ("fps", "jitterP99Us", "jitterP999Us", "memBytes")

    def __init__(self):
        self.fps = 240
        self.jitterP99Us = 23
        self.jitterP999Us = 41
        self.memBytes = 96 * 1048576


class TestTerminalHud(unittest.TestCase):
    def test_paint_rows_from_flyweights(self):
        st = ProfileFlyweight()
        st.siliconTier = 1
        st.thermalState = 0
        st.featureFlagsLo = (1 << 7) | (1 << 0)  # NEON + WASM_SIMD128
        st.batteryPermille = 870
        st.visibility = 0
        gov = CadenceState()
        gov.capHz = 240
        hud = SpectrumHudTerminal()
        code = hud.paint(st, gov, FakeMetrics())
        self.assertEqual(code, 0)
        self.assertEqual(hud.rows["TIER"], "T1 FLAGSHIP")
        self.assertIn("NEON", hud.rows["FEATURES"])
        self.assertEqual(hud.rows["FPS"], "240")
        self.assertEqual(hud.rows["JITTER p99/p99.9"], "23/41us")
        self.assertEqual(hud.rows["THERMAL"], "nominal")
        self.assertEqual(hud.rows["CADENCE"], "240 Hz")
        self.assertIn("87%", hud.rows["STATUS"])

    def test_fail_safe_never_raises(self):
        hud = SpectrumHudTerminal()
        boom = type("Hostile", (), {
            "siliconTier": property(lambda self: (_ for _ in ()).throw(RuntimeError("device gone"))),
        })()
        code = hud.paint(boom, None, None)
        self.assertEqual(code, E_HUD_CONTEXT_LOST)
        self.assertFalse(hud.healthy)
        self.assertIn("FALLBACK", hud.banner)

    def test_recovery_clears_banner(self):
        hud = SpectrumHudTerminal()
        hud.paint(None, None, None)  # None state -> exception path? (attribute error)
        if not hud.healthy:
            st = ProfileFlyweight()
            st.siliconTier = 2
            gov = CadenceState()
            gov.capHz = 120
            code = hud.paint(st, gov, None)
            self.assertEqual(code, E_HUD_RECOVERED)
            self.assertTrue(hud.healthy)
            self.assertEqual(hud.banner, "")


if __name__ == "__main__":
    unittest.main()
