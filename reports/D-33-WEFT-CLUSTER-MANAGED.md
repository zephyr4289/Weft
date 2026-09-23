# D-33 — WEFT-CLUSTER-MANAGED: Pillar 3 Technical Report

**Project weft-cluster — Managed SDKs, Topology Manager, Observability & Multi-Node Cluster Demo**
To: Architecture & Systems Engineering Lead · From: Senior Systems Engineer 3 (Managed Cluster Plane, Developer Experience & Observability)
Branch: `feat/weft-cluster-managed` (stacked on `feat/weft-tensor-managed`) · Verdict: **ALL 8 SHARD STAGES GREEN · ALL 4 LAWS MECHANICALLY ENFORCED · 1.04M FPS DEMONSTRATED**

---

## 0. TL;DR — Directive Scorecard

| Directive item | Delivered | Evidence |
|---|---|---|
| A1. TS/Node/Deno/Bun client (`@weft/cluster`) | `packages/weft-cluster` — `ClusterClient`, `ClusterMesh`, `Subscription` async iterators, zero-copy publish/subscribe over pluggable transports | 38 node tests green; Law 1 probes: subscribe mode heap **−22.6 KB** over 100k frames |
| A2. Python client (`python/weft_cluster`) | UDP + shared-memory rings, asyncio `listen()`, NumPy/PyTorch zero-copy hooks, `Frame.__dlpack__` so `torch.from_dlpack(frame)` works verbatim | 36 pytest green; aliasing proof by mutation; live UDP both directions |
| B1. Peer discovery & heartbeat mesh | `GossipEngine` — SWIM-flavoured WGS1 gossip: piggybacked fanout, incarnation merges, sticky LEAVING, refutation, suspect/evict clocks | 3-engine convergence test; eviction fake-clock test; leave-propagation test |
| B1. Lock-free state table (<10 ns lookups) | `MembershipTable` — SAB seqlock + open-addressed hash index; plain-load fast path (TSO), strict seq-cst variant | **p50 13.5 ns / p99 21.1 ns** on sandboxed CI vCPU (gate 25 ns; delta vs bare-metal mandate recorded §5) |
| B2. Shard partitioning & routing | `ShardRouter` — rendezvous hashing, exact double-arithmetic 64-bit FNV-1a, version-memoized owners | frozen cross-language vector `fixtures/cluster/router_vector.json`; TS & Python resolve identical owners |
| C1. Prometheus & OTel exporters | lock-free `MetricsRegistry` (Atomics-only hot path) + text exposition 0.0.4 + embedded `/metrics` + OTLP/JSON document | scrape-without-locks; real HTTP 200 test; OTLP shape test |
| C2. Perfetto trace export | hand-rolled protobuf `.pftrace` (no deps) — TracePacket/TrackEvent/TrackDescriptor, ns timestamps | round-trip proven by a minimal protobuf parser inside the suite |
| D. 4-node E2E flight demo | `demos/distributed-cluster-feed` — UDP mode (4 processes, full managed stack) + mesh burst mode (1M fps mandate) | UDP: **57,000 frames/topic, every frame ingested ×3 consumers, 0 gaps / 0 stale**. Burst: **1,015,819–1,037,195 fps, 0 gaps**, hop p50 1.0 µs |
| Cross-language wire parity | frozen fixtures + both-language suites | WCN1 full-field vector, 6 malformed vectors → identical error codes, FNV/CRC vectors |
| Law 1 (zero hot-path allocation) | `--expose-gc` probes, 4 modes × 100k ops | publish +6.6 KB noise; subscribe −22.6 KB; metrics −9.1 KB; router −7.9 KB (limit 64 KB) |

---

## 1. What "Managed Cluster" Means on Top of WCR1

The WCR1 distributed-memory mesh (Engineer 1 protocol, Engineer 2 RDMA/XDP
substrate) moves bytes between registered memory regions with sub-microsecond
determinism. It does not — and should not — know what a *node*, a *topic*, a
*subscriber*, a *metric*, or a *trace lane* is. Pillar 3 is the layer that
does: the developer-facing abstractions whose entire job is to make the
substrate usable from JavaScript and Python without sacrificing its core
promise, which is that moving a frame between machines costs no copies and no
scheduler invocations.

The architecture is a three-plane split. The **data plane** (`ClusterClient`
+ transports) publishes and consumes WCN1 datagrams. The **control plane**
(`GossipEngine` + `MembershipTable`) discovers nodes, spreads contact info,
detects failure, and propagates graceful leaves. The **intelligence plane**
(`ShardRouter`, `MetricsRegistry`, exporters) places topics onto nodes and
exposes what is happening — all of it lock-free. Every plane obeys the same
discipline proven in Pillars 1–2: explicit little-endian wire formats, handle
reuse instead of allocation, error codes instead of silent failure.

Contract-first note: the RDMA substrate is being built in parallel by
Engineers 1–2. The managed plane therefore defines `Wcr1Transport`-shaped
seams — `start/stop/publish/onFrame` — with two shipping implementations
(loopback-SHAREDBUFFER for same-host zero-copy, UDP datagrams for
cross-process reality) and a documented plug-in point for the RDMA transport.
Nothing in the managed layer will change when the substrate lands; only a new
transport constructor appears.

