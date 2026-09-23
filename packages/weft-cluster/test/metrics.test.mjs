// metrics.test.mjs — lock-free registry, Prometheus text, OTLP JSON,
// Perfetto .pftrace protobuf round-trip, and the embedded /metrics server.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MetricsRegistry, HIST_BUCKETS, bucketUpperBound } from '../src/metrics.js';
import { renderPrometheus, startMetricsServer } from '../src/exporters/prometheus.js';
import { toOtelJson } from '../src/exporters/otel.js';
import { buildPftrace } from '../src/exporters/perfetto.js';

test('registry: counters, gauges, histogram quantile sanity', () => {
  const reg = new MetricsRegistry({ labels: { node: '7' } });
  const c = reg.counter('weft_cluster_frames_total', 'Frames published.');
  for (let i = 0; i < 1000; i++) c.add(1);
  const g = reg.gauge('weft_cluster_ring_depth', 'Slots in flight.');
  g.set(42);
  const h = reg.histogram('weft_cluster_hop_latency_ns', 'One-way hop.');
  // Uniform 1..1000 ns — p50 should land near 500, p99 near 990 (bucket error
  // bounded by the 1.5x sub-level split).
  for (let i = 1; i <= 1000; i++) h.record(i);
  const snap = reg.snapshot();
  assert.equal(snap.counters[0].value, 1000);
  assert.equal(snap.gauges[0].value, 42);
  const hist = snap.histograms[0];
  assert.equal(hist.total, 1000);
  assert.ok(hist.p50 >= 400 && hist.p50 <= 750, `p50 ${hist.p50} in [400,750]`);
  assert.ok(hist.p99 >= 900 && hist.p99 <= 1500, `p99 ${hist.p99} in [900,1500]`);
  assert.ok(hist.p99 >= hist.p50);
});

test('histogram bucket bounds follow the 1, 2, 3, 4, 6, 8, 12... sequence', () => {
  assert.equal(bucketUpperBound(0), 1);
  assert.equal(bucketUpperBound(1), 2);
  assert.equal(bucketUpperBound(2), 3);
  assert.equal(bucketUpperBound(3), 4);
  assert.equal(bucketUpperBound(4), 6);
  assert.equal(bucketUpperBound(5), 8);
  assert.equal(bucketUpperBound(6), 12);
  assert.equal(bucketUpperBound(7), 16);
  // Monotone across the whole table.
  let prev = 0;
  for (let i = 0; i < HIST_BUCKETS; i++) {
    assert.ok(bucketUpperBound(i) > prev);
    prev = bucketUpperBound(i);
  }
});

