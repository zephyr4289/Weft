---
RFC: 0018
Title: WCR1 — Weft Cluster Ring v1: distributed zero-copy tensor fabric protocol, two-store seqlock, and ring-embedded consensus
Status: Implemented (Pillar 3, Senior Systems Engineer 1)
Authors: weft-contributor
Created: 2026-09-20
Supersedes / Superseded-by: None
---

# RFC 0018 — WCR1 (Weft Cluster Ring v1)

## Summary

`core/c/cluster/` adds the protocol engine, memory layout, and
consensus/arbitration layer for **weft-cluster**: Weft's zero-copy,
zero-serialization memory model extended to distributed multi-node
clusters over one-sided RDMA (RoCEv2/InfiniBand). The normative artifact
is the **WCR1 ring**: a 128-byte ring header + 64-byte slot headers,
little-endian, 64-byte cacheline bracketed, `ibv_reg_mr`-ready, with a
**two-store atomic publish protocol** (producer_seq hi/lo split) whose
commit point is a single hardware DMA write completion, a **wait-free
consumer acquire** with automatic sequence-tear detection and bounded
retry, **in-band backpressure** embedded in slot headers (no TCP ACKs,
no broker), and a **ring-embedded Raft** — leader election, leases,
term epochs and fencing tokens living directly inside the ring headers,
with deterministic split-brain arbitration and no external coordination
service.

The frozen kernel (`core/c/weft.{c,h}`) is untouched; the module is
additive and self-contained.

## Motivation

The strategic directive names the target: **< 500 ns cross-server memory
synchronization** over RoCEv2/InfiniBand one-sided writes, obliterating
the broker class (Kafka, NATS, ZeroMQ, WebSockets) whose
serialize/broker/queue/acknowledge/deserialize round trips cost
milliseconds. The design consequence: the cluster fabric must inherit
Weft's laws — zero heap allocation on the steady-state path, bounded and
deterministic execution, byte-frozen kernel core, and honest refusal
ladders — while remaining compatible with hardware DMA write completion
semantics (no read-modify-write protocol steps on the data path).

## 1. Topology and roles

A cluster of N nodes forms a full mesh of **pairwise rings**:

- `R(P → H)`: a ring **hosted** by node H (the memory lives on H), with
  exactly one **remote producer** P. H is the local consumer.
- Every node hosts N−1 rings (one per peer producer) and produces into
  N−1 rings hosted by peers.

Consequences (all deliberate, all load-bearing):

- **All reads are local.** Consumers, consensus observers and election
  tallies read their own hosted memory — zero network reads in steady
  state.
- **All writes are one-sided.** A producer computes the remote slot
  address by pure arithmetic (`wcr1_slot_byte_offset`) and issues
  direct stores through the transport seam — zero roundtrip RPCs. The
  memory-region base is exchanged once, out-of-band, at attach.
- **Single-writer per field, always.** Field ownership (§2) makes every
  register single-writer, which is what makes the lock-free protocol
  sound on top of plain DMA writes (no RMW verbs required on the data
  path; RoCEv2 without atomics is sufficient).
- Elections ride the data plane: VOTE_REQUEST / VOTE_GRANT slots flow
  through the same rings as tensors (§6.3).

## 2. Normative memory layout

All multi-byte fields are **little-endian** on the wire and in shared
memory. WCR1 requires little-endian hosts (x86-64, AARCH64-LE; enforced
by `#error` at compile time). Regions are allocated 128-byte aligned and
page-locked best-effort (`mlock`; a failure is a deployment fact, not an
engine error — see §9). Every ring region is exactly:

```
region_size = 128 + capacity_slots * slot_size     (bytes)
slot i      = region + 128 + i * slot_size
payload     = slot + 64;  payload_capacity = slot_size - 64
capacity_slots is a power of two in [2, 2^20]
slot_size is a multiple of 64 and >= 128
```

### 2.1 Ring header — 128 bytes (two cachelines)

**Cacheline 0 (0x00–0x3F): identity + geometry — IMMUTABLE after create.**