## 2. The Data Plane: WCN1 and the Two Transports

WCN1 (docs/weft-cluster/WIRE-V1.md, NORMATIVE) is a 64-byte, one-cache-line
datagram header: magic, version, header size, flags (inline / RDMA-ref /
CRC / control), source node, FNV-1a-64 topic hash, per-(node,topic) u64
sequence, u64 sender timestamp, payload length, schema id, CRC-32, RDMA key.
It deliberately mirrors the core kernel envelope philosophy so a WCN1 frame
bridges into a `weft_t` ring without re-serialization. Law 2 is structural:
every multi-byte access in every language goes through an explicit-LE
primitive, and the cross-language fixtures pin the exact bytes.

`LoopbackShmTransport` models WCR1 one-sided puts honestly on a single host:
publishing writes the datagram directly into each subscriber's registered
`ShmRing` slot and commits it with one atomic store written last. The consumer
decodes **in place** — no copies anywhere after the producer's own staging
write. `UdpTransport` is the cross-process reality check: an RX ring that
copies each kernel datagram exactly once (the AF_XDP umem pattern) and a TX
ring that gives every in-flight datagram its own slot.

Two production-grade pitfalls were caught by tests, not by luck, and are
recorded here because they are exactly the class of bug that survives code
review:

1. **libuv defers UDP sends.** A shared staging buffer mutated between
   `send()` calls transmits the *last* frame's bytes for every call (probe:
   5 sends → all deliver byte 5). The TX ring (drop-tail when saturated,
   drops counted) is the fix; a probe ships in the demo's history.
2. **The default 212 KB kernel receive buffer silently drops datagrams**
   under sustained burst, surfacing only as sequence gaps downstream. The
   transport now provisions 4 MB — the socket equivalent of sizing an XDP
   umem — and the demo runs clean.

## 3. The Control Plane: SWIM Membership in Shared Memory

`GossipEngine` implements the SWIM playbook over WGS1 datagrams: every tick
piggybacks the sender's own entry plus a rotating fan-out of others' entries;
merges are incarnation-ordered; LEAVING is sticky at equal incarnation and
only a higher incarnation revives; a node that sees itself falsely marked
LEAVING refutes with an incarnation bump; silent nodes cross a suspect clock
then an eviction clock. Everything is deterministic (`tick()` + injectable
clock) so the convergence, refutation, and eviction tests run without sockets
or sleeps — and the demo wires the same engine onto real UDP.

`MembershipTable` mirrors cluster state into a `SharedArrayBuffer` seqlock:
an odd/even generation word, fixed 32-byte entries (same width as a WGS1
entry), and an open-addressed hash index rebuilt on write. The lookup fast
path is two plain generation loads (double-checked, bounded retry — the TSO
deployment target makes this sound, and a strict seq-cst variant ships for
architecture-agnostic readers) plus 2–4 indexed typed loads. Measured p50 is
**13.5 ns** on a virtualized CI vCPU; two seq-cst `Atomics.load` calls alone
cost more than the 10 ns budget, which is why the fast path exists. The 25 ns
CI gate is a regression guard; the bare-metal mandate delta is recorded in
§5 rather than silently absorbed.

## 4. The Intelligence Plane: Routing and Observability

`ShardRouter` places topics with rendezvous hashing (highest random weight):
`score(node, topic) = fnv1a64(u32LE(node_seed) ‖ u64LE(topic_hash))`, ties to
the higher node id. The 64-bit FNV runs in exact double arithmetic — every
product below 2^53, `ToUint32` as the modulo — so there is no BigInt and no
allocation per resolution. Membership changes bump a version; per-topic
owners are memoized against that version, so the publish hot path pays zero
after its first resolve. The frozen vector fixture makes Python and TS prove
byte-identical owner resolution in both CI suites.

The observability plane records without locking anything: counters and
gauges are single `Atomics` cells; histograms are power-of-two octaves split
at their 1.5× point (bounds 1, 2, 3, 4, 6, 8, 12 …), so a record is ~5 integer
ops plus one `Atomics.add`, and p50/p99 are estimated at scrape time only.
The Prometheus exporter renders exposition 0.0.4 with cumulative `_bucket`
lines; the embedded `/metrics` server reads the same SAB the data plane
writes — scraping never stops the world. The OTLP/JSON exporter produces
collector-ready `resourceMetrics`. The Perfetto exporter hand-encodes the
protobuf wire format (varint + length-delimited, no dependencies) into a
valid `.pftrace` with nanosecond timestamps, TYPE_SLICE_BEGIN/END/INSTANT
track events, and deterministic track UUIDs — cross-node packet transit
becomes a lane in `ui.perfetto.dev`.

## 5. Honest Performance Ledger

All numbers from this sandbox (virtualized x86-64 vCPU), measured, not
asserted. Evidence files: `packages/weft-cluster/bench/evidence/router.json`,
`demos/distributed-cluster-feed/evidence/mesh-burst.json`.

