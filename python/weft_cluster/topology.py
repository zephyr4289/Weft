# topology.py — lightweight Python-side topology: rendezvous ShardRouter
# (byte-exact with TS — proven by fixtures/cluster/router_vector.json) and a
# WGS1 gossip codec. Full membership/gossip engines live in the TS mesh; the
# Python SDK consumes their state for routing and status display.

from .wire import topic_hash64  # noqa: F401  (public re-export)

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
U64 = 0xFFFFFFFFFFFFFFFF

# FNV prime split for exact 64-bit multiply via 32-bit limbs.
_P_LO = 0x1B3
_P_HI = 0x100


class _State:
    """Mutable fnv accumulator (avoids tuple churn in owner resolution)."""

    __slots__ = ("hi", "lo")


def _fnv_reset(s: _State) -> None:
    s.hi = 0xCBF29CE4
    s.lo = 0x84222325


def _fnv_byte(s: _State, b: int) -> None:
    s.lo = (s.lo ^ b) & 0xFFFFFFFF
    prod_lo = s.lo * _P_LO                      # < 2^41 — exact
    new_lo = prod_lo & 0xFFFFFFFF
    carry = prod_lo >> 32
    prod_hi = s.lo * _P_HI + s.hi * _P_LO + carry  # < 2^42 — exact
    s.lo = new_lo
    s.hi = prod_hi & 0xFFFFFFFF


def _score(node_id: int, topic_hash: int) -> tuple[int, int]:
    """fnv1a64(u32LE(node_id) || u64LE(topic_hash)) as (hi, lo) — mirrors
    packages/weft-cluster/src/router.js byte-for-byte."""
    s = _State()
    _fnv_reset(s)
    for shift in (0, 8, 16, 24):
        _fnv_byte(s, (node_id >> shift) & 0xFF)
    for shift in (0, 8, 16, 24, 32, 40, 48, 56):
        _fnv_byte(s, (topic_hash >> shift) & 0xFF)
    return (s.hi, s.lo)


class ShardRouter:
    """Rendezvous (highest-random-weight) topic -> node router."""

    def __init__(self):
        self.nodes: list[int] = []
        self.version = 0

    def add_node(self, node_id: int) -> "ShardRouter":
        node_id &= 0xFFFFFFFF
        if node_id not in self.nodes:
            self.nodes.append(node_id)
            self.nodes.sort()
            self.version += 1
        return self

    def remove_node(self, node_id: int) -> "ShardRouter":
        node_id &= 0xFFFFFFFF
        if node_id in self.nodes:
            self.nodes.remove(node_id)
            self.version += 1
        return self

    def owner_of(self, topic_hash: int) -> int | None:
        """Owner node id, or None when unrouted. Deterministic across
        languages (frozen vector: fixtures/cluster/router_vector.json)."""
        if not self.nodes:
            return None
        best = None
        best_score = (-1, -1)
        for node in self.nodes:
            score = _score(node, topic_hash)
            if score > best_score or (score == best_score and
                                      (best is None or node > best)):
                best_score = score
                best = node
        return best
