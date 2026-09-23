// exporters/prometheus.js — embedded Prometheus exposition (text 0.0.4).
//
// Scrapes the lock-free registry WITHOUT stopping or locking the data plane
// (counter/histogram reads are single atomic loads). Serve with:
//   const srv = startMetricsServer(registry, { port: 9100 });
//   ... srv.close();
// Text rendering is scrape-time (cold) work only.

import http from 'node:http';
import { bucketUpperBound, HIST_BUCKETS } from '../metrics.js';

function labelsFor(reg, extra) {
  const l = reg.labelStr ? reg.labelStr + (extra ? ',' + extra : '') : (extra ?? '');
  return l ? `{${l}}` : '';
}

/** Render the registry into Prometheus text exposition format 0.0.4. */
export function renderPrometheus(registry) {
  const snap = registry.snapshot();
  const out = [];
  for (const c of snap.counters) {
    out.push(`# HELP ${c.name} ${c.help}`);
    out.push(`# TYPE ${c.name} counter`);
    out.push(`${c.name}${labelsFor(registry)} ${c.value}`);
  }
  for (const g of snap.gauges) {
    out.push(`# HELP ${g.name} ${g.help}`);
    out.push(`# TYPE ${g.name} gauge`);
    out.push(`${g.name}${labelsFor(registry)} ${g.value}`);
  }
  for (const h of snap.histograms) {
    out.push(`# HELP ${h.name} ${h.help}`);
    out.push(`# TYPE ${h.name} histogram`);
    let cum = 0;
    for (let i = 0; i < HIST_BUCKETS; i++) {
      cum += h.counts[i];
      const ub = bucketUpperBound(i);
      out.push(`${h.name}_bucket${labelsFor(registry, `le="${ub}"`)} ${cum}`);
    }
    out.push(`${h.name}_bucket${labelsFor(registry, 'le="+Inf"')} ${h.total}`);
    out.push(`${h.name}_count${labelsFor(registry)} ${h.total}`);
  }
  return out.join('\n') + '\n';
}

/**
 * Embedded /metrics endpoint. Returns a promise resolving to a handle with
 * `port`, `url` and `close()` (promisified). One plain HTTP server — no
 * dependency on the data plane beyond the registry's SharedArrayBuffer.
 */
export async function startMetricsServer(registry, opts = {}) {
  const server = http.createServer((req, res) => {
    if (req.url === '/metrics' || req.url === '/metrics/') {
      const body = renderPrometheus(registry);
      res.writeHead(200, { 'content-type': 'text/plain; version=0.0.4; charset=utf-8' });
      res.end(body);
      return;
    }
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('not found (try /metrics)\n');
  });
  const handle = { close: () => new Promise((res) => server.close(() => res())) };
  await new Promise((res) => {
    server.listen(opts.port ?? 0, opts.host ?? '127.0.0.1', () => {
      handle.port = server.address().port;
      handle.url = `http://127.0.0.1:${handle.port}/metrics`;
      res();
    });
  });
  return handle;
}
