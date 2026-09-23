# test_ring.py — RNG1 ring reader: header validation, seqlock protocol,
# topic filtering, flyweight semantics, fixture parity with the TS reader.

import struct

import pytest

from weft_robotics import (RingReader, Rng1Error, RNG1_HEADER_SIZE,
                           FMT_IMU6DOF, FMT_POINTS_F32, FMT_FRAME_DESC)
from conftest import (build_ring, imu_payload, points_payload, boxes_payload,
                      frm1_payload)


def test_header_fail_closed():
    with pytest.raises(Rng1Error) as e:
        RingReader(b'RNG')
    assert e.value.code == 1  # E_SHORT
    bad_magic = bytearray(build_ring([]))
    bad_magic[0] = 0x58
    with pytest.raises(Rng1Error) as e:
        RingReader(bytes(bad_magic))
    assert e.value.code == 2  # E_MAGIC
    bad_ver = bytearray(build_ring([]))
    struct.pack_into('<H', bad_ver, 4, 2)
    with pytest.raises(Rng1Error) as e:
        RingReader(bytes(bad_ver))
    assert e.value.code == 3  # E_VERSION
    bad_geom = bytearray(build_ring([]))
    struct.pack_into('<I', bad_geom, 8, 4000)  # not a power of two
    with pytest.raises(Rng1Error) as e:
        RingReader(bytes(bad_geom))
    assert e.value.code == 5  # E_GEOMETRY


def test_acquire_newest_wins_and_field_decode():
    ring = build_ring([
        (1, FMT_IMU6DOF, imu_payload(ts=111), False),
        (1, FMT_IMU6DOF, imu_payload(ts=222), False),
    ], topics=(1,))
    r = RingReader(ring)
    rec = r.acquire(topic_id=1)
    assert rec is not None and rec.fmt == FMT_IMU6DOF
    vals = rec.imu()
    assert vals[0] == 222  # newest record
    assert abs(vals[1] - 0.7071) < 1e-9
    assert r.committed_seq() == 2
    assert r.acquire(topic_id=1) is None  # consumed: newest-wins, no repeat


def test_torn_record_is_counted_not_thrown():
    # torn write-in-flight publishes seq|1: detectable ONLY at even
    # positions (the frozen fixture's torn record is seq 4 for the same
    # reason — the seqlock's documented odd-seq blind spot at odd s).
    ring = build_ring([
        (1, FMT_POINTS_F32, points_payload(4), False),
        (1, FMT_POINTS_F32, points_payload(4), True),   # torn at seq 2
    ], topics=(1,))
    r = RingReader(ring)
    rec = r.acquire()
    assert rec is None
    assert r.torn == 1
    assert r.acquires == 1  # drop-not-block: counted, never raised


def test_topic_filter_counts_filtered():
    ring = build_ring([
        (2, FMT_IMU6DOF, imu_payload(), False),
    ], topics=(1, 2))
    r = RingReader(ring)
    assert r.acquire(topic_id=1) is None
    assert r.filtered == 1
    assert r.topic_table() == [1, 2]


def test_payload_zero_copy_views():
    ring = build_ring([
        (1, FMT_POINTS_F32, points_payload(6), False),
        (1, FMT_BOXES_F32 if 'FMT_BOXES_F32' in dir() else 4,
         boxes_payload(2), False),
    ], topics=(1,))
    r = RingReader(ring)
    np = pytest.importorskip('numpy')
    seen = []
    for rec in r.drain():
        seen.append(rec)
        if rec.fmt == 4:
            mv, n = rec.boxes_view()
            assert n == 2
        else:
            mv, n = rec.points_view()
            assert n == 6
            arr = np.frombuffer(mv, dtype='<f4')
            assert arr.shape == (18,)
            assert float(arr[0]) == 0.0 and float(arr[3]) == 1.0
    assert len(seen) == 2  # drain walks the full committed window


def test_frm1_descriptor_roundtrip():
    ring = build_ring([
        (3, FMT_FRAME_DESC, frm1_payload(width=64, height=48, stride=192),
         False),
    ], topics=(3,))
    r = RingReader(ring)
    rec = next(iter(r.drain()))
    d = rec.frm1()
    assert d is not None
    assert d['width'] == 64 and d['height'] == 48 and d['stride'] == 192
    assert d['format'] == 1 and d['handle'] == (7, 0)


def test_fixture_parity_with_frozen_rng1(tmp_path):
    """Decode the COMMITTED rng1-ring.bin fixture (TS-side frozen) —
    same committed seq, same per-record topics/fmts/torn accounting."""
    from pathlib import Path
    root = Path(__file__).resolve().parents[3]
    fixture = root / 'tests' / 'adapters' / 'managed' / 'fixtures' / 'rng1-ring.bin'
    if not fixture.exists():
        pytest.skip('fixture not frozen')
    r = RingReader(fixture.read_bytes())
    assert r.slot_size == 4096 and r.slot_count == 8
    assert r.committed_seq() == 10
    seen = []
    for rec in r.drain():
        seen.append((rec.topic_id, rec.fmt, rec.payload_len))
    assert len(seen) >= 7   # 10 records incl. 1 torn + filtered variety
    assert any(t == 1 for t, _f, _l in seen)
