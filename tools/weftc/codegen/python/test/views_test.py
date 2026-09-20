"""weftc Python backend — full verification suite (stdlib unittest).

Gates enforced here:
  Golden parity    every field of every fixture vs expected.json (bit-exact)
  Law 1            zero-copy aliasing: buffer mutations are visible in views;
                   NumPy void views write through to writable buffers
  Law 2            static scan: every struct format string is '<'-prefixed;
                   NumPy dtypes carry explicit little-endian byte order
  Law 3            pure-stdlib struct path (numpy optional, skipped if absent)
  Law 4            validation matrix mirrors kernel weft_envelope_decode()
"""
import importlib.util
import json
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN_BIN = os.path.normpath(os.path.join(HERE, "..", "..", "..", "tests", "golden"))
PY_GOLDEN = os.path.join(HERE, "..", "golden")

sys.path.insert(0, PY_GOLDEN)

import weft_envelope as we_env_mod  # noqa: E402
import telemetry_frame as tel_mod  # noqa: E402
import imu_sample as imu_mod  # noqa: E402

WeftEnvelope = we_env_mod.WeftEnvelope
TelemetryFrame = tel_mod.TelemetryFrame
ImuSample = imu_mod.ImuSample

with open(os.path.join(GOLDEN_BIN, "expected.json"), encoding="utf-8") as f:
    EXPECTED = json.load(f)

def _bin(name):
    with open(os.path.join(GOLDEN_BIN, f"{name}.bin"), "rb") as f:
        return f.read()

try:
    import numpy as _np
except ImportError:
    _np = None


class GoldenParity(unittest.TestCase):
    def test_telemetry_every_field(self):
        v = TelemetryFrame().bind(_bin("telemetry_frame"))
        e = EXPECTED["telemetry_frame"]
        self.assertTrue(v.validate_header())
        self.assertEqual(v.schema_id, int(e["schemaId"], 16))
        self.assertEqual(v.timestamp_ns, int(e["timestampNs"], 16))
        ts = int(e["timestampNs"], 16)
        self.assertEqual(v.timestamp_ns_lo, ts & 0xFFFFFFFF)
        self.assertEqual(v.timestamp_ns_hi, ts >> 32)
        self.assertEqual(v.get_velocity_all(), tuple(e["velocity"]))
        self.assertEqual(v.pressure_pa, e["pressurePa"])
        self.assertEqual(v.state, e["state"])
        self.assertEqual(v.get_gps_coordinates_all(), tuple(e["gpsCoordinates"]))
        self.assertEqual([v.get_payload_hash_at(i) for i in range(8)], e["payloadHash"])

    def test_envelope_every_field(self):
        v = WeftEnvelope().bind(_bin("weft_envelope"))
        e = EXPECTED["weft_envelope"]
        self.assertEqual([v.get_magic_at(i) for i in range(4)], [ord(c) for c in e["magic"]])
        self.assertEqual(v.version, e["version"])
        self.assertEqual(v.header_size, e["headerSize"])
        self.assertEqual(v.seq, e["seq"])
        self.assertEqual(v.payload_len, e["payloadLen"])

    def test_imu_every_field(self):
        v = ImuSample().bind(_bin("imu_sample"))
        e = EXPECTED["imu_sample"]
        self.assertEqual(v.timestamp_ns, int(e["timestampNs"], 16))
        self.assertEqual(v.get_accel_all(), tuple(e["accel"]))
        self.assertEqual(v.get_gyro_all(), tuple(e["gyro"]))

    def test_u64_exactness_beyond_double_mantissa(self):
        # 1737504000123456789 > 2**53: JSON numbers would lose this.
        v = TelemetryFrame().bind(_bin("telemetry_frame"))
        self.assertEqual(v.timestamp_ns, 1737504000123456789)
        # the double mantissa CANNOT distinguish the two adjacent u64 values:
        self.assertEqual(float(v.timestamp_ns), float(1737504000123456768))