test('prometheus: exposition format with cumulative histogram buckets', () => {
  const reg = new MetricsRegistry({ labels: { node: '3' } });
  reg.counter('weft_cluster_frames_total', 'Frames published.').add(5);
  reg.histogram('weft_cluster_hop_latency_ns', 'One-way hop.');
  for (let i = 1; i <= 100; i++) reg._hists[0].record(300); // all in one bucket
  const text = renderPrometheus(reg);
  assert.match(text, /# HELP weft_cluster_frames_total Frames published\./);
  assert.match(text, /# TYPE weft_cluster_frames_total counter/);
  assert.match(text, /weft_cluster_frames_total\{node="3"\} 5/);
  assert.match(text, /# TYPE weft_cluster_hop_latency_ns histogram/);
  // Cumulative buckets are monotone and end with +Inf == total.
  const buckets = [...text.matchAll(/weft_cluster_hop_latency_ns_bucket\{[^}]*le="([^"]+)"\} (\d+)/g)];
  assert.ok(buckets.length === HIST_BUCKETS + 1);
  const values = buckets.map((m) => parseInt(m[2], 10));
  for (let i = 1; i < values.length; i++) {
    assert.ok(values[i] >= values[i - 1], 'cumulative monotone');
  }
  assert.equal(values[values.length - 1], 100);
});

test('otel: OTLP/JSON document shape (resourceMetrics -> metrics)', () => {
  const reg = new MetricsRegistry({ labels: { node: '9' } });
  reg.counter('weft_cluster_frames_total', 'Frames.').add(11);
  reg.gauge('weft_cluster_ring_depth', 'Depth.').set(3);
  const h = reg.histogram('weft_cluster_hop_latency_ns', 'Hop.');
  h.record(1000);
  const doc = toOtelJson(reg);
  const rm = doc.resourceMetrics[0];
  assert.equal(rm.scopeMetrics[0].scope.name, '@weft/cluster');
  const names = rm.scopeMetrics[0].metrics.map((m) => m.name);
  assert.deepEqual(names, ['weft_cluster_frames_total', 'weft_cluster_ring_depth',
    'weft_cluster_hop_latency_ns']);
  assert.equal(rm.scopeMetrics[0].metrics[0].sum.dataPoints[0].asInt, '11');
  assert.equal(rm.scopeMetrics[0].metrics[0].sum.isMonotonic, true);
  const hp = rm.scopeMetrics[0].metrics[2].histogram.dataPoints[0];
  assert.equal(hp.count, '1');
  assert.equal(hp.bucketCounts.length, 96);
  assert.equal(hp.explicitBounds.length, 96);
});

// --- minimal protobuf reader for the .pftrace round-trip test ---------------
function readVarint(buf, pos) {
  let result = 0n, shift = 0n;
  for (;;) {
    const b = buf[pos++];
    result |= BigInt(b & 0x7f) << shift;
    if ((b & 0x80) === 0) return [result, pos];
    shift += 7n;
  }
}
function parseFields(buf, start, end) {
  const fields = [];
  let pos = start;
  while (pos < end) {
    let tag;
    [tag, pos] = readVarint(buf, pos);
    const field = Number(tag >> 3n), wt = Number(tag & 7n);
    if (wt === 0) { let v; [v, pos] = readVarint(buf, pos); fields.push({ field, wt, v }); }
    else if (wt === 2) {
      let len;
      [len, pos] = readVarint(buf, pos);
      fields.push({ field, wt, start: pos, end: pos + Number(len) });
      pos += Number(len);
    } else throw new Error(`wiretype ${wt} not handled`);
  }
  return fields;
}

test('perfetto: .pftrace protobuf round-trips packets and track events', () => {
  const events = [
    { ts: 1000n, dur: 500n, name: 'publish->consume', track: 'node1->node2' },
    { ts: 2000n, dur: 0, name: 'gossip-tick', track: 'gossip' },
    { ts: 3000n, dur: 250n, name: 'metrics-scrape', track: 'node1->node2' },
  ];
  const bytes = buildPftrace(events);
  const fields = parseFields(bytes, 0, bytes.length);
  // Every top-level field must be packet (1).
  assert.ok(fields.every((f) => f.field === 1 && f.wt === 2));
  const packets = fields;
  // BEGIN/END pairing: 2 slice packets + 1 instant + 3 track descriptors.
  let begins = 0, ends = 0, instants = 0, descriptors = 0;
  const names = [];
  const timestamps = [];
  for (const pk of packets) {
    const inner = parseFields(bytes, pk.start, pk.end);
    let hasTrackEvent = false, hasDescriptor = false;
    for (const f of inner) {
      if (f.field === 8 && f.wt === 0) timestamps.push(f.v);
      if (f.field === 11 && f.wt === 2) {
        hasTrackEvent = true;
        for (const t of parseFields(bytes, f.start, f.end)) {
          if (t.field === 9) {
            if (t.v === 1n) begins++;
            else if (t.v === 2n) ends++;
            else if (t.v === 3n) instants++;
          }
          if (t.field === 23 && t.wt === 2) {
            names.push(new TextDecoder().decode(bytes.subarray(t.start, t.end)));
          }
        }
      }
      if (f.field === 60 && f.wt === 2) hasDescriptor = true;
    }
    if (hasTrackEvent) hasDescriptor = false;
    if (hasDescriptor) descriptors++;
  }
  assert.equal(begins, 2);
  assert.equal(ends, 2);
  assert.equal(instants, 1);
  assert.equal(descriptors, 2, 'two distinct tracks');
  assert.ok(names.includes('publish->consume'));
  assert.ok(names.includes('gossip-tick'));
  // Timestamps decode as BigInt ns and stay ordered.
  assert.ok(timestamps.length >= 5);
  assert.deepEqual([...timestamps].sort((a, b) => (a < b ? -1 : 1)), timestamps);
});

test('prometheus: embedded /metrics server answers 200 over real HTTP', async () => {
  const reg = new MetricsRegistry({ labels: { node: '1' } });
  reg.counter('weft_cluster_frames_total', 'Frames.').add(777);
  const srv = await startMetricsServer(reg);
  try {
    const res = await fetch(srv.url);
    assert.equal(res.status, 200);
    assert.match(res.headers.get('content-type'), /text\/plain/);
    const text = await res.text();
    assert.match(text, /weft_cluster_frames_total\{node="1"\} 777/);
    const miss = await fetch(srv.url.replace(/metrics$/, 'nope'));
    assert.equal(miss.status, 404);
  } finally {
    await srv.close();
  }
});
