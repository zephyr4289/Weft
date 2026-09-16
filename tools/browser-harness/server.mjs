// server.mjs — COOP/COEP harness server for the browser SAB leg.
//
// WHY EXISTS: the TS port's SAB path is gated by cross-origin isolation
// (WHITEPAPER §8.2: SharedArrayBuffer requires COOP/COEP). Nothing in the
// tree PROVED a real browser grants crossOriginIsolated=true and the ring
// runs end-to-end — the vitest suite runs on node, where SAB needs no
// headers. This server + probe make the browser claim falsifiable:
//   - serves packages/core/dist (the real @weft/core build)
//   - Cross-Origin-Opener-Policy: same-origin
//   - Cross-Origin-Embedder-Policy: require-corp
//   - collects the in-page report from POST /report
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

const DIST = process.argv[2] || 'packages/core/dist';
const PORT = Number(process.argv[3] || 8123);
const HERE = new URL('.', import.meta.url).pathname;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
};

let report = null;

const server = http.createServer(async (req, res) => {
  // The isolation headers ARE the test subject.
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');

  const url = new URL(req.url, `http://localhost:${PORT}`);

  if (url.pathname === '/report' && req.method === 'POST') {
    let body = '';
    for await (const chunk of req) body += chunk;
    report = JSON.parse(body);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end('{"ok":true}');
    return;
  }

  let file = null;
  if (url.pathname === '/' || url.pathname === '/index.html') {
    file = join(HERE, 'harness.html');
  } else if (url.pathname.startsWith('/dist/')) {
    const p = normalize(url.pathname.slice('/dist/'.length));
    file = join(DIST, p);
  } else if (url.pathname.startsWith('/harness/')) {
    const p = normalize(url.pathname.slice('/harness/'.length));
    file = join(HERE, p);
  }
  if (!file) {
    res.writeHead(404); res.end('not found'); return;
  }
  try {
    const data = await readFile(file);
    res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(data);
  } catch {
    res.writeHead(404); res.end('not found');
  }
});

server.listen(PORT, () => {
  console.log(`[browser-harness] listening on ${PORT} (COOP/COEP set)`);
});

// Exit 0 when the report lands and PASSES; exit 1 on failure; 3 on timeout.
const timeout = setTimeout(() => {
  console.error('[browser-harness] TIMEOUT waiting for report');
  process.exit(3);
}, 90_000);

function check(r) {
  return r && r.crossOriginIsolated === true
    && r.sabAvailable === true
    && r.kernelRoundtrip?.ok === true
    && r.fanout?.ok === true
    && r.fanout?.tornAccepted === 0;
}

(async function awaitReport() {
  while (!report) {
    await new Promise((r) => setTimeout(r, 200));
  }
  const pass = check(report);
  console.log('[browser-harness] report:', JSON.stringify(report, null, 2));
  clearTimeout(timeout);
  server.close();
  process.exit(pass ? 0 : 1);
})();
