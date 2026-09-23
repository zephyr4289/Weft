# wire.py — WCN1 (datagram) + WGS1 (gossip) wire format, byte-exact mirror of
# packages/weft-cluster/src/wire.js, per docs/weft-cluster/WIRE-V1.md.
#
# Law 2: every multi-byte field uses explicit little-endian struct formats
# ('<') and is verified against the TS implementation by cross-language
# vector fixtures (fixtures/cluster/wcn1_vectors.json).
# Law 4: decoders return stable numeric codes on the hot path; the throwing
# wrapper exists only for cold-path callers.

import struct
import zlib

WCN1_MAGIC = b"WCN1"
WGS1_MAGIC = b"WGS1"
WCN1_VERSION = 1
WCN1_HEADER_SIZE = 64
WGS1_VERSION = 1
WGS1_HEADER_SIZE = 48
WGS1_ENTRY_SIZE = 32

WC_FLAG_INLINE_PAYLOAD = 1
WC_FLAG_RDMA_REF = 2
WC_FLAG_CRC_PRESENT = 4
WC_FLAG_CONTROL = 8  # payload[0]: 1 = SUB, 2 = UNSUB

# <4sHH  magic, version, header_size
# I      flags
# I      src_node_id
# II     topic_hash (lo, hi)
# II     seq (lo, hi)
# II     timestamp_ns (lo, hi)
# I      payload_len
# I      schema_id
# I      crc32
# I      rdma_key
# II     reserved (must be 0)
_WCN1 = struct.Struct("<4sHHIIQQQIIIIQ")
assert _WCN1.size == WCN1_HEADER_SIZE

_WGS1 = struct.Struct("<4sHHIIQQQII")
assert _WGS1.size == WGS1_HEADER_SIZE

_WGS_ENTRY = struct.Struct("<IIHHQHHII")
assert _WGS_ENTRY.size == WGS1_ENTRY_SIZE

_GOSSIP_FLAG_ALIVE = 1
_GOSSIP_FLAG_LEAVING = 2

CTL_SUB = 1
CTL_UNSUB = 2


def fnv1a64(data) -> int:
    """FNV-1a 64 over bytes — byte-exact with the TS BigInt implementation."""
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def topic_hash64(topic: str) -> int:
    """FNV-1a 64 of a topic name's UTF-8 bytes."""
    return fnv1a64(topic.encode("utf-8"))


def crc32_ieee(data) -> int:
    """IEEE CRC-32 (zlib) — byte-exact with packages/weft-cluster crc32.js."""
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


class Frame:
    """Reusable decode target for one WCN1 datagram (Law 1: keep ONE per hot
    path and re-target it; views reference the underlying buffer — zero copy)."""

    __slots__ = ("mv", "offset", "version", "header_size", "flags", "src_node",
                 "topic_hash", "seq", "timestamp_ns", "payload_len", "schema_id",
                 "crc", "rdma_key", "payload")

    def __init__(self):
        self.mv = None
        self.offset = 0

    def fields(self):
        return {
            "version": self.version, "header_size": self.header_size,
            "flags": self.flags, "src_node": self.src_node,
            "topic_hash": self.topic_hash, "seq": self.seq,
            "timestamp_ns": self.timestamp_ns, "payload_len": self.payload_len,
            "schema_id": self.schema_id, "crc": self.crc,
            "rdma_key": self.rdma_key,
        }


def decode_wcn1_into(mv, off: int, frame: Frame) -> int:
    """Decode + validate a WCN1 header from memoryview `mv` at `off`.
    RETURNS a stable WC_* code (0 = WC_OK) — never raises on the hot path."""
    avail = len(mv) - off
    if avail < 4:
        return 3  # WC_E_TRUNCATED
    if bytes(mv[off:off + 4]) != WCN1_MAGIC:
        return 1  # WC_E_BAD_MAGIC
    if avail < 8:
        return 3
    (_magic, version, header_size) = struct.unpack_from("<4sHH", mv, off)
    if version > WCN1_VERSION:
        return 2  # WC_E_BAD_VERSION
    if header_size < WCN1_HEADER_SIZE or header_size > avail:
        return 4  # WC_E_BAD_HEADER
    (flags, src_node, topic_hash, seq, ts, payload_len, schema_id, crc,
     rdma_key) = struct.unpack_from("<IIQQQIIII", mv, off + 8)
    inline = (flags & WC_FLAG_INLINE_PAYLOAD) != 0
    if inline and payload_len > avail - header_size:
        return 3  # WC_E_TRUNCATED
    frame.mv = mv
    frame.offset = off
    frame.version = version
    frame.header_size = header_size
    frame.flags = flags
    frame.src_node = src_node
    frame.topic_hash = topic_hash
    frame.seq = seq
    frame.timestamp_ns = ts
    frame.payload_len = payload_len
    frame.schema_id = schema_id
    frame.crc = crc
    frame.rdma_key = rdma_key
    frame.payload = mv[off + header_size:off + header_size + payload_len]
    return 0  # WC_OK


