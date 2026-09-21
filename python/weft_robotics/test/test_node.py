# test_node.py — RoboticsNode managed shim: subscribe_zerocopy contract,
# typed errors, stats. (Alloc gate lives in test_alloc.py.)

import pytest

from weft_robotics import RoboticsNode, Rng1Error, FMT_POINTS_F32
from conftest import build_ring, points_payload


def test_subscribe_requires_attach():
    node = RoboticsNode()
    with pytest.raises(Rng1Error) as e:
        node.subscribe_zerocopy('lidar', 'points_f32')
    assert e.value.code == -2


def test_unknown_type_fail_closed():
    node = RoboticsNode().attach_ring(build_ring([], topics=(1,)))
    with pytest.raises(Rng1Error):
        node.subscribe_zerocopy('lidar', 'frobnicate')


def test_subscribe_zerocopy_yields_newest():
    ring = build_ring([
        (1, FMT_POINTS_F32, points_payload(4), False),
        (1, FMT_POINTS_F32, points_payload(8), False),
        (2, FMT_POINTS_F32, points_payload(2), False),
    ], topics=(1, 2))
    node = RoboticsNode().attach_ring(ring)
    node.bind_topic('lidar', 1)
    node.bind_topic('cam', 2)
    np = pytest.importorskip('numpy')

    got = []
    for rec in node.subscribe_zerocopy('cam', 'points_f32'):
        mv, n = rec.points_view()
        arr = np.frombuffer(mv, dtype='<f4').reshape(n, 3)
        got.append((rec.topic_id, n, float(arr[0, 0])))
    assert len(got) == 1  # newest-wins: one record per pass
    assert got[0][0] == 2 and got[0][1] == 2  # the topic-2 record


def test_unbound_topic_name_fail_closed():
    node = RoboticsNode().attach_ring(build_ring([], topics=(1,)))
    with pytest.raises(Rng1Error) as e:
        node.subscribe_zerocopy('nope', 'points_f32')
    assert e.value.code == -3


def test_subscription_any_topic_and_stats():
    ring = build_ring([
        (1, FMT_POINTS_F32, points_payload(2), False),
        (2, FMT_POINTS_F32, points_payload(2), False),
    ], topics=(1, 2))
    node = RoboticsNode().attach_ring(ring)
    seen = [rec.topic_id for rec in
            node.subscribe_zerocopy(None, None)]
    assert seen == [2]
    st = node.stats()
    assert st['acquires'] >= 1 and st['committed_seq'] == 2


def test_rmw_weft_seam_shape():
    """Documented contract with Engineer 2's native RMW: the managed
    shim consumes an RNG1 ring; attach is the ONLY native touch point."""
    node = RoboticsNode()
    assert node.attached is False
    node.attach_ring(build_ring([], topics=(1,)))
    assert node.attached is True
    assert node.topic_ids() == [1]
    node.close()
    assert node.attached is False