| Offset | Size | Field                | Semantics |
|--------|------|----------------------|-----------|
| 0x00 | 4 | `magic` | `0x31524357` (LE bytes `'W','C','R','1'`) |
| 0x04 | 1 | `layout_major` | `1` — bump on incompatible change |
| 0x05 | 1 | `layout_minor` | `0` — bump on compatible change |
| 0x06 | 2 | `hdr_flags` | reserved, zero |
| 0x08 | 16 | `cluster_id` | UUIDv4, canonical byte order |
| 0x18 | 4 | `home_node_id` | the hosting (consumer) node |
| 0x1C | 4 | `producer_node_id` | the single remote producer |
| 0x20 | 4 | `config_crc32` | CRC-32/IEEE over bytes [0x00,0x20) |
| 0x24 | 4 | `reserved0` | zero |
| 0x28 | 8 | `epoch` | cluster config epoch (monotonic) |
| 0x30 | 8 | `capacity_slots` | power of two |
| 0x38 | 8 | `slot_size` | multiple of 64, ≥ 128 |

**Cacheline 1 (0x40–0x7F): synchronization + consensus — field ownership is single-writer.**

| Offset | Size | Field | Owner | Semantics |
|--------|------|-------|-------|-----------|
| 0x40 | 4 | `seq_hi` | producer | producer_seq bits 32..62 \| parity mirror (bit 31) |
| 0x44 | 4 | `seq_lo` | producer | producer_seq bits 0..31 — **THE COMMIT STORE** |
| 0x48 | 4 | `cons_hi` | home node | consumer watermark hi \| parity |
| 0x4C | 4 | `cons_lo` | home node | consumer watermark lo — commit store |
| 0x50 | 8 | `term` | producer | cluster Raft term (monotonic) |
| 0x58 | 8 | `lease_expire_ns` | producer | leader lease expiry (mono clock) |
| 0x60 | 4 | `leader_node_id` | producer | current leader (0 = none) |
| 0x64 | 4 | `cflags` | producer | `WCR1_CFLAG_NODE_EVICTED` (bit 0); **written only inside the seqlock bracket** |
| 0x68 | 8 | `fencing_token` | producer | `(term << 32) \| leader` — written LAST before commit |
| 0x70 | 8 | `heartbeat_seq` | producer | consensus seqlock: += 2 per publish; even = committed |
| 0x78 | 8 | `quorum_mask` | producer | observability: nodes ACKing the last refresh |

### 2.2 Slot header — 64 bytes (one cacheline)

| Offset | Size | Field | Semantics |
|--------|------|-------|-----------|
| 0x00 | 8 | `message_seq` | sequence assigned by the producer |
| 0x08 | 8 | `timestamp_ns` | producer monotonic clock at publish |
| 0x10 | 8 | `fencing_token` | publisher's token at publish (0 = pre-consensus) |
| 0x18 | 8 | `producer_epoch` | cluster epoch at publish |
| 0x20 | 4 | `payload_bytes` | ≤ payload_capacity |
| 0x24 | 4 | `flags` | `WCR1_SLOT_F_*` |
| 0x28 | 8 | `bp_lag` | in-band producer-side lag snapshot |
| 0x30 | 4 | `payload_crc32` | iff `F_WITH_CRC` |
| 0x34 | 4 | `header_crc32` | CRC-32/IEEE over [0x00,0x34), iff `F_WITH_CRC` |
| 0x38 | 4 | `producer_node_id` | self-describing producer |
| 0x3C | 4 | `reserved0` | zero |

Slot flags: `F_HEARTBEAT` (liveness echo), `F_VOTE_REQUEST`,
`F_VOTE_GRANT`, `F_BP_MARK` (published at/above high-water),
`F_WITH_CRC` (opt-in integrity), `F_STALE_TOKEN` (reader-side marking).

CRC discipline: integrity CRCs are **opt-in** per publish (`~1 cycle/byte`
makes mandatory CRCs incompatible with the <500 ns budget); RC-QP
transport integrity plus the seq/alias guards are the default.

## 3. The two-store seqlock (RFC-core)

A 64-bit monotonic sequence `S` is published as exactly TWO ordered
32-bit stores:

