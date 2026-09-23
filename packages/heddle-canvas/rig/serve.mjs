#!/usr/bin/env node
// serve.mjs — the COOP/COEP harness server for the heddle rig (the
// Series-7 browser-sab pattern: cross-origin isolation is a REQUIREMENT
// for SharedArrayBuffer, so the server sends the headers, not the page).
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join } from 'node:path';

const root = new URL('.', import.meta.url).pathname;
const port = Number(process.env.PORT ?? 8931);

const MIME = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript' };

createServer(async (req, res) => {
  const path = req.url === '/' ? '/heddle_rig.html' : req.url;
  try {
    const body = await readFile(join(root, path.replace(/^\//, '')));
    res.writeHead(200, {
      'content-type': MIME[extname(path)] ?? 'application/octet-stream',
      'cross-origin-opener-policy': 'same-origin',
      'cross-origin-embedder-policy': 'require-corp',
    });
    res.end(body);
  } catch {
    res.writeHead(404);
    res.end('not found');
  }
}).listen(port, () => console.log(`rig server on :${port}`));