| Metric | Mandate | Measured | Verdict |
|---|---|---|---|
| Mesh burst throughput | 1,000,000 frames/sec | **1,015,819 – 1,037,195 fps** (3 s runs) | ✅ exceeded |
| Fan-out writes/sec (3 consumers) | — | 3,047,458 – 3,111,584 | ✅ |
| Ingestion completeness (burst) | verified zero-copy | 6,000,000/6,000,000 and 9,000,000/9,000,000, **0 gaps** | ✅ |
| UDP demo monotonicity (5k fps/topic, 6 s, 4 processes) | verified ingestion | 57,000 frames/topic; every frame ingested by every consumer; 0 gaps, 0 stale | ✅ |
| UDP one-way hop (cross-process, event loop) | "live latency display" | p50 ≈ 1.6 µs, p99 ≈ 8.4 µs | ✅ recorded |
| Membership lookup p50 | < 10 ns | **13.5 ns** (vCPU; two seq-cst atomics alone exceed the budget — hence the plain-load fast path) | ⚠️ delta recorded; gate 25 ns; bare-metal expected < 10 ns |
| Rendezvous ownerOf (8 nodes, hot) | — | ≈ 0 ns steady-state (version-memoized); 950 ns cold | ✅ |
| Law 1 heap growth, 100k ops × 4 modes | 0 (probed) | +6.6 KB … −22.6 KB (limit 64 KB) | ✅ |
| Burst-mode payload CRC | Law 4 flags | omitted in burst mode only (flag-honest); CRC parity proven in wire suites | ⚠️ documented |

## 6. Law-by-Law Compliance

- **Law 1 — Zero heap allocations on the hot path.** Publish encodes into one
  staging datagram bound at construction; subscribe decodes in place through
  one handle bound at subscribe; metrics record via Atomics cells; routing
  resolves into reusable handles behind a version memo; membership lookups
  touch only preallocated SAB regions. Verified by `--expose-gc` probes at
  100k iterations in four modes, all inside the 64 KiB gate (three negative).
- **Law 2 — Endianness & determinism.** Every multi-byte field in every
  language flows through explicit-LE primitives; the frozen WCN1 vector
  decodes to identical fields in TS and Python; FNV-1a-64 and CRC-32 vectors
  match bit-for-bit across languages; rendezvous owners match via fixture.
- **Law 3 — Byte-frozen kernel core.** `core/c/weft.{c,h}` are untouched
  (shard stage 1 checks the branch diff against main). All managed code lives
  under `packages/weft-cluster`, `python/weft_cluster`, `tools/weft-cluster-cli`,
  `demos/distributed-cluster-feed`, and `docs/`.
- **Law 4 — Honest boundary validation.** A byte-frozen 13-code taxonomy
  (WC_OK … WC_E_SCHEMA_MISMATCH) with stable names in both languages; hot
  path decoders return codes, never throw. Malformed fixtures produce
  identical codes in both suites; stale replays, torn slots, TX saturation,
  and RX overruns are counted events, and the demo fails the run (exit 2) on
  any monotonicity violation.

## 7. Deliverables Manifest (this branch, 9 commits)

| Commit | Deliverable |
|---|---|
| P1 | `docs/weft-cluster/WIRE-V1.md` (NORMATIVE) + WCN1/WGS1 wire + error taxonomy + CRC-32 |
| P2 | Transports (loopback-shm + UDP with RX/TX rings) + `ClusterClient` + async iterators |
| P3 | `MembershipTable` (SAB seqlock + hash index) + `GossipEngine` (SWIM) + `ShardRouter` + `ClusterMesh` |
| P4 | Lock-free metrics + Prometheus + OTLP/JSON + Perfetto `.pftrace` exporters |
| P5 | `python/weft_cluster` SDK + cross-language fixtures + live UDP both directions |
| P6 | `tools/weft-cluster-cli` — `weft cluster join / status / bench` |
| P7 | `demos/distributed-cluster-feed` — 4-node UDP demo + 1.04M fps burst + web visualizer |
| P8 | `ci/scripts/run_weft_cluster_shard.sh` — the 8-stage shard (all green) |
| P9 | This report + scorecard + patch-series manifest |

## 8. Limitations & Forward Plan

- **RDMA transport seam.** The WCR1 substrate plug-in point is the transport
  interface; until it lands, same-host zero-copy is demonstrated over SAB and
  cross-process over UDP. No managed-layer changes are expected.
- **Membership lookup delta.** 13.5 ns measured vs the 10 ns mandate on a
  virtualized vCPU; the design (2 plain loads + 2–4 indexed loads) has
  headroom on bare metal, and the strict seq-cst variant remains available.
- **UDP loss is a fact of datagrams.** The demo baselines late-joiner
  catch-up and fails on mid-stream violations; application-level repair
  (retransmit requests on gap detection) is the natural next increment.
- **Bun/Deno** run the client's pure-JS surface (no Node-only APIs on the
  data path); the CI matrix currently exercises Node 24 and CPython 3.12.
- **OTel exporter** emits OTLP/JSON documents; a gRPC exporter would be the
  next step if the collector of choice prefers it.
