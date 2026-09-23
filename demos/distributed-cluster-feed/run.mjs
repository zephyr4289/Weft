#!/usr/bin/env node
// run.mjs — 4-node distributed cluster feed demo (deliverable D4).
//
// Spawns 4 node processes on localhost UDP (1 producer + 3 consumers, each
// with its own gossip+data socket and Prometheus /metrics endpoint), waits
// for gossip convergence, then renders a live ANSI terminal dashboard:
// per-node fps, sequence-monotonicity verification, and one-way hop latency
// (p50/p99, CLOCK_MONOTONIC cross-process). Writes evidence JSON at the end
// and FAILS (exit 2) if any consumer observed sequence gaps/stale frames.
//
//   node run.mjs                       # 10 s at 25k fps/topic
//   node run.mjs --seconds 15 --fps 50000
//   node run.mjs --web                 # also start the browser visualizer

import process from 'node:process';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const arg = (name, dflt) => {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : dflt;
};
const has = (name) => process.argv.includes(`--${name}`);

const NODES = [
  { id: 1, role: 'producer' },
  { id: 2, role: 'consumer' },
  { id: 3, role: 'consumer' },
  { id: 4, role: 'consumer' },
];
const BASE_GOSSIP = parseInt(arg('gossip-base', '5601'), 10);
const seconds = parseFloat(arg('seconds', '8'));
const fps = parseInt(arg('fps', '5000'), 10);
const web = has('web');

const G = (s) => `\x1b[${s}m`;
const CLEAR = '\x1b[2J\x1b[H';
const BOLD = G('1'), DIM = G('2'), GREEN = G('32'), YELLOW = G('33'),
  CYAN = G('36'), RED = G('31'), RESET = G('0');

const states = new Map(NODES.map((n) => [n.id, {
  ...n, fps: 0, total: 0, gaps: 0, stale: 0, p50: 0, p99: 0, members: 0,
  routes: {}, metricsPort: 0,
}]));
let convergedAt = null;
const t0 = Date.now();

const procs = NODES.map((n) => {
  const seeds = NODES.filter((o) => o.id !== n.id)
    .map((o) => `127.0.0.1:${BASE_GOSSIP + o.id - 1}`).join(',');
  const child = spawn(process.execPath, [
    path.join(__dirname, 'lib', 'node_app.mjs'),
    '--id', String(n.id), '--role', n.role,
    '--gossip-port', String(BASE_GOSSIP + n.id - 1),
    '--seeds', seeds, '--fps', String(fps), '--seconds', String(seconds),
    '--run-id', arg('run-id', `run-${t0}`),
  ], { cwd: __dirname, stdio: ['ignore', 'pipe', 'inherit'] });
  child.stdout.setEncoding('utf8');
  let buf = '';
  child.stdout.on('data', (chunk) => {
    buf += chunk;
    let idx;
    while ((idx = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, idx); buf = buf.slice(idx + 1);
      if (line.startsWith('#STATUS ')) {
        try {
          const s = JSON.parse(line.slice(8));
          Object.assign(states.get(s.node), s);
          if (s.members >= NODES.length && convergedAt === null) {
            convergedAt = Date.now() - t0;
          }
        } catch { /* partial line — ignore */ }
      }
    }
  });
  return child;
});

function render(final) {
  const rows = [...states.values()];
  const maxFps = Math.max(1, ...rows.map((r) => r.fps));
  const lines = [];
  lines.push(`${BOLD}weft-cluster${RESET} — distributed cluster feed ${DIM}(4 nodes, localhost UDP, WCN1 wire)${RESET}`);
  lines.push(`run ${seconds}s @ ${fps} fps/topic   uptime ${(Date.now() - t0) / 1000 | 0}s   ${convergedAt ? `${GREEN}mesh converged in ${convergedAt} ms${RESET}` : `${YELLOW}discovering peers...${RESET}`}`);
  lines.push('');
  lines.push(`${BOLD}NODE  ROLE      MEM  FPS      TOTAL     GAPS  STALE  HOP P50    HOP P99    ROUTES${RESET}`);
  for (const r of rows) {
    const bar = '█'.repeat(Math.round((r.fps / maxFps) * 18)).padEnd(18, '░');
    const color = r.gaps > 0 || r.stale > 0 ? RED : (r.role === 'producer' ? CYAN : GREEN);
    const routes = Object.entries(r.routes ?? {})
      .map(([t, o]) => `${t.split('.')[1]}→n${o}`).join(' ') || '-';
    lines.push(
      `n${r.id}    ${r.role.padEnd(10)}${String(r.members).padEnd(5)}${color}${bar} ${String(r.fps).padStart(7)}${RESET} ${String(r.total).padStart(9)} ${String(r.gaps).padStart(5)} ${String(r.stale).padStart(6)} ${((r.p50) / 1000).toFixed(1).padStart(6)}us ${(r.p99 / 1000).toFixed(1).padStart(6)}us  ${DIM}${routes}${RESET}`);
  }
  lines.push('');
  lines.push(`${DIM}zero-copy ingestion over the WCR1 mesh · seq monotonicity verified per (node,topic) · latency = CLOCK_MONOTONIC one-way${RESET}`);
  if (final) {
    const bad = rows.some((r) => r.role === 'consumer' && (r.gaps > 0 || r.stale > 0));
    lines.push(bad ? `${RED}FAIL: sequence violations detected${RESET}` : `${GREEN}PASS: sequence monotonic, zero gaps, zero stale on all consumers${RESET}`);
  }
  process.stdout.write(CLEAR + lines.join('\n') + '\n');
}

const renderTimer = setInterval(() => render(false), 300);
if (web) {
  const server = http.createServer((req, res) => {
    if (req.url === '/api/status') {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({
        runId: arg('run-id', `run-${t0}`), seconds, fps,
        convergedMs: convergedAt,
        nodes: [...states.values()],
      }));
      return;
    }
    if (req.url === '/' || req.url === '/index.html') {
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end(fs.readFileSync(path.join(__dirname, 'web', 'index.html')));
      return;
    }
    res.writeHead(404); res.end();
  });
  await new Promise((r) => server.listen(parseInt(arg('web-port', '8099'), 10), r));
  console.log(`browser visualizer: http://127.0.0.1:${server.address().port}/`);
}

await new Promise((r) => setTimeout(r, seconds * 1000 + 2500));
clearInterval(renderTimer);
const exits = Promise.all(procs.map((p) => new Promise((r) => {
  if (p.exitCode !== null) r(); else p.once('exit', r);
})));
for (const p of procs) p.kill('SIGTERM');
await Promise.race([exits, new Promise((r) => setTimeout(r, 5000))]);
render(true);

const evidence = {
  kind: 'weft-cluster-demo-run', transport: 'udp', nodes: NODES.length,
  fpsTargetPerTopic: fps, seconds, convergedMs: convergedAt,
  at: new Date().toISOString(),
  nodes: [...states.values()],
  monotonicity: [...states.values()]
    .every((r) => r.role !== 'consumer' || (r.gaps === 0 && r.stale === 0)),
};
const evidenceDir = path.join(__dirname, 'evidence');
fs.mkdirSync(evidenceDir, { recursive: true });
fs.writeFileSync(path.join(evidenceDir, `udp-run-${evidence.runId}.json`),
  JSON.stringify(evidence, null, 2) + '\n');
process.exit(evidence.monotonicity ? 0 : 2);
