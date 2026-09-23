"""test_fixture.py — byte-exact validation of the committed golden fixtures
against docs/weft-tensor/LAYOUT-V1.md §7. THE SAME bytes the TypeScript
suite asserts (cross-language parity anchor, Law 2)."""
from pathlib import Path

import numpy as np
import pytest

from weft_tensor import WeftRing

FIXTURES = Path(__file__).resolve().parents[2] / "fixtures" / "weft-tensor"


def load(name):
    data = (FIXTURES / name).read_bytes()
    return WeftRing.attach(data)


def test_f32_fixture_header_and_payload_parity():
    ring = load("ring-v1-f32.bin")
    assert ring.producer_seq == 10
    assert ring.slot_count == 4 and ring.layout.slot_stride == 128
    assert ring.payload_cap == 64 and ring.layout.tick_hz == 120
    # 10 frames through 4 slots: only seq 7..10 survive the wrap.
    for seq in range(1, 7):
        assert ring.acquire_frame(seq) is None, f"seq {seq} lapped"
    for seq in range(7, 11):
        v = ring.acquire_frame(seq)
        assert v is not None
        assert v.seq == seq
        assert v.timestamp_ns == seq * 1_000_000
        assert v.duration_us == 8333
        assert v.fourcc == "F32 "
        arr = v.as_numpy()  # zero-copy view
        want = np.array([(seq * 10 + i) * 0.25 for i in range(6)],
                        dtype=np.float32).reshape(2, 3)
        assert np.array_equal(arr, want)


def test_u8_fixture_header_and_payload_parity():
    ring = load("ring-v1-u8.bin")
    assert ring.producer_seq == 6
    L = ring.layout
    assert (L.dtype_code, L.dtype_bits) == (1, 8)  # kDLUInt/8
    assert L.shape[:3] == (2, 2, 4, 0)[:3]
    for seq in range(1, 3):
        assert ring.acquire_frame(seq) is None
    for seq in range(3, 7):
        v = ring.acquire_frame(seq)
        assert v is not None and v.fourcc == "RAW "
        raw = bytes(v.memory[:v.payload_len])
        assert raw == bytes((seq * 37 + i * 11) & 0xFF for i in range(16))


def test_fixture_files_are_deterministic():
    """Generator double-run parity is CI-enforced; here we pin the byte sizes."""
    assert (FIXTURES / "ring-v1-f32.bin").stat().st_size == 640
    assert (FIXTURES / "ring-v1-u8.bin").stat().st_size == 640
