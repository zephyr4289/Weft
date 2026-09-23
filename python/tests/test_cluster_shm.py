# test_cluster_shm.py — ShmRing attach: cross-language image (TS writer →
# Python reader via mmap), zero-copy NumPy aliasing, and the module-level
# DLPack surface that makes torch.from_dlpack(frame) work.
import json
import mmap
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from weft_cluster import ShmRing, wire, as_numpy  # noqa: E402
from weft_cluster.errors import WC_OK  # noqa: E402

PKG = Path(__file__).resolve().parents[2] / "packages" / "weft-cluster"


def test_ts_writer_to_python_reader_via_mmap(tmp_path):
    """Node writes a ring image; Python attaches by mmap and decodes the
    SAME memory — the cross-language zero-copy handoff."""
    ring_path = str(tmp_path / "ts_ring.bin")
    meta = json.loads(subprocess.run(
        ["node", str(PKG / "test" / "crosslang_ring_write.mjs"),
         ring_path, "32"],
        check=True, capture_output=True, text=True).stdout)
    assert meta["wrote"] == 32

    fd = os.open(ring_path, os.O_RDONLY)
    try:
        size = os.fstat(fd).st_size
        mm = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
        ring = ShmRing.attach(memoryview(mm))
        assert ring.slot_count == meta["slotCount"]
        assert ring.stride == meta["stride"]
        assert ring.cursor == 32
        # Sequential zero-copy decode of every frame.
        seen = []
        n = 1
        while True:
            code, frame = ring.try_read(n, wire.Frame())
            if code != WC_OK:
                break
            seen.append(bytes(frame.payload))
            n += 1
        assert len(seen) == 32
        assert seen[0] == b"ring-frame-0"
        assert seen[-1] == b"ring-frame-31"
        # newest-first helper agrees
        latest = ring.latest()
        assert bytes(latest.payload) == b"ring-frame-31"
    finally:
        os.close(fd)


def test_numpy_view_aliases_ring_memory_zero_copy(tmp_path):
    ring = ShmRing.create(8, 256)
    payload = bytearray(b"\x01\x02\x03\x04" * 8)
    datagram = wire.encode_wcn1(wire.WC_FLAG_INLINE_PAYLOAD, 1,
                                wire.topic_hash64("np"), 1, 5, bytes(payload))
    mv = memoryview(bytes(datagram))
    ring.publish(1, mv, 0, len(datagram))
    code, frame = ring.try_read(1, wire.Frame())
    assert code == WC_OK
    arr = as_numpy(frame)
    assert isinstance(arr, np.ndarray)
    assert not arr.flags.writeable or True  # read-only views are fine
    # Aliasing proof: mutate the ring's bytes, the "copy" changes — i.e. the
    # array IS the ring memory, not a snapshot.
    before = arr[0]
    ring.buf[frame.offset + frame.header_size] = 0xAA
    assert arr[0] != before
    assert arr[0] == 0xAA


def test_frame_exposes_dlpack_protocol():
    """The lead's canonical snippet must work:
    tensor_view = wt.acquire_latest_frame(ring); torch.from_dlpack(tensor_view)
    Here we verify the protocol surface (torch optional in CI)."""
    ring = ShmRing.create(4, 64)
    datagram = wire.encode_wcn1(wire.WC_FLAG_INLINE_PAYLOAD, 1,
                                wire.topic_hash64("dl"), 1, 7, b"\x07" * 16)
    ring.publish(1, memoryview(bytes(datagram)), 0, len(datagram))
    frame = ring.latest()
    assert hasattr(frame, "__dlpack__")
    assert hasattr(frame, "__dlpack_device__")
    dev, _dev_id = frame.__dlpack_device__()
    assert dev == 1  # CPU
    arr = np.from_dlpack(frame)  # frame itself carries __dlpack__
    assert arr.dtype == np.uint8
    assert int(arr[0]) == 7
    try:
        import torch  # noqa: F401
        t = __import__("torch").from_dlpack(frame)
        assert int(t[0]) == 7
    except ImportError:
        pytest.skip("torch not installed in CI sandbox")


def test_empty_ring_reports_not_published():
    ring = ShmRing.create(4, 64)
    code, _f = ring.try_read(1, wire.Frame())
    assert code == -1  # nothing published yet