class Law4ValidationMatrix(unittest.TestCase):
    def test_envelope_kernel_decision_table(self):
        stack = _bin("weft_frame_stack")
        ok = WeftEnvelope().bind(stack)
        self.assertTrue(ok.validate_header(len(stack)))

        bare = WeftEnvelope().bind(_bin("weft_envelope"))
        self.assertFalse(bare.validate_header(16))   # no payload bytes
        self.assertFalse(bare.validate_header(79))   # one byte short
        self.assertTrue(bare.validate_header(80))    # exactly enough

        bad_magic = bytearray(stack)
        bad_magic[1] = 0x00
        self.assertFalse(WeftEnvelope().bind(bytes(bad_magic)).validate_header(88))

        bad_ver = bytearray(stack)
        bad_ver[4] = 9
        self.assertFalse(WeftEnvelope().bind(bytes(bad_ver)).validate_header(88))

        bad_hs = bytearray(stack)
        bad_hs[6] = 8
        self.assertFalse(WeftEnvelope().bind(bytes(bad_hs)).validate_header(88))

        big_hs = bytearray(stack)
        big_hs[6] = 200
        self.assertFalse(WeftEnvelope().bind(bytes(big_hs)).validate_header(88))

        big_pl = bytearray(stack)
        big_pl[12] = 200
        self.assertFalse(WeftEnvelope().bind(bytes(big_pl)).validate_header(88))

        self.assertFalse(WeftEnvelope().validate_header(88))  # unbound

    def test_telemetry_schema_id_handshake(self):
        bad = bytearray(_bin("telemetry_frame"))
        bad[0] ^= 0xFF
        self.assertFalse(TelemetryFrame().bind(bytes(bad)).validate_header())

    def test_imu_bounds_only(self):
        self.assertTrue(ImuSample().bind(_bin("imu_sample")).validate_header())
        self.assertFalse(ImuSample().validate_header())  # unbound


class VerticalSlice(unittest.TestCase):
    def test_stack_envelope_then_payload(self):
        stack = _bin("weft_frame_stack")
        env = WeftEnvelope().bind(stack)
        self.assertTrue(env.validate_header(len(stack)))
        frame = TelemetryFrame().bind(stack, env.header_size)
        self.assertTrue(frame.validate_header())
        self.assertEqual(frame.timestamp_ns, 1737504000123456789)
        self.assertEqual(frame.pressure_pa, EXPECTED["telemetry_frame"]["pressurePa"])


class Law1ZeroCopy(unittest.TestCase):
    def test_mutation_visibility(self):
        ba = bytearray(_bin("telemetry_frame"))
        v = TelemetryFrame().bind(ba)
        self.assertEqual(v.pressure_pa, 1013.25)
        ba[28:32] = struct.pack("<f", 55.5)
        self.assertEqual(v.pressure_pa, 55.5)  # no copy: buffer change visible

    def test_write_path_and_chain(self):
        ba = bytearray(_bin("telemetry_frame"))
        v = TelemetryFrame().bind(ba)
        ret = (
            v.with_timestamp_ns(0x0123456789ABCDEF)
            .with_velocity_at(2, -0.75)
            .with_pressure_pa(998.5)
            .with_state(5)
            .with_gps_coordinates_at(1, 12.5)
            .with_payload_hash_at(7, 0xA5)
            .with_timestamp_ns_lo(0x89ABCDEF)
            .with_timestamp_ns_hi(0x01234567)
        )
        self.assertIs(ret, v)
        self.assertEqual(v.timestamp_ns, 0x0123456789ABCDEF)
        self.assertEqual(v.get_velocity_at(2), -0.75)
        self.assertEqual(v.pressure_pa, 998.5)
        self.assertEqual(v.state, 5)
        self.assertEqual(v.get_gps_coordinates_at(1), 12.5)
        self.assertEqual(v.get_payload_hash_at(7), 0xA5)

    def test_readonly_bind_getters_only(self):
        v = TelemetryFrame().bind(_bin("telemetry_frame"))
        with self.assertRaises(TypeError):
            v.pressure_pa = 1.0

    def test_flyweight_rebind(self):
        v = TelemetryFrame()
        a = v.bind(_bin("telemetry_frame"))
        self.assertEqual(a.timestamp_ns, 1737504000123456789)
        b = v.bind(_bin("weft_frame_stack"), 16)
        self.assertIs(a, b)
        self.assertEqual(b.timestamp_ns, 1737504000123456789)

    def test_bind_out_of_bounds(self):
        with self.assertRaises(ValueError):
            TelemetryFrame().bind(b"\x00" * 8)
        with self.assertRaises(ValueError):
            ImuSample().bind(b"\x00" * 32, 1)
        with self.assertRaises(ValueError):
            ImuSample().bind(b"\x00" * 32, -1)


