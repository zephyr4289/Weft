"""test_ring.py — seqlock semantics on the Python side: commit/acquire,
torn reads, overrun, exact-frame access, wait, read-only attach. Law 1/2/4."""
import struct
import time

import numpy as np
import pytest

from weft_tensor import WeftRing, LayoutError, DLPACK_FLOAT, DLPACK_UINT, SLOT_FLAG_COMMITTED


def u8_ring(slot_count=2, payload_cap=4):
    return WeftRing.create(slot_count=slot_count, payload_cap=payload_cap,
                           dtype_code=DLPACK_UINT, dtype_bits=8, shape=[payload_cap])


def test_commit_and_acquire_latest_exact_values():
    ring = WeftRing.create(slot_count=4, payload_cap=24, dtype_code=DLPACK_FLOAT,
                           dtype_bits=32, shape=[2, 3])
    assert ring.producer_seq == 0
    assert ring.acquire_latest() is None
    src = np.array([1.5, -2.25, 0.125, 64.0, 0.5, 3.0], dtype=np.float32)
    seq = ring.commit(src.tobytes(), ts=1_000_000)
    assert seq == 1
    v = ring.acquire_latest()
    assert v.seq == 1 and v.timestamp_ns == 1_000_000
    assert v.shape == (2, 3) and v.strides == (3, 1)
    arr = v.as_numpy()
    assert arr.dtype == np.float32
    assert np.array_equal(arr, src.reshape(2, 3))
    # exact scalars through the ndarray view = IEEE 754 parity (Law 2)
    assert arr[0, 0] == np.float32(1.5)
    # acquiring again returns the same latest frame
    assert ring.acquire_latest().seq == 1


def test_acquire_watermark_and_exact_frame():
    ring = u8_ring(slot_count=4, payload_cap=4)
    for i in range(1, 4):
        ring.commit(bytes([i, i, i, i]))
    assert ring.acquire_latest(3) is None
    assert ring.acquire_latest(2).seq == 3
    assert ring.acquire_frame(2).payload_len == 4
    assert ring.acquire_frame(4) is None  # future
    assert ring.acquire_frame(0) is None


def test_overrun_reports_and_drops_lapped_frames():
    ring = u8_ring(slot_count=2, payload_cap=4)
    for i in range(1, 6):
        ring.commit(bytes([i, i, i, i]))
    assert ring.producer_seq == 5
    assert ring.acquire_frame(1) is None, "seq 1 lapped"
    assert ring.acquire_frame(4).memory[0] == 4
    latest = ring.acquire_latest()
    assert latest.seq == 5 and latest.memory[0] == 5
    assert ring.stats["overruns"] >= 1


def test_torn_read_invisible_then_visible():
    ring = u8_ring(slot_count=2, payload_cap=4)
    ring.commit(b"\x09\x09\x09\x09")
    mv = ring._mv
    base = ring.layout.header_size + ring.layout.slot_stride  # slot 1
    # producer mid-write: magic + fields, COMMITTED bit CLEAR, then publish
    mv[base:base + 4] = b"WFRM"
    struct.pack_into("<I", mv, base + 4, 4)
    struct.pack_into("<Q", mv, base + 8, 2)
    struct.pack_into("<Q", mv, base + 16, 77)
    struct.pack_into("<I", mv, base + 28, 0)  # torn marker
    struct.pack_into("<I", mv, 96, 2)         # publish (lo word)
    ring.stats["torn_reads"] = 0
    assert ring.acquire_latest() is None, "uncommitted slot must be invisible"
    assert ring.stats["torn_reads"] >= 1
    mv[base + 64:base + 68] = b"\x07\x07\x07\x07"
    struct.pack_into("<I", mv, base + 28, SLOT_FLAG_COMMITTED)
    v = ring.acquire_latest()
    assert v is not None and v.seq == 2 and v.timestamp_ns == 77


def test_wait_for_new_frame_and_timeout():
    ring = u8_ring()
    ring.commit(b"\x01\x01\x01\x01")
    # already ahead: immediate
    assert ring.wait_for_new_frame(0, timeout_s=0.1) == 1
    t0 = time.monotonic()
    assert ring.wait_for_new_frame(1, timeout_s=0.05) is None
    assert time.monotonic() - t0 >= 0.04


def test_readonly_attach_rejects_commit_but_reads():
    ring = u8_ring()
    ring.commit(b"\x01\x02\x03\x04")
    ro = WeftRing.attach(bytes(ring._mv))  # bytes -> read-only
    v = ro.acquire_latest()
    assert v.seq == 1
    with pytest.raises(LayoutError) as e:
        ro.commit(b"\x05\x05\x05\x05")
    assert e.value.code == "WTR1_READONLY"
    arr = v.as_numpy()
    assert arr.flags.writeable is False


def test_create_input_validation():
    with pytest.raises(LayoutError) as e:
        WeftRing.create(slot_count=1, payload_cap=4, shape=[1])
    assert e.value.code == "WTR1_BAD_SLOT_COUNT"
    with pytest.raises(LayoutError) as e:
        WeftRing.create(payload_cap=0, shape=[1])
    assert e.value.code == "WTR1_BAD_PAYLOAD_CAP"
    with pytest.raises(LayoutError) as e:
        WeftRing.create(payload_cap=4, shape=[])
    assert e.value.code == "WTR1_BAD_RANK"
    with pytest.raises(LayoutError) as e:
        WeftRing.create(payload_cap=4, shape=[1, 2, 3])
    assert e.value.code == "WTR1_TOO_SMALL"
    with pytest.raises(LayoutError) as e:
        WeftRing.create(payload_cap=4, shape=[1]).commit(b"x" * 999)
    assert e.value.code == "WTR1_COMMIT_RANGE"


def test_commit_range_and_mmap_attach(tmp_path):
    ring = u8_ring()
    with pytest.raises(LayoutError) as e:
        ring.commit(b"x" * (ring.payload_cap + 1))
    assert e.value.code == "WTR1_COMMIT_RANGE"
    # full attach of a produced ring file via mmap (consumer process story)
    path = tmp_path / "ring.bin"
    for i in range(1, 4):
        ring.commit(bytes([i] * 4))
    path.write_bytes(ring._mv)
    import mmap
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        attached = WeftRing.attach(mm)
        assert attached.producer_seq == 3
        assert attached.acquire_frame(2).memory[0] == 2
        del attached, mm  # drop exported views BEFORE closing the mapping
