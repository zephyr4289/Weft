"""E2E integration (Python leg): read the C harness's kernel-shaped buffer.

The C binary is built by tools/weftc/tests/run_integration.sh; this suite
skips itself gracefully when the harness has not been built.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
GOLDEN = REPO / "tools" / "weftc" / "tests" / "golden"

sys.path.insert(0, str(REPO / "tools" / "weftc" / "codegen" / "python" / "golden"))

import weft_envelope as env_mod  # noqa: E402
import telemetry_frame as tel_mod  # noqa: E402

WeftEnvelope = env_mod.WeftEnvelope
TelemetryFrame = tel_mod.TelemetryFrame

EMIT_BIN = Path(os.environ.get("WEFTC_EMIT_BIN", "/tmp/weftc_emit"))
WEFT_C = REPO / "core" / "c" / "weft.c"
CAN_BUILD = WEFT_C.exists()
SKIP = (not EMIT_BIN.exists()) and (not CAN_BUILD)


def produce_kernel_buffer() -> bytes:
    bin_path = EMIT_BIN
    if not bin_path.exists():
        bin_path = Path(tempfile.mkdtemp(prefix="weftc-")) / "weftc_emit"
        subprocess.run(
            ["cc", "-std=c11", "-I", str(REPO / "core" / "c"),
             "-o", str(bin_path),
             str(REPO / "tools" / "weftc" / "tests" / "harness" / "emit_frame_stack.c"),
             str(WEFT_C)],
            check=True,
        )
    out = Path(tempfile.mkdtemp(prefix="weftc-")) / "kernel_stack.bin"
    subprocess.run([str(bin_path), str(out)], check=True)
    return out.read_bytes()


@unittest.skipIf(SKIP, "C harness binary not built (run tools/weftc/tests/run_integration.sh)")
class KernelBufferRoundTrip(unittest.TestCase):
    def test_kernel_buffer_reads_back_through_python_views(self):
        stack = produce_kernel_buffer()
        self.assertEqual(len(stack), 88)

        env = WeftEnvelope().bind(stack)
        self.assertTrue(env.validate_header(len(stack)), "kernel envelope must validate")
        self.assertEqual(env.version, 1)
        self.assertEqual(env.header_size, 16)
        self.assertEqual(env.seq, 0xBEEF)
        self.assertEqual(env.payload_len, 64)

        frame = TelemetryFrame().bind(stack, env.header_size)
        self.assertTrue(frame.validate_header())
        self.assertEqual(frame.schema_id, 0x8F4C1120A9B30012)
        self.assertEqual(frame.timestamp_ns, 1737504000123456789)
        self.assertEqual(frame.get_velocity_all(), (1.5, -2.25, 4000.125))
        self.assertEqual(frame.pressure_pa, 1013.25)
        self.assertEqual(frame.state, 7)
        self.assertEqual(frame.get_gps_coordinates_all(), (37.5, -122.25))
        self.assertEqual(frame.get_payload_hash_at(0), 0xDE)
        self.assertEqual(frame.get_payload_hash_at(7), 0xBE)

        # kernel canary rule: u64 LE == seq at buf_end-8
        import struct
        canary = struct.unpack_from("<Q", stack, 80)[0]
        self.assertEqual(canary, 0xBEEF)

    def test_golden_stack_byte_identical_to_kernel_emitter(self):
        stack = produce_kernel_buffer()
        golden = (GOLDEN / "weft_frame_stack.bin").read_bytes()
        self.assertEqual(stack, golden,
                         "C harness and Python golden generator must agree byte-for-byte")


if __name__ == "__main__":
    unittest.main(verbosity=2)