@unittest.skipIf(_np is None, "numpy not installed")
class Law1NumpyZeroCopy(unittest.TestCase):
    def test_dtype_layout_exactness(self):
        for cls, nbytes in ((TelemetryFrame, 64), (WeftEnvelope, 16), (ImuSample, 32)):
            dt = cls.dtype()
            self.assertEqual(dt.itemsize, nbytes)
            # explicit little-endian formats
            for name in dt.names:
                fmt = dt.fields[name][0]
                base = fmt.base if fmt.shape else fmt
                if base.kind == "f" or (base.kind in "iu" and base.itemsize > 1):
                    # numpy canonicalizes '<' to '=' on little-endian hosts;
                    # the SOURCE string is '<' (static scan proves it), and
                    # the host must be little-endian for the wire to match.
                    self.assertIn(base.byteorder, ("<", "="), name)
                    self.assertEqual(sys.byteorder, "little")

    def test_dtype_offsets_match_ir(self):
        dt = TelemetryFrame.dtype()
        for name, off in (
            ("schema_id", 0), ("timestamp_ns", 8), ("velocity", 16),
            ("pressure_pa", 28), ("state", 32), ("reserved", 33),
            ("gps_coordinates", 40), ("payload_hash", 56),
        ):
            self.assertEqual(dt.fields[name][1], off, name)

    def test_aliasing_both_directions(self):
        ba = bytearray(_bin("telemetry_frame"))
        v = TelemetryFrame().bind(ba)
        n = v.as_numpy()
        self.assertEqual(n["pressure_pa"], 1013.25)
        ba[28:32] = struct.pack("<f", 55.5)
        self.assertEqual(n["pressure_pa"], 55.5)  # buffer -> view
        n["velocity"][1] = -99.5
        self.assertEqual(v.get_velocity_at(1), -99.5)  # view -> buffer

    def test_gap_integrity(self):
        # the 4-byte gap at 36..40 must stay untouched by the numpy view
        dt = TelemetryFrame.dtype()
        self.assertEqual(dt.itemsize, 64)
        self.assertEqual(dt.fields["gps_coordinates"][1], 40)
        self.assertEqual(dt.fields["reserved"][1] + dt.fields["reserved"][0].itemsize, 36)


class Law2LittleEndian(unittest.TestCase):
    def test_all_formats_explicit_le(self):
        import re
        for mod in (we_env_mod, tel_mod, imu_mod):
            src = open(mod.__file__, encoding="utf-8").read()
            formats = re.findall(r'struct\.Struct\("([^"]+)"\)', src)
            self.assertTrue(formats)
            for fmt in formats:
                self.assertTrue(fmt.startswith("<"), f"non-LE format {fmt} in {mod.__name__}")

    def test_golden_bytes_are_le(self):
        # velocity[0] = 1.5 -> LE f32 bytes 00 00 C0 3F
        self.assertEqual(_bin("telemetry_frame")[16:20], b"\x00\x00\xc0\x3f")
        # timestamp u64 LE
        self.assertEqual(_bin("telemetry_frame")[8:16], struct.pack("<Q", 1737504000123456789))


class Law3Runtime(unittest.TestCase):
    def test_pure_stdlib_imports(self):
        import weft_envelope
        src = open(weft_envelope.__file__, encoding="utf-8").read()
        self.assertNotIn("ctypes", src)
        self.assertNotIn("pybind11", src)
        # numpy import is guarded — module still imports without it
        self.assertTrue(hasattr(weft_envelope, "WeftEnvelope"))

    def test_memoryview_and_bytearray_and_bytes(self):
        raw = _bin("telemetry_frame")
        for buf in (raw, bytearray(raw), memoryview(raw)):
            v = TelemetryFrame().bind(buf)
            self.assertEqual(v.timestamp_ns, 1737504000123456789)


if __name__ == "__main__":
    unittest.main(verbosity=2)
