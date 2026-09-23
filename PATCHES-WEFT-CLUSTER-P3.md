# PATCHES-WEFT-CLUSTER-P3 — Pillar 3 Scorecard (Project weft-cluster)

Engineer 3 · branch `feat/weft-cluster-managed` · base: `feat/weft-tensor-managed` tip
CI: `ci/scripts/run_weft_cluster_shard.sh` — **ALL 8 STAGES GREEN** · Report: `reports/D-33-WEFT-CLUSTER-MANAGED.md`

| # | Commit | Deliverable | Tests / gates |
|---|--------|-------------|---------------|
| P1 | `feat(weft-cluster): WCN1/WGS1 wire format + error taxonomy + crc32` | docs/weft-cluster/WIRE-V1.md (NORMATIVE) + wire.js + errors.js + crc32.js | 9 tests: LE layout vectors, 7-case malformed taxonomy, WGS1 round-trip, FNV/CRC reference vectors (0xCBF43926) |
| P2 | `feat(weft-cluster): ClusterClient + loopback-shm/UDP transports + async iterators` | ShmRing/ShmFabric/LoopbackShmTransport/UdpTransport (RX ring + TX ring) + ClusterClient/Subscription + poke wake path | 6 tests: shm pub/sub monotonicity, fanout+unsubscribe, tryLatest, stale-replay drop counting, live UDP e2e with SUB control, silent no-sub; probes: publish +6.6 KB / subscribe −22.6 KB @ 100k |
| P3 | `feat(weft-cluster): topology manager + rendezvous ShardRouter` | MembershipTable (SAB seqlock + open-addressed index, plain-load fast path + strict variant) + GossipEngine (SWIM: incarnation merges, sticky LEAVING, refutation, suspect/evict) + ShardRouter (exact double-arithmetic FNV-1a 64) + ClusterMesh | 14 tests: 3-engine convergence, 40-node collision soak, refutation, eviction clocks, graceful leave, determinism, balance <3×, minimal disruption 5–20%, unrouted; bench evidence: lookup p50 13.5 ns / p99 21.1 ns |
| P4 | `feat(weft-cluster): zero-overhead observability plane` | MetricsRegistry (Atomics-only hot path, 1.5×-split octave histograms) + renderPrometheus + embedded /metrics + toOtelJson + buildPftrace/writePftrace (hand-rolled protobuf) | 6 tests: quantile sanity, bound-sequence monotonicity, exposition format with cumulative buckets, OTLP shape, .pftrace protobuf round-trip via in-suite parser, real HTTP scrape 200 |
| P5 | `feat(weft-cluster): python/weft_cluster SDK + byte-exact cross-language parity` | client.py (UDP + pump + replay filter), ring.py (TS-layout ShmRing, mmap/shared_memory attach), streams.py (asyncio), topology.py (rendezvous), NumPy/PyTorch hooks + Frame.__dlpack__ | 16 pytest + live UDP both directions (TS→Py, Py→TS); mmap aliasing proof by mutation; fixtures: wcn1_vectors.json + router_vector.json decoded byte-exactly in BOTH languages |
| P6 | `feat(weft-cluster-cli): weft cluster join/status/bench` | bin/weft-cluster.mjs + package.json + README | bench: 1,139,121 fps loopback-shm (batched backpressure), gaps 0 / stale 0 |
| P7 | `demo(distributed-cluster-feed): 4-node UDP cluster demo + 1.04M fps mesh burst + web visualizer` | run.mjs (orchestrator + ANSI dashboard) + lib/node_app.mjs + mesh_burst.mjs + web/index.html + evidence/ | UDP: 57,000 frames/topic, every frame ingested ×3 consumers, 0 gaps / 0 stale, hop p50 1.6 µs; burst: 1,037,195 fps, 9,000,000/9,000,000, 0 gaps, hop p50 1.0 µs |
| P8 | `ci(weft-cluster): 8-stage fail-closed shard` | ci/scripts/run_weft_cluster_shard.sh | 8/8 stages green (Law 3 integrity, TS+Py parity, suites, live cross-language UDP, 4 alloc probes, demo monotonicity + ≥1M fps + lookup gates) |
| P9 | `docs(report): D-33 + this scorecard` | reports/D-33-WEFT-CLUSTER-MANAGED.md | — |

## Law enforcement matrix (mechanical, not aspirational)

| Law | Mechanism | Where |
|---|---|---|
| 1 zero-alloc | one staging datagram per client; one FrameHandle per subscription bound at subscribe; Atomics-cell metrics; version-memoized routing; reusable gossip handles; `--expose-gc` probes ×4 modes with 64 KiB gate | client.js, wire.js, metrics.js, router.js, shard stage 7 |
| 2 LE + determinism | explicit-LE primitives on every access in both languages; frozen WCN1 vector + 6 malformed vectors + FNV/CRC vectors decoded byte-exactly in TS AND Python; rendezvous owner vector frozen | wire.test.mjs, crosslang.test.mjs, test_cluster_wire.py, test_cluster_router.py, shard stages 2–3 |
| 3 byte-frozen kernel | shard stage 1 diffs core/c/weft.{c,h} against merge-base; all managed code under packages/, python/, tools/, demos/, docs/ | run_weft_cluster_shard.sh stage 1 |
| 4 boundary validation | byte-frozen 13-code taxonomy mirrored in both languages; hot-path decoders return codes (never throw); stale/torn/saturation/overrun counted events; demo exits 2 on monotonicity violation | errors.js/errors.py, wire tests, udp tests, run.mjs |

## Probe-proven pitfalls recorded for the record

1. **libuv defers UDP sends** — a shared staging buffer mutated between
   `send()` calls transmits the last frame's bytes N times. Fixed by the
   per-in-flight-datagram TX ring with counted drop-tail.
2. **Default 212 KB kernel RX buffer silently drops bursts** — surfaced only
   as Law 4 seq gaps in the demo; fixed by provisioning a 4 MB receive buffer.
3. **Node Buffer pooling inflates a bound view's `avail` window** — hex
   fixtures bound to a pooled Buffer decode against the whole 8 KB pool; the
   crosslang helpers slice exact ArrayBuffers (API pitfall documented in test).
4. **Iterator stale-check anti-pattern** — a seq-equality check in the
   subscription iterator flags every frame stale on datagram paths because the
   pump updates `last_seq` before the slot read; ring commit words already
   order frames, so the check lives only in the transport replay filter.
