"""test_dlpack.py — DLPack capsule over ring memory: np.from_dlpack consumes
it zero-copy; torch path is skip-guarded (explicit reason, never silent).
Also: double-acquire produces fresh capsules; array interface parity."""
import gc

import numpy as np
import pytest

from weft_tensor import WeftRing, DLPACK_FLOAT, DLPACK_UINT


def f32_ring():
    return WeftRing.create(slot_count=4, payload_cap=24, dtype_code=DLPACK_FLOAT,
                           dtype_bits=32, shape=[2, 3])


def test_numpy_from_dlpack_zero_copy():
    ring = f32_ring()
    src = np.array([[1.5, -2.25, 0.125], [64.0, 0.5, 3.0]], dtype=np.float32)
    ring.commit(src.tobytes(), ts=1)
    v = ring.acquire_latest()
    assert v.__dlpack_device__() == (1, 0)
    t = np.from_dlpack(v)
    assert t.dtype == np.float32 and t.shape == (2, 3)
    assert np.array_equal(t, src)
    # zero-copy proof: the DLPack tensor points INTO the ring buffer —
    # mutating the ring mutates the numpy view (same memory).
    payload_addr_t = t.__array_interface__["data"][0]
    view_addr = v.as_numpy().__array_interface__["data"][0]
    assert payload_addr_t == view_addr, "DLPack must alias the ring slot exactly"


def test_dlpack_twice_two_capsules():
    ring = f32_ring()
    ring.commit(np.arange(6, dtype=np.float32).tobytes())
    v = ring.acquire_latest()
    t1 = np.from_dlpack(v)
    t2 = np.from_dlpack(v)
    assert np.array_equal(t1, t2)
    del t1, t2
    gc.collect()  # deleters fire through the capsule path


def test_dlpack_rejects_copy_and_foreign_device():
    ring = f32_ring()
    ring.commit(np.zeros(6, dtype=np.float32).tobytes())
    v = ring.acquire_latest()
    with pytest.raises(BufferError):
        v.__dlpack__(copy=True)
    with pytest.raises(BufferError):
        v.__dlpack__(dl_device=(3, 0))  # kDLCUDA — not ours in V1


def test_array_interface_matches_as_numpy():
    ring = f32_ring()
    ring.commit(np.ones(6, dtype=np.float32).tobytes())
    v = ring.acquire_latest()
    iface = v.__array_interface__
    assert iface["shape"] == (2, 3)
    assert iface["typestr"] == "<f4"
    assert iface["data"][0] == v.as_numpy().__array_interface__["data"][0]


def test_torch_bridge_skip_guarded():
    torch = pytest.importorskip(
        "torch", reason="torch not installed in this environment — "
        "bridge validated via np.from_dlpack (same DLPack protocol); "
        "torch lane runs where torch exists (CI gpu lane)")
    ring = f32_ring()
    src = np.array([0.5, 1.5, 2.5, 3.5, 4.5, 5.5], dtype=np.float32)
    ring.commit(src.tobytes(), ts=42)
    v = ring.acquire_latest()
    t = torch.from_dlpack(v)  # the lead's canonical zero-copy line
    assert t.dtype == torch.float32 and t.shape == (2, 3)
    assert torch.equal(t, torch.from_numpy(src.reshape(2, 3)))


def test_u8_tensor_via_dlpack():
    ring = WeftRing.create(slot_count=2, payload_cap=16, dtype_code=DLPACK_UINT,
                           dtype_bits=8, shape=[2, 2, 4])
    ring.commit(bytes(range(16)))
    v = ring.acquire_latest()
    t = np.from_dlpack(v)
    assert t.dtype == np.uint8 and t.shape == (2, 2, 4)
    assert t[1, 1, 3] == 15
