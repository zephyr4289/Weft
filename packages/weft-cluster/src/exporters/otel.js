// exporters/otel.js — OpenTelemetry OTLP/JSON metrics snapshot.
//
// Emits a resourceMetrics document shaped for the OTLP/HTTP JSON mapping
// (application/json body of POST /v1/metrics): resource -> scopeMetrics ->
// metrics[] with sum / gauge / histogram payloads. Import into any OTLP
// collector; unit metadata keeps ns histograms readable in backends.

/**
 * Build an OTLP/JSON metrics payload from the registry snapshot.
 * @param {import('../metrics.js').MetricsRegistry} registry
 */
export function toOtelJson(registry, opts = {}) {
  const snap = registry.snapshot();
  const now = BigInt(Date.now()) * 1000000n; // unix ns
  const metrics = [];
  for (const c of snap.counters) {
    metrics.push({
      name: c.name, description: c.help, unit: c.unit ?? '1',
      sum: {
        dataPoints: [{
          attributes: Object.entries(snap.labels).map(([k, v]) =>
            ({ key: k, value: { stringValue: String(v) } })),
          asInt: String(c.value),
          timeUnixNano: String(now),
        }],
        aggregationTemporality: 2, // CUMULATIVE
        isMonotonic: true,
      },
    });
  }
  for (const g of snap.gauges) {
    metrics.push({
      name: g.name, description: g.help, unit: g.unit ?? '1',
      gauge: {
        dataPoints: [{
          attributes: Object.entries(snap.labels).map(([k, v]) =>
            ({ key: k, value: { stringValue: String(v) } })),
          asInt: String(g.value),
          timeUnixNano: String(now),
        }],
      },
    });
  }
  for (const h of snap.histograms) {
    // Bucket bounds follow the registry's 1, 1.5, 2, 3, 4, 6 ... sequence.
    const bounds = [];
    const bucketCounts = [];
    let cum = 0;
    const { bucketUpperBound, HIST_BUCKETS } = lazyBounds();
    for (let i = 0; i < HIST_BUCKETS; i++) {
      bounds.push(bucketUpperBound(i));
      cum += h.counts[i];
      bucketCounts.push(String(h.counts[i]));
    }
    metrics.push({
      name: h.name, description: h.help, unit: h.unit ?? 'ns',
      histogram: {
        dataPoints: [{
          attributes: Object.entries(snap.labels).map(([k, v]) =>
            ({ key: k, value: { stringValue: String(v) } })),
          timeUnixNano: String(now),
          count: String(h.total),
          min: '0',
          explicitBounds: bounds,
          bucketCounts,
          aggregationTemporality: 2,
        }],
      },
    });
  }
  return {
    resourceMetrics: [{
      resource: {
        attributes: [
          { key: 'service.name', value: { stringValue: opts.serviceName ?? 'weft-cluster' } },
          ...Object.entries(snap.labels).map(([k, v]) =>
            ({ key: k, value: { stringValue: String(v) } })),
        ],
      },
      scopeMetrics: [{
        scope: { name: '@weft/cluster', version: '0.1.0' },
        metrics,
      }],
    }],
  };
}

// tiny indirection so this module stays dependency-light and testable
function lazyBounds() {
  return {
    bucketUpperBound: (i) => {
      const b = (i / 2) | 0;
      const sub = i & 1;
      if (b === 0) return sub === 0 ? 1 : 1.5;
      return sub === 0 ? 2 ** b : 1.5 * 2 ** b;
    },
    HIST_BUCKETS: 96,
  };
}
