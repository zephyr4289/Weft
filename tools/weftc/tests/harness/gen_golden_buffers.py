#!/usr/bin/env python3
"""weftc golden fixture generator — deterministic little-endian binary buffers.

Writes the canonical parity buffers consumed by every managed-backend test
(TS, Python runtime tests; Swift/Dart static audits; the C verify harness):

    tools/weftc/tests/golden/weft_envelope.bin     16 B  triad-1 envelope
    tools/weftc/tests/golden/telemetry_frame.bin   64 B  payload schema
    tools/weftc/tests/golden/imu_sample.bin        32 B  header-less schema
    tools/weftc/tests/golden/weft_frame_stack.bin  88 B  envelope + payload + canary
    tools/weftc/tests/golden/expected.json         shared expectation table

Determinism: no RNG, no timestamps, no dict-order dependence (all writes are
explicit struct.pack calls). Re-running must be byte-identical — the codegen
determinism CI gate diff-checks these files.

Law 2: every pack uses an explicit little-endian format ('<').
"""
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.normpath(os.path.join(HERE, "..", "golden"))

# --- canonical values (all floats dyadic => bit-exact in f32/f64 everywhere) ---
SCHEMA_ID = 0x8F4C1120A9B30012
SEQ = 0x0000BEEF          # 48879
PAYLOAD_LEN = 64
VERSION = 1
HEADER_SIZE = 16

TS_TELEMETRY = 1737504000123456789
VELOCITY = (1.5, -2.25, 4000.125)
PRESSURE_PA = 1013.25
STATE = 7
RESERVED = (0, 0, 0)
GPS = (37.5, -122.25)
PAYLOAD_HASH = (0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE)

TS_IMU = 999999999
ACCEL = (0.25, -0.5, 9.8125)
GYRO = (0.125, -0.0625, 3.5)


def build_envelope() -> bytes:
    # Triad-1: magic "WEFT", version u16, header_size u16, seq u32, payload_len u32.
    return struct.pack("<4sHHII", b"WEFT", VERSION, HEADER_SIZE, SEQ, PAYLOAD_LEN)


def build_telemetry_frame() -> bytes:
    return b"".join([
        struct.pack("<Q", SCHEMA_ID),                # schemaId   @0
        struct.pack("<Q", TS_TELEMETRY),             # timestampNs@8
        struct.pack("<3f", *VELOCITY),               # velocity   @16
        struct.pack("<f", PRESSURE_PA),              # pressurePa @28
        struct.pack("<B", STATE),                    # state      @32
        struct.pack("<3B", *RESERVED),               # reserved   @33
        b"\x00\x00\x00\x00",                         # pad to 40 (f64 alignment)
        struct.pack("<2d", *GPS),                    # gps        @40
        struct.pack("<8B", *PAYLOAD_HASH),           # hash       @56
    ])


def build_imu_sample() -> bytes:
    return b"".join([
        struct.pack("<Q", TS_IMU),                   # timestampNs @0
        struct.pack("<3f", *ACCEL),                  # accel       @8
        struct.pack("<3f", *GYRO),                   # gyro        @20
    ])


def build_frame_stack() -> bytes:
    # Kernel buffer layout per core/c/weft.h:
    #   [0..16) envelope | [16..16+payload) payload | [end-8..end) canary u64 LE = seq
    canary = struct.pack("<Q", SEQ)
    return build_envelope() + build_telemetry_frame() + canary


def main() -> None:
    os.makedirs(GOLDEN, exist_ok=True)
    buffers = {
        "weft_envelope.bin": build_envelope(),
        "telemetry_frame.bin": build_telemetry_frame(),
        "imu_sample.bin": build_imu_sample(),
        "weft_frame_stack.bin": build_frame_stack(),
    }
    sizes = {"weft_envelope.bin": 16, "telemetry_frame.bin": 64,
             "imu_sample.bin": 32, "weft_frame_stack.bin": 88}
    for name, blob in buffers.items():
        assert len(blob) == sizes[name], f"{name}: {len(blob)} != {sizes[name]}"
        with open(os.path.join(GOLDEN, name), "wb") as f:
            f.write(blob)

    expected = {
        "_meta": {
            "note": "Single source of truth for cross-language parity tests. "
                    "Floats are dyadic => bit-exact under IEEE-754 nearest rounding.",
            "endian": "little",
        },
        "weft_envelope": {
            "magic": "WEFT",
            "version": VERSION,
            "headerSize": HEADER_SIZE,
            "seq": SEQ,
            "payloadLen": PAYLOAD_LEN,
        },
        "telemetry_frame": {
            "schemaId": "0x8f4c1120a9b30012",
            "schemaIdLower": SCHEMA_ID,
            "timestampNs": TS_TELEMETRY,
            "velocity": list(VELOCITY),
            "pressurePa": PRESSURE_PA,
            "state": STATE,
            "reserved": list(RESERVED),
            "gpsCoordinates": list(GPS),
            "payloadHash": list(PAYLOAD_HASH),
        },
        "imu_sample": {
            "timestampNs": TS_IMU,
            "accel": list(ACCEL),
            "gyro": list(GYRO),
        },
        "weft_frame_stack": {
            "canarySeq": SEQ,
            "payloadOffset": HEADER_SIZE,
        },
    }
    with open(os.path.join(GOLDEN, "expected.json"), "w", encoding="utf-8") as f:
        json.dump(expected, f, indent=2, sort_keys=True)
        f.write("\n")

    for name in buffers:
        path = os.path.join(GOLDEN, name)
        print(f"wrote {path} ({sizes[name]} bytes)")
    print("wrote", os.path.join(GOLDEN, "expected.json"))


if __name__ == "__main__":
    main()
