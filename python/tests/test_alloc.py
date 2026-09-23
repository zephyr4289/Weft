"""test_alloc.py — Law 1 evidence on the Python side: steady-state
commit/acquire/as_numpy loops must not drive GC churn or heap growth.
Methodology: warmup -> gc() -> measured loop with gc callbacks counting."""
import gc

import numpy as np
import pytest

from weft_tensor import WeftRing, DLPACK_FLOAT, DLPACK_UINT


def test_steady_state_zero_gc_pressure():
    ring = WeftRing.create(slot_count=4, payload_cap=256, dtype_code=DLPACK_FLOAT,
                           dtype_bits=32, shape=[64])
    src = np.zeros(64, dtype=np.float32)

    # warmup (JIT-free CPython, but warm caches / freelists)
    for i in range(2000):
        ring.commit(src.tobytes(), ts=i)
        v = ring.acquire_latest()
        assert v is not None

    collections = []
    def cb(phase, info):
        if phase == "start":
            collections.append(info.get("generation"))

    gc.collect()
    before = len(gc.get_objects())
    # count collections during the measured loop
    counter = {"n": 0}
    def _cb(phase, info):
        if phase == "start":
            counter["n"] += 1
    gc.callbacks.append(_cb)
    try:
        payload = src.tobytes()
        for i in range(10000):
            ring.commit(payload, ts=i)
            v = ring.acquire_latest()
            arr = v.as_numpy()
            assert float(arr.reshape(-1)[0]) >= 0.0
    finally:
        gc.callbacks.remove(_cb)
    assert counter["n"] == 0, (
        f"{counter['n']} GC collections during 10k steady-state frames — "
        "per-frame garbage leaked into the hot loop (Law 1)"
    )
    after = len(gc.get_objects())
    # Object-count growth (containers/modules aside) must stay tiny.
    assert after - before < 200, f"object population grew by {after - before}"


def test_audio_feeder_zero_gc_pressure():
    ring = WeftRing.create(slot_count=8, payload_cap=1024, dtype_code=DLPACK_FLOAT,
                           dtype_bits=32, shape=[256])
    from weft_tensor import AudioPcmFeeder
    feeder = AudioPcmFeeder(ring, channels=1, chunk_samples=256, sample_hz=48000)
    chunk = np.zeros(256, dtype=np.float32)
    for i in range(2000):
        feeder.feed(chunk)  # warmup
    counter = {"n": 0}
    def _cb(phase, info):
        if phase == "start":
            counter["n"] += 1
    gc.callbacks.append(_cb)
    try:
        for i in range(20000):
            chunk[0] = i
            feeder.feed(chunk)
            v = ring.acquire_latest()
            assert v is not None
    finally:
        gc.callbacks.remove(_cb)
    assert counter["n"] == 0, "audio feeder drives GC (Law 1 violation)"
    assert feeder.stats["slot_rolls"] >= 20000