```
store #1 (data half):   seq_lo := (uint32_t)S
store #2 (COMMIT):      seq_hi := ((S >> 32) & 0x7FFFFFFF) | ((S & 1) << 31)
```

Bit 31 of `seq_hi` **mirrors the parity of `S`** (which alternates every
increment because S increments by exactly 1 per publish).

**Tear detection.** The transient window between the two stores exposes
the pair `(old_hi, new_lo)`; since old/new parities always differ, the
mirror bit mismatches and the reader rejects the pair. The consumer
double-reads both halves; a pair is **stable and valid** iff:

```
h1 == h2  &&  l1 == l2  &&  (h1 >> 31) == (l1 & 1)
```

A producer that crashes between the two stores leaves a permanently torn
pair — readers retry up to `max_acquire_retries` (default 64) and then
return `WEFT_CLUSTER_E_SEQ_TORN`. Bounded retry is the honest bound
(Law 2); the **application contract** is to back off (yield) and retry
the operation, not to spin (proven in R11: 100k-message fork IPC under
saturation).

**Publish order (normative).** Payload store → slot-header store (one
64-byte transport write) → release fence → `seq_lo` store → release
fence → `seq_hi` store. The final `seq_hi` store is the commit point and
the only event a consumer may synchronize on.

**Consumer acquire (wait-free).** Read the stable watermark; `want =
cursor + 1`; if `committed < want` → `E_NOT_PUBLISHED` (non-blocking
miss, never spin); if `committed - want >= capacity` →
`E_SEQ_OVERRUN`; else validate `slot.message_seq == want` (wrapping-alias
guard: `message_seq == want - capacity` → `E_SEQ_OVERRUN`; anything else
→ `E_SEQ_CORRUPT`) and return a **zero-copy view** (pointers into the
ring).

Sequence range: bits 0..62 (2^63−1). Not a limitation in practice
(292 years at 1e9 ops/s); `E_SEQ_CORRUPT` guards violations.

The identical encoding and discipline apply to the **consumer
watermark** (`cons_hi`/`cons_lo`, home-node writer, producer reader) —
the only ACK-equivalent in the protocol.

## 4. Backpressure (in-band, no TCP ACKs)

- The producer reads the peer's watermark (advisory; a torn read falls
  back to the last known value) and computes `lag = committed -
  consumed`.
- `lag_after >= capacity / 2` → the slot is stamped `F_BP_MARK`
  (in-band high-water marker, telemetry for the consumer).
- `lag >= capacity` → the publish is **REFUSED**
  (`WEFT_CLUSTER_E_BACKPRESSURE`): slow consumers are never overrun;
  their unconsumed slots are never overwritten. Bounded memory, honest
  stall.
- A crashed consumer surfaces as a permanently full ring → producer
  stall → eviction path (§6.6).

## 5. Transport seam (Engineer 2 integration contract)

Every remote write is issued through `wcr1_transport_t`:

```c
bool (*store32)(t, volatile uint32_t *dest, uint32_t v);
bool (*store64)(t, volatile uint64_t *dest, uint64_t v);
bool (*store_buf)(t, void *dest, const void *src, size_t len);
```

The default (`WCR1_TRANSPORT_LOCAL`) performs direct stores into mapped
memory — semantics identical to one-sided RDMA writes landing in a
mapped MR; the simulation harness gates these for partitions. The
production transport (libibverbs RC QP / XDP / io_uring) must honor:

1. Stores from ONE producer to ONE destination ring are delivered in
   **issue order** (RC QP write ordering / fenced WRs).
2. A slot's payload/header writes complete before subsequent
   register stores to the same ring.
3. `true` = known delivered (quorum accounting only — never single-message
   correctness, which is fire-and-forget).
4. Links change only at event granularity (a posted write lands whole or
   not at all — never half).

