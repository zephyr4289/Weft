# test_alloc.py — Law 1/Law 2 heap evidence (robotics lane):
# tracemalloc gates over 200,000 zero-copy ring acquisitions and camera
# slot cycles, with a biting negative control.
#
# The ring loop emulates a REAL producer: each iteration rewrites the
# slot header for seq s (slot (s-1) % slot_count), publishes committed,
# then acquires — the consumer path under trace is exactly the live
# drop-not-block path.

import gc
import struct
import tracemalloc

from weft_robotics import (CameraSource, RingReader, FMT_POINTS_F32,
                           FMT_GRAY8)

GATE_BYTES = 64 * 1024
RECORDS = 200_000
WARMUP = 10_000
SLOT_COUNT = 64
SLOT_SIZE = 4096

RETAIN_SINK = []


def _payload(n=8):
    vals = []
    for i in range(n):
        vals += [float(i), 0.5, -0.25]
    return struct.pack(f'<{n * 3}f', *vals)


def _producer_write(mv, seq, topic, fmt, payload_len):
    """Emulate the producer writing seq into its slot, then publishing."""
    slot = (seq - 1) % SLOT_COUNT
    base = 128 + slot * SLOT_SIZE
    struct.pack_into('<QI', mv, base, seq, payload_len)
    struct.pack_into('<I', mv, base + 12, topic)
    struct.pack_into('<Q', mv, base + 16, 1000 + seq)
    struct.pack_into('<II', mv, base + 24, fmt, 0)
    struct.pack_into('<Q', mv, 24, seq)  # committed = seq


def test_ring_ingest_200k_retained_growth_under_gate(ring_factory):
    payload = _payload()
    ring = bytearray(ring_factory(
        [(1, FMT_POINTS_F32, payload, False)] * SLOT_COUNT,
        slot_size=SLOT_SIZE, slot_count=SLOT_COUNT, topics=(1,)))
    mv = memoryview(ring)

    # warmup: 10k producer/consumer cycles on a throwaway reader
    warm_reader = RingReader(ring)
    for i in range(WARMUP):
        _producer_write(mv, i + 1, 1, FMT_POINTS_F32, len(payload))
        warm_reader.acquire()
    del warm_reader
    gc.collect()

    reader = RingReader(ring)
    tracemalloc.start()
    for i in range(RECORDS):
        seq = WARMUP + i + 1
        _producer_write(mv, seq, 1, FMT_POINTS_F32, len(payload))
        rec = reader.acquire()
        assert rec is not None and rec.fmt == FMT_POINTS_F32
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    assert reader.torn == 0
    assert current < GATE_BYTES, (
        f'retained growth {current} B over {RECORDS} ring acquisitions '
        f'exceeds {GATE_BYTES} B gate (Law 1)')
    assert peak > 0


def test_camera_slot_cycle_retained_growth_under_gate():
    cam = CameraSource(width=64, height=48, fmt=FMT_GRAY8, slots=4)
    for _ in range(WARMUP):
        f, _w = cam.grab_into_slot()
        f.release()
    gc.collect()

    tracemalloc.start()
    for _ in range(RECORDS):
        f, _w = cam.grab_into_slot()
        f.release()
    current, _peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    cam.close()

    assert current < GATE_BYTES, (
        f'retained growth {current} B over {RECORDS} slot cycles '
        f'exceeds {GATE_BYTES} B gate (Law 1)')


def test_negative_control_bites(ring_factory):
    payload = _payload()
    ring = bytearray(ring_factory(
        [(1, FMT_POINTS_F32, payload, False)] * SLOT_COUNT,
        slot_size=SLOT_SIZE, slot_count=SLOT_COUNT, topics=(1,)))
    mv = memoryview(ring)
    RETAIN_SINK.clear()
    reader = RingReader(ring)
    tracemalloc.start()
    for i in range(50_000):
        _producer_write(mv, i + 1, 1, FMT_POINTS_F32, len(payload))
        rec = reader.acquire()
        if rec is not None:
            RETAIN_SINK.append({'fmt': rec.fmt, 'ts': rec.ts_ns})
    current, _ = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    retained = len(RETAIN_SINK)
    RETAIN_SINK.clear()
    assert retained == 50_000
    assert current > GATE_BYTES, (
        'negative control did NOT bite — probe insensitive')
