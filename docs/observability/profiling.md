# Profiling Weft in Production — flame graphs, perf gates, and live metrics

Issue: [#20 — Tier 5: Observability & Tooling](https://github.com/zephyr4289/Weft/issues/20)
Covers: task 2 (Prometheus/Grafana), task 3 (flame graphs), and how they
compose with the task 4 perf-regression gate.

---

## 1. Live metrics (Prometheus + Grafana)

`core/c/metrics-dump` renders the kernel's telemetry plus a LIVE measured
publish/claim latency distribution in the Prometheus TEXT exposition
format. `tools/prometheus/weft_exporter.py` (stdlib-only) scrapes it and
serves `:9108/metrics`; `tools/prometheus/grafana-dashboard.json` panels
exactly these series:

| Metric | Type | Source |
|--------|------|--------|
| `weft_publish_total` | counter | kernel `t_publish` |
| `weft_claim_total` | counter | kernel `t_claim` |
| `weft_dropped_total` | counter | kernel `t_drop` (DROPPED_REVOKED) |
| `weft_invalid_total` | counter | kernel `t_invalid` (TIER4 validation wall) |
| `weft_reclaim_timeouts_total` | counter | kernel `t_reclaim_timeouts` (TIER4 bounded revocation) |
| `weft_publish_latency_us{quantile}` | summary | measured over 20k live ops |
| `weft_claim_latency_us{quantile}` | summary | measured over 20k live ops |

Run it:

```bash
make -C core/c metrics-dump
python3 tools/prometheus/weft_exporter.py --port 9108   # stdlib only
# Grafana: import tools/prometheus/grafana-dashboard.json
```

Bring your own app: the exporter accepts `WEFT_METRICS_CMD` — point it at
any wrapper that renders YOUR kernel's counters in the same exposition
format (JNI/Dart/Swift hosts read the same `t_*` atomics through the C
kernel's telemetry getters).

Honesty: the latency quantiles describe the exporter's measured workload
on that machine (`host` label; set `WEFT_METRICS_HOST` to pin it). They
are a live heartbeat, not a benchmark claim — the benchmark claims live in
the perf-regression baselines (§3).

## 2. Flame graphs (publish/claim hot spots)

```bash
tools/flamegraph/weft-flame.sh linux 10   # perf record + fold + SVG
tools/flamegraph/weft-flame.sh macos 10   # sample(1) call tree
```

- Linux path records at 997 Hz over the B1 bench cell (the same
  short-burst publish/claim workload the perf gate pins), with
  `-fno-omit-frame-pointer` builds so stacks are complete without DWARF.
- With Brendan Gregg's FlameGraph tools on PATH (or in `./FlameGraph/`),
  you get `bench/flame/weft-flame.svg`; without them, the folded stacks
  land in `bench/flame/perf-script.txt`, importable into speedscope /
  FlameScope.
- What to look for: the hot path should be `weft_publish` → envelope
  encode + canary + `atomic_exchange` and `weft_r_claim` →
  `atomic_exchange` — nothing else. Anything else on the flame (memcpy
  from a fill path, allocator frames, a litmus pattern helper) is a
  caller-side cost, and the flame tells you whose it is.

The measured reference shape on the CI sandbox: publish P99 ≈ 0.35 µs,
claim P99 ≈ 0.04 µs (256 B payloads) — dominated by the single AcqRel
exchange, exactly as the protocol predicts.

## 3. The perf-regression gate (companion)

`ci/scripts/run_perf_regression_shard.sh` fails CI on regressions vs the
pinned baselines (`ci/baselines/`): the W-suite P99 frame-rate cells and
the publish/claim P99 latency cells from `p99-bench`. Threshold: **5%**
(issue #20's bar). Baseline updates go through the `perf-baseline-update`
label — a PR that moves the baseline is a PR that declares why.

## 4. Traces for the post-mortem

When the flame and the metrics tell you *that* something regressed but not
*what the kernel decided*, capture a `.weftrec` v4 trace (RFC 0014) and
read the decision stream: publish/claim/drop/revoke/ack per event, CRC-
protected, schema-validated JSON export for the Inspector. See
`docs/security/tier4-hardening.md` §3 for the canary side and
`rfcs/0014-weftrec-trace-standard.md` for the format.
