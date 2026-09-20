# WCN1 — Weft Cluster Network Wire Format v1 (NORMATIVE)

Status: **NORMATIVE** for `packages/weft-cluster`, `python/weft_cluster`,
`tools/weft-cluster-cli` and `demos/distributed-cluster-feed`.
Layer: managed cluster plane (Pillar 3). Sits on top of the WCR1 distributed
memory mesh (Engineer 1 protocol + Engineer 2 RDMA/XDP substrate).

## Law compliance (Weft Core Laws, Pillar 3 edition)

- **Law 1 (zero hot-path allocation):** every encode/decode primitive in this
  spec has a handle-reusing form: `encodeInto(handle, buf, off)` /
  `decodeInto(buf, off, handle)`. No object, array, string, TypedArray or
  DataView may be constructed per frame on the hot path.
- **Law 2 (endianness & determinism):** every multi-byte field is **explicitly
  little-endian** (DataView `true` flag / Python `struct '<'`). float fields,
  when present in payloads, are IEEE 754 little-endian. Any non-LE byte stream
  is rejected at the boundary with `WC_E_BAD_HEADER`.
- **Law 3 (byte-frozen kernel core):** WCN1 does not touch `core/c/weft.{c,h}`.
  WCN1 is a *network* envelope; it deliberately mirrors the core envelope
  philosophy (magic, version, header_size, seq, payload_len) so a WCN1 frame
  can be bridged into a core `weft_t` ring without re-serialization.
- **Law 4 (honest boundary validation):** every rejection maps to a stable
  numeric error code (§5). Unknown *trailing* header bytes (between the parsed
  header and `header_size`) are skipped, never fatal — forward compatibility.

## 1. WCN1 datagram header — 64 bytes

A WCN1 datagram is `[64-byte header][payload]` when `FLAGS.INLINE_PAYLOAD`,
or `[64-byte header]` alone (descriptor-only) when `FLAGS.RDMA_REF` — the
payload then lives in WCR1-registered memory addressed by `rdma_key` and
`payload_offset` semantics owned by the substrate.

| Off | Size | Type   | Field           | Notes                                            |
|-----|------|--------|-----------------|--------------------------------------------------|
| 0   | 4    | bytes  | magic           | `"WCN1"` = `57 43 4E 31`                          |
| 4   | 2    | u16 LE | version         | `1`                                              |
| 6   | 2    | u16 LE | header_size     | `64` (may grow in future versions)               |
| 8   | 4    | u32 LE | flags           | bit0 INLINE, bit1 RDMA_REF, bit2 CRC_PRESENT      |
| 12  | 4    | u32 LE | src_node_id     | sender cluster node id                            |
| 16  | 8    | u64 LE | topic_hash      | FNV-1a 64 of topic name (UTF-8 bytes)             |
| 24  | 8    | u64 LE | seq             | monotonic per `(src_node_id, topic_hash)`         |
| 32  | 8    | u64 LE | timestamp_ns    | sender monotonic clock at send                    |
| 40  | 4    | u32 LE | payload_len     | bytes of payload (0 for RDMA_REF)                 |
| 44  | 4    | u32 LE | schema_id       | producer-declared payload schema                  |
| 48  | 4    | u32 LE | crc32           | IEEE CRC-32 of payload (0 when CRC not present)   |
| 52  | 4    | u32 LE | rdma_key        | WCR1 memory key (0 for inline)                    |
| 56  | 8    | u64 LE | reserved        | must be 0                                         |

Header is exactly one 64-byte cache line. 64-bit fields are stored as one u64
LE; JS implementations split into hi/lo u32 pairs (lo first at the offset,
hi at offset+4) — this is *the* LE u64 layout, not an implementation detail.

## 2. WGS1 gossip message — 48-byte header + 32-byte entries

