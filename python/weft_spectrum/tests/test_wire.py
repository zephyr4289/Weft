# tests/test_wire.py — SHP1 golden decode parity + Law 4 matrix (Python lane).
import json
import unittest
from pathlib import Path

from weft_spectrum import wire
from weft_spectrum.wire import (
    RECORD_SIZE, CRC_OFFSET, ERROR_NAMES, ProfileView, ProfileFlyweight,
    decode_profile, crc32_ref, E_BAD_MAGIC, E_BAD_VERSION, E_BAD_SIZE,
    E_CRC_MISMATCH, E_RESERVED_DIRTY, FEAT,
    TIER_FLAGSHIP, TIER_MID, TIER_BUDGET,
)

ROOT = Path(__file__).resolve().parents[3]
FIX = ROOT / "tests" / "spectrum" / "managed" / "fixtures"
EXPECTED = json.loads((FIX / "expected_profile.json").read_text())

NUMERIC_FIELDS = [
    "layoutVersion", "recordSize", "featureFlagsLo", "featureFlagsHi",
    "siliconTier", "thermalState", "perfCores", "effCores", "gpuFamily",
    "cacheLineBytes", "cpuMaxClockKhz", "memoryTotalBytes",
    "memoryBudgetBytes", "simdWidthBits", "frameBudgetUs",
    "maxFrameRateMilliHz", "batteryPermille", "batteryCharging",
    "visibility", "dmaLaneCount", "vendorId", "deviceId", "crc32",
]
# crc32 lives on the VIEW (integrity metadata), not the flyweight (mirrors TS)
FLYWEIGHT_FIELDS = [f for f in NUMERIC_FIELDS if f != "crc32"]


class TestGoldenParity(unittest.TestCase):
    def _decode(self, name):
        buf = (FIX / name).read_bytes()
        code, fw, view = decode_profile(buf)
        return code, fw, view, buf

    def test_flagship_byte_parity(self):
        code, fw, view, buf = self._decode("hw-profile-flagship.bin")
        self.assertEqual(code, 0)
        exp = EXPECTED["flagship"]
        for f in FLYWEIGHT_FIELDS:
            self.assertEqual(getattr(fw, f), exp[f], f"field {f}")
        self.assertEqual(view.crc32, exp["crc32"], "view crc32")
        for name in exp["features"]:
            self.assertTrue(view.has_feature_bit(FEAT[name]), name)
        self.assertEqual(fw.siliconTier, TIER_FLAGSHIP)

    def test_mid_byte_parity(self):
        code, fw, _, _ = self._decode("hw-profile-mid.bin")
        self.assertEqual(code, 0)
        exp = EXPECTED["mid"]
        for f in FLYWEIGHT_FIELDS:
            self.assertEqual(getattr(fw, f), exp[f], f"field {f}")
        self.assertEqual(fw.siliconTier, TIER_MID)

    def test_budget_byte_parity(self):
        code, fw, _, _ = self._decode("hw-profile-budget.bin")
        self.assertEqual(code, 0)
        exp = EXPECTED["budget"]
        for f in FLYWEIGHT_FIELDS:
            self.assertEqual(getattr(fw, f), exp[f], f"field {f}")
        self.assertEqual(fw.siliconTier, TIER_BUDGET)

    def test_torn_fails_closed(self):
        buf = (FIX / "hw-profile-torn.bin").read_bytes()
        code, _, _ = decode_profile(buf)
        self.assertEqual(code, E_CRC_MISMATCH)
        self.assertEqual(EXPECTED["torn"]["error"], "E_CRC_MISMATCH")

    def test_zero_copy_identity(self):
        buf = bytearray((FIX / "hw-profile-budget.bin").read_bytes())
        view = ProfileView(buf)
        self.assertEqual(view.validate(), 0)
        before = view.thermalState
        buf[20] = (before + 1) & 0xFF
        self.assertEqual(view.thermalState, (before + 1) & 0xFF, "no snapshot taken")


class TestLaw4Matrix(unittest.TestCase):
    def setUp(self):
        self.good = bytearray((FIX / "hw-profile-flagship.bin").read_bytes())

    def _validate(self, b):
        return ProfileView(bytes(b)).validate()

    def test_bad_magic(self):
        b = bytearray(self.good); b[0] = 0x58
        self.assertEqual(self._validate(b), E_BAD_MAGIC)

    def test_bad_version(self):
        b = bytearray(self.good); b[4] = 9
        self.assertEqual(self._validate(b), E_BAD_VERSION)

    def test_bad_size_field(self):
        b = bytearray(self.good); b[6] = 128
        self.assertEqual(self._validate(b), E_BAD_SIZE)

    def test_truncated(self):
        self.assertEqual(self._validate(self.good[:-1]), E_BAD_SIZE)

    def test_reserved_dirty(self):
        b = bytearray(self.good); b[120] = 1
        self.assertEqual(self._validate(b), E_RESERVED_DIRTY)

    def test_payload_mutation_caught_by_crc(self):
        b = bytearray(self.good); b[12] = 1
        self.assertEqual(self._validate(b), E_CRC_MISMATCH)


class TestCrcReference(unittest.TestCase):
    def test_vectors(self):
        self.assertEqual(crc32_ref(b""), 0)
        self.assertEqual(crc32_ref(b"123456789"), 0xCBF43926)

    def test_taxonomy_complete(self):
        self.assertEqual(len(ERROR_NAMES), 15)
        self.assertEqual(ERROR_NAMES[4], "E_CRC_MISMATCH")
        self.assertEqual(ERROR_NAMES[15], "E_HUD_RECOVERED")


class TestFlyweightReuse(unittest.TestCase):
    def test_same_object_mutated_in_place(self):
        fw = ProfileFlyweight()
        buf = (FIX / "hw-profile-flagship.bin").read_bytes()
        code, fw, view = decode_profile(buf, dst=fw)
        self.assertIs(code, 0)
        buf2 = (FIX / "hw-profile-budget.bin").read_bytes()
        code2, fw2, _ = decode_profile(buf2, dst=fw)
        self.assertIs(fw2, fw, "decode into the SAME flyweight")
        self.assertEqual(fw.siliconTier, TIER_BUDGET)


if __name__ == "__main__":
    unittest.main()
