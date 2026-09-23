# test_cluster_router.py — Python rendezvous routing must resolve the SAME
# owners as the TS ShardRouter for the frozen fixture (cross-language
# consistent hashing proof) and balance/minimal-disruption invariants.
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from weft_cluster import wire
from weft_cluster.topology import ShardRouter  # noqa: E402

VEC = json.loads(
    (Path(__file__).resolve().parents[2] / "fixtures" / "cluster" / "router_vector.json")
    .read_text())


def test_router_vector_resolves_identical_owners():
    r = ShardRouter()
    for n in VEC["nodes"]:
        r.add_node(n)
    for t in VEC["topics"]:
        owner = r.owner_of(t["topicHash"]["hi"] << 32 | t["topicHash"]["lo"])
        assert owner == t["owner"], f"topic {t['topic']}: {owner} != {t['owner']}"
    # And by NAME (hashing parity end-to-end).
    for t in VEC["topics"]:
        assert r.owner_of(wire.topic_hash64(t["topic"])) == t["owner"]


def test_balance_within_3x_envelope():
    r = ShardRouter()
    for i in range(1, 9):
        r.add_node(i)
    counts = {}
    for i in range(1000):
        o = r.owner_of(wire.topic_hash64(f"topic-{i}"))
        counts[o] = counts.get(o, 0) + 1
    assert len(counts) == 8
    assert max(counts.values()) / min(counts.values()) < 3


def test_minimal_disruption_when_scaling_out():
    r = ShardRouter()
    for i in range(1, 9):
        r.add_node(i)
    owners = {i: r.owner_of(wire.topic_hash64(f"t-{i}")) for i in range(1000)}
    r.add_node(9)
    moved = sum(1 for i, o in owners.items()
                if r.owner_of(wire.topic_hash64(f"t-{i}")) != o)
    assert 0.05 < moved / 1000 < 0.20
