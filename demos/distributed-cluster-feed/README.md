# distributed-cluster-feed — 4-node real-time cluster demo (Pillar 3 D)

End-to-end demonstration of the weft-cluster managed plane: 1 producer node
streaming market-shaped frames into 3 consumer nodes with verified cross-node
zero-copy ingestion, live sequence-monotonicity checks, and one-way hop
latency — in two execution modes.

## Mode 1 — real UDP cluster (cross-process, the full managed stack)

```bash
node run.mjs                      # 4 processes, 6s @ 5k fps/topic (default)
node run.mjs --seconds 10 --fps 8000 --web   # + browser visualizer :8099
```

* 4 node processes (n1 producer, n2–n4 consumers), each with its own gossip
  socket, data socket, and Prometheus `/metrics` endpoint.
* Gossip membership converges over WGS1 datagrams; consumers learn the
  producer's data port from the membership table and attach (SUB control).
* Frames flow as WCN1 datagrams: `[u64 seq][u64 send ns]` payload, CRC-32,
  explicit little-endian everywhere.
* Terminal ANSI dashboard: per-node fps, totals, gaps, stale, hop p50/p99,
  rendezvous routes. Evidence JSON lands in `evidence/`.
* **Pass criterion:** every consumer sees strictly monotonic per-topic
  sequences after attach — zero gaps, zero stale.

Measured on the CI sandbox (5k fps/topic, 6 s): 57,000 frames produced per
topic, 59,400 ingested per consumer (every frame), **0 gaps, 0 stale**,
hop p50 ≈ 1.6 µs, p99 ≈ 8.4 µs (CLOCK_MONOTONIC one-way, cross-process).

## Mode 2 — mesh burst (the 1,000,000 frames/sec mandate number)

```bash
node mesh_burst.mjs --fps 1000000 --seconds 3
```

4 logical nodes over the LoopbackShm fabric — `SharedArrayBuffer` plays the
role of WCR1 registered memory; publishing is the same one-sided write into
each consumer's ring the RDMA substrate performs across machines. Frames are
ingested by direct zero-copy ring drains with per-(node,topic) sequence
verification on **every** frame.

Measured: **1,037,195 frames/sec sustained**, 3,111,584 fan-out writes/sec,
9,000,000 / 9,000,000 frames ingested, **0 sequence gaps**, hop p50 1.0 µs /
p99 2.0 µs. (Payload CRC is omitted in this mode to isolate memory-plane
throughput; CRC correctness is proven in the wire test suites.)

## Files

| path | role |
|------|------|
| `lib/node_app.mjs` | shared node runtime (roles, metrics, status lines) |
| `run.mjs` | UDP orchestrator: spawns 4 nodes, ANSI dashboard, evidence |
| `mesh_burst.mjs` | in-process 1M fps burst over the SAB fabric |
| `web/index.html` | browser visualizer (served by `run.mjs --web`) |
| `evidence/` | run artifacts (JSON) |
