# weft-cluster-cli

Operator CLI for the weft-cluster managed plane (`@weft/cluster`). No runtime
dependencies beyond the in-repo package.

```bash
node bin/weft-cluster.mjs join  --id 7 --gossip-port 5101 --seed 127.0.0.1:5102
node bin/weft-cluster.mjs status --seed 127.0.0.1:5102 --routes telemetry,trades
node bin/weft-cluster.mjs bench --seconds 3
```

- **join** — run a cluster node: gossips heartbeats, prints membership-view
  changes, Ctrl-C announces LEAVING (graceful leave propagation).
- **status** — passively observe the gossip mesh for a window and dump the
  membership table (node ids, ports, incarnation, freshness) plus rendezvous
  topic->owner routes.
- **bench** — self-contained loopback-shm pub/sub burst with achieved fps and
  publish->consume hop latency (p50/p99) as one JSON line (CI-friendly).