Gossip and heartbeats share one format. A heartbeat is a gossip message whose
`entry_count` is `0` (or carries the sender's own entry).

| Off | Size | Type   | Field                | Notes                                  |
|-----|------|--------|----------------------|----------------------------------------|
| 0   | 4    | bytes  | magic                | `"WGS1"` = `57 47 53 31`               |
| 4   | 2    | u16 LE | version              | `1`                                    |
| 6   | 2    | u16 LE | header_size          | `48`                                   |
| 8   | 4    | u32 LE | sender_node_id       |                                        |
| 12  | 4    | u32 LE | entry_count          | number of 32-byte entries following    |
| 16  | 8    | u64 LE | gossip_round         | sender-local round counter             |
| 24  | 8    | u64 LE | sender_monotonic_ns  | for one-way delay estimation           |
| 32  | 8    | u64 LE | sender_boot_id       | random per node process lifetime       |
| 40  | 4    | u32 LE | sender_flags         | bit0 ALIVE, bit1 LEAVING               |
| 44  | 4    | u32 LE | reserved             | 0                                      |

### 2.1 Gossip entry — 32 bytes

| Off | Size | Type   | Field        | Notes                                   |
|-----|------|--------|--------------|-----------------------------------------|
| 0   | 4    | u32 LE | node_id      |                                         |
| 4   | 4    | u32 LE | addr_ipv4    | network-order-neutral LE u32; 0 = same host |
| 8   | 2    | u16 LE | gossip_port  |                                         |
| 10  | 2    | u16 LE | entry_flags  | bit0 ALIVE, bit1 LEAVING                |
| 12  | 8    | u64 LE | last_seen_ms | sender's view of last heartbeat, ms     |
| 20  | 4    | u32 LE | incarnation  | SWIM-style incarnation counter          |
| 24  | 2    | u16 LE | data_port    | WCN1 data socket (0 = same as gossip)   |
| 26  | 2    | u16 LE | reserved     | 0                                       |
| 28  | 4    | u32 LE | reserved     | 0                                       |

## 3. Membership table (lock-free snapshot)

Each node mirrors cluster state into a local `SharedArrayBuffer` region laid
out as a **seqlock**: a generation word followed by fixed-width entries.

```
0   4   u32 LE  generation (odd = writer active)
4   4   u32 LE  entry_count
8   40  entry[0..N] — 40 bytes each:
    +0  4   node_id        +4  4  addr_ipv4
    +8  2   gossip_port    +10 2  entry_flags
    +12 8   last_seen_ms   +20 4  incarnation
    +24 2   data_port      +26 2  reserved
    +28 4   topic_count (owned shard count, advisory)
```

Reader fast path (Law 1: zero allocation, preallocated handle):

```
g1 = Atomics.load(gen)          // ~5 ns warm
if (g1 & 1) retry
read entry fields (plain typed loads)
g2 = Atomics.load(gen)
if (g1 !== g2) retry
```

Writer: `gen+1 (odd) -> write -> gen+1 (even)`, all `Atomics.store/release`.
Target lookup cost on warm cache: **< 10 ns** (two atomic loads + plain
typed-array reads). Evidence: `bench/evidence/router.json` records the
measured distribution.

## 4. Hashing

- `topic_hash` = FNV-1a 64 over the UTF-8 bytes of the topic name:
  `hash = 0xcbf29ce484222325`; per byte: `hash ^= byte; hash *= 0x100000001b3 (mod 2^64)`.
- Shard routing = **rendezvous hashing**: `score(node, topic) =
  fnv1a64(node_seed_u32 || topic_hash)` (8 input bytes: u32 LE seed + u64 LE
  topic hash); the node with the highest score owns the topic. Ties broken by
  higher node_id. Deterministic across languages; adding/removing a node only
  moves that node's shares (minimal disruption).

## 5. Error codes (Law 4) — stable, cross-language

| Code | Name                  | Meaning                                            |
|------|-----------------------|----------------------------------------------------|
| 0    | WC_OK                 | success                                            |
| 1    | WC_E_BAD_MAGIC        | magic is not `"WCN1"` / `"WGS1"`                    |
| 2    | WC_E_BAD_VERSION      | version > 1 (we never downgrade-read)              |
| 3    | WC_E_TRUNCATED        | buffer < header_size, or payload_len overruns       |
| 4    | WC_E_BAD_HEADER       | header_size < minimum, or non-LE marker mismatch    |
| 5    | WC_E_BAD_CRC          | payload CRC-32 mismatch                             |
| 6    | WC_E_UNKNOWN_NODE     | src_node_id absent from membership table            |
| 7    | WC_E_STALE_SEQ        | seq <= last-seen for (src, topic) — replay/gap probe|
| 8    | WC_E_TOPIC_UNROUTED   | no live node owns the requested topic               |
| 9    | WC_E_PARTITION        | gossip quorum lost                                  |
| 10   | WC_E_NODE_LOST        | heartbeat timeout on a required peer                |
| 11   | WC_E_SHUTDOWN         | local node is leaving                               |
| 12   | WC_E_SCHEMA_MISMATCH  | payload schema_id != expected schema                |

Implementations must expose both the numeric code and the name. Codes are
byte-frozen: appending new codes is allowed, renumbering is not.

## 6. Cross-language parity

`ci/scripts/run_weft_cluster_shard.sh` must verify:

1. TS encodes a fixed vector set of datagrams to a file; Python decodes and
   field-checks them (and vice versa).
2. Live UDP loop: TS producer → Python consumer and Python producer → TS
   consumer on localhost.
3. Both languages reject the same malformed-byte fixtures with the *same*
   error codes.