def encode_wcn1(flags: int, src_node: int, topic_hash: int, seq: int,
                timestamp_ns: int, payload, schema_id: int = 1,
                crc: int | None = None, rdma_key: int = 0) -> bytearray:
    """Cold-path-friendly full datagram encode (header + inline payload).
    crc=None computes the IEEE CRC-32 automatically when CRC_PRESENT."""
    if crc is None:
        crc = crc32_ieee(payload) if (flags & WC_FLAG_CRC_PRESENT) else 0
    head = _WCN1.pack(WCN1_MAGIC, WCN1_VERSION, WCN1_HEADER_SIZE, flags,
                      src_node, topic_hash, seq, timestamp_ns, len(payload),
                      schema_id, crc, rdma_key, 0)
    out = bytearray(head)
    out += payload
    return out


def verify_frame_crc(frame: Frame) -> int:
    if not (frame.flags & WC_FLAG_CRC_PRESENT):
        return 0
    got = crc32_ieee(frame.payload)
    return 0 if got == frame.crc else 5  # WC_E_BAD_CRC


def parse_wcn1(mv, off: int = 0) -> Frame:
    """Cold-path decode that raises WeftClusterError on malformed input."""
    from .errors import WeftClusterError
    f = Frame()
    code = decode_wcn1_into(mv, off, f)
    if code != 0:
        raise WeftClusterError(code)
    return f


# --- WGS1 gossip -------------------------------------------------------------

def encode_wgs1(sender_node: int, entries, gossip_round: int,
                sender_monotonic_ns: int, boot_id: int, sender_flags: int) -> bytes:
    """entries: list of dicts {node_id, addr, gossip_port, entry_flags,
    last_seen_ms, incarnation, data_port}."""
    head = _WGS1.pack(WGS1_MAGIC, WGS1_VERSION, WGS1_HEADER_SIZE, sender_node,
                      len(entries), gossip_round, sender_monotonic_ns, boot_id,
                      sender_flags, 0)
    out = bytearray(head)
    for e in entries:
        out += _WGS_ENTRY.pack(e["node_id"], e["addr"], e["gossip_port"],
                               e["entry_flags"], e["last_seen_ms"],
                               e["incarnation"], e["data_port"], 0, 0)
    return bytes(out)


def decode_wgs1_into(mv, handle: dict) -> int:
    """Decode a WGS1 header into a reusable dict handle. Returns WC_* code."""
    avail = len(mv)
    if avail < 4:
        return 3
    if bytes(mv[0:4]) != WGS1_MAGIC:
        return 1
    if avail < 8:
        return 3
    (_magic, version, header_size) = struct.unpack_from("<4sHH", mv, 0)
    if version > WGS1_VERSION:
        return 2
    if header_size < WGS1_HEADER_SIZE or header_size > avail:
        return 4
    (sender_node, entry_count, gossip_round, sender_ts, boot_id, sender_flags,
     _rsv) = struct.unpack_from("<IIQQQII", mv, 8)
    if header_size + entry_count * WGS1_ENTRY_SIZE > avail:
        return 3
    handle.update(sender_node=sender_node, entry_count=entry_count,
                  gossip_round=gossip_round, sender_monotonic_ns=sender_ts,
                  boot_id=boot_id, sender_flags=sender_flags)
    return 0


def read_gossip_entry(mv, index: int) -> dict:
    base = WGS1_HEADER_SIZE + index * WGS1_ENTRY_SIZE
    (node_id, addr, gossip_port, entry_flags, last_seen_ms, incarnation,
     data_port, _r1, _r2) = _WGS_ENTRY.unpack_from(mv, base)
    return {"node_id": node_id, "addr": addr, "gossip_port": gossip_port,
            "entry_flags": entry_flags, "last_seen_ms": last_seen_ms,
            "incarnation": incarnation, "data_port": data_port}