UD / unreliable transports are NOT supported (refused by design: the
two-store protocol's ordering contract is incompatible).

## 6. Ring-embedded consensus (Raft, in-header)

### 6.1 The consensus block seqlock

The consensus fields (term, lease, leader, cflags, token, quorum_mask)
are published by the ring's producer with a seqlock on `heartbeat_seq`:

```
hb := odd  →  term, lease, leader, cflags  →  token  →  hb := even   [COMMIT]
```

The **fencing token embeds `(term, leader)`**, so a block whose fields
came from two different refreshes cannot pass validation: readers check
`(token >> 32) == term && (token & 0xFFFFFFFF) == leader`. This pairing
guard is the cross-writer tear detector; the read fence pattern
(`hb1 acquire → fields relaxed → rmb → hb2 acquire`) prevents
instruction-sched reordering from manufacturing false stability.

### 6.2 Election

- Election timeouts are **node-id staggered deterministic functions**:
  `timeout(node) = 2*lease + (node % 8) * lease/4`. Candidate RETRIES
  re-apply the same stagger (a flat retry window synchronizes dueling
  candidates into a permanent tie — found and fixed during torture).
- A candidate publishes `VOTE_REQUEST` slots (16-byte wire payload:
  `u64 term | u32 candidate | u32 rsvd`) into every peer-hosted ring it
  produces. Unreachable peers are skipped and counted — elections
  campaign *through* partitions.
- A voter grants at most once per term (`E_VOTE_GRANTED` discipline;
  idempotent re-grants are safe because tallying is a bitmask).
- Grants return as `VOTE_GRANT` slots on the candidate's hosted rings;
  the candidate tallies **locally** (zero distributed atomics, zero
  roundtrips). At quorum (`N/2 + 1`) it declares leadership and
  replicates its consensus block to every produced ring.

### 6.3 Leadership

- The leader refreshes leases every `lease/2`: consensus-block write to
  every produced ring + a **header-only heartbeat slot** whose
  consumption by the peer advances that ring's watermark — the liveness
  echo that drives eviction (§6.6).
- Quorum accounting: fewer than a majority of known-delivered refresh
  writes → the leader **steps down** (`E_QUORUM_LOST`). A minority
  partition therefore never retains an acting leader — the split-brain
  window is structurally closed before it opens.
- **Raft safety gate:** a node that has seen term T never adopts a
  leader view with term < T (it ignores the stale view and resolves via
  its own election timeout — the term ladder).

### 6.4 Adoption and arbitration (deterministic)

Each poll round, a node reads every hosted ring's consensus block
locally and computes the winning view: **higher term wins; same term →
lower node id wins**. Two different leaders observed at the same term
(block-vs-block or block-vs-adopted) is surfaced as
`WEFT_CLUSTER_E_SPLIT_BRAIN` (named, counted) while the deterministic
winner is adopted. For non-Byzantine nodes, quorum intersection makes
same-term dueling leaders impossible — the split-brain code path is the
**Byzantine detector** (SB2 proves it with a hand-injected conflicting
view).

Adopted views are gossiped into the adopting node's produced rings on
identity change only (bounded; speeds partition healing; single-writer
ownership preserved).

### 6.5 Fencing tokens

`token = (term << 32) | leader`, strictly ordered: higher term wins.
Every leader publish stamps its token into the slot header; consumers
fence stale-token slots (`E_FENCED`, counted as `fenced_slots`, never
delivered). `wcr1_fence_check()` gates caller-side operations against
the node's max adopted token.

### 6.6 Eviction

A peer whose hosted-ring watermark stays static for
`3 × lease` (while heartbeat slots accumulate) is evicted: the leader
marks the ring's `cflags` `NODE_EVICTED` (inside the seqlock bracket)
and fences it from the data plane (`E_NODE_EVICTED` on publish).
Re-attach to a marked ring is refused. **Rejoin = new epoch** (the
reconfiguration path) — v1 refuses honestly rather than partially
implementing membership change.

### 6.7 Clock discipline

Lease comparison is a cross-clock-domain operation. WCR1 requires
bounded clock skew: attach measures the peer offset and refuses beyond
`WCR1_MAX_CLOCK_SKEW_NS` (250 µs; PTP/chrony-class sync assumed). The
lease lower bound is `4 × max_skew + 2` (enforced at cfg validation).

## 7. Error ladder (Law 4 — every failure named)

36 distinct codes, every one reachable in the reference test matrix
(two are transport-level reservations for Engineer 2's layer):
`E_MAGIC, E_LAYOUT, E_CLUSTER_ID, E_CFG_CRC, E_CAPACITY, E_SLOT_SIZE,
E_ALIGN, E_REGION_SIZE, E_EPOCH, E_NODE_ID, E_NODE_EVICTED,
E_NODE_LIMIT, E_CLOCK_SKEW, E_SEQ_TORN, E_RETRY_EXHAUSTED (reserved),
E_NOT_PUBLISHED, E_SEQ_OVERRUN, E_SEQ_CORRUPT, E_BACKPRESSURE,
E_PAYLOAD, E_CRC, E_ARG, E_STATE, E_CONSENSUS_TORN, E_LEASE_EXPIRED,
E_NOT_LEADER, E_QUORUM_LOST, E_TERM_STALE, E_FENCED, E_VOTE_GRANTED,
E_ELECTION_BUSY (reserved), E_SPLIT_BRAIN, E_PARTITION (reserved),
E_TRANSPORT, E_NOMEM`. `wcr1_err_name()` returns the canonical name for
every code (never NULL, never "unknown" for a defined code).

## 8. Weft Core Laws — compliance

- **Law 1 (zero heap on the hot path):** all regions, peer tables and
  consensus scratch are allocated at ctx-create/attach through the
  engine alloc hooks; steady-state publish/acquire/tick/poll perform
  zero allocations — proven by hook counters across 100k ring ops
  (R4), 100k cluster ops (C10), and the full 44k-op torture (SB3).
- **Law 2 (bounded, deterministic):** every retry loop has a
  compile-time-visible bound; explicit LE wire formatting; 64-byte
  cacheline bracketing; virtual-clock elections are byte-deterministic
  (3× identical torture runs, evidence committed).
- **Law 3 (byte-frozen kernel):** `core/c/weft.{c,h}` untouched (empty
  diff vs origin/main); all cluster code under `core/c/cluster/`.
- **Law 4 (honest boundaries):** §7; the torture whitelist asserts no
  unexpected code ever escapes the engine.

## 9. Deployment prerequisites and honesty notes

- `mlock` may fail in unprivileged containers (rings still function;
  DMA pinning is a deployment prerequisite, observable via proc).
- Slot CRCs are opt-in; per-slot mandatory CRC is incompatible with the
  sub-microsecond budget (measured cost documented in D-31).
- Single producer per ring, one consumer thread per hosted ring (v1
  threading contract). MPSC/SPMC multi-thread coordination is Pillar 2
  (weft-tensor) local-ring territory.
- Cluster membership change (epoch bump, quorum resize) is out of scope
  for v1 — the epoch/CRC scaffolding and `E_EPOCH` refusal are in
  place; eviction ends in rejoin-by-new-epoch, refused honestly.
- The engine assumes aligned ≤8-byte single-field stores are
  atomic-on-arrival (x86-64 TSO, single DMA TLP); all cross-field
  consistency rides the seqlock/parity protocols, never field
  atomicity assumptions.

## 10. Reference implementation & test matrix

`core/c/cluster/`: `weft_cluster.h` (contract), `weft_cluster_ring.c`
(engine), `weft_cluster_consensus.c` (consensus),
`sim_cluster.{h,c}` (multi-node virtual-clock harness with partition
link matrix and gated transport). Tests: R1–R11 (ring: golden bytes,
ladder, two-store tears, backpressure, alias guards, fork IPC 100k),
C1–C10 (consensus: election, stability, quorum, fencing, heal, votes,
skew, eviction, seqlock, 100k zero-alloc), SB1–SB4 (duel, Byzantine
injection, 7-node 3000-round chaos with per-tick invariants and refusal
whitelist, crash recovery). CI: `ci/scripts/run_weft_cluster_shard.sh`
wired into `extreme-test.yml` (plain/ASAN/TSAN + scoreboard).

Performance envelope (protocol engine, local transport — D-31 for the
full scoreboard and methodology): two-store commit+read 3.9 ns,
acquire+consume 5.0 ns, publish 31 ns @128 B slots, full 5-node
cluster-sync 38 ns/message, stable consensus read 3.0 ns, election
convergence 23.75 ms virtual (deterministic) / µs-scale wall.
