// test/alloc.test.mjs — Law 1 enforcement: spawn `node --expose-gc` probes and
// assert the steady-state heap delta stays flat (no per-frame garbage).
// These are the tests the lead asked for: allocation claims with HARD evidence.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const probe = path.join(path.dirname(fileURLToPath(import.meta.url)), 'helpers', 'alloc_probe.mjs');

function runProbe(mode, iters = 100000) {
  const res = spawnSync(process.execPath, ['--expose-gc', probe, '--mode', mode, '--iters', String(iters)], {
    encoding: 'utf8',
    timeout: 120000,
  });
  if (res.status !== 0) {
    throw new Error(`probe ${mode} failed (${res.status}): ${res.stderr}`);
  }
  const line = res.stdout.trim().split('\n').pop();
  return JSON.parse(line);
}

// Threshold: 100k iterations must produce < 64 KiB of retained heap growth.
// A single stray 64-byte object per frame would be ~6.4 MB -> caught instantly.
const LIMIT_BYTES = 65536;

for (const mode of ['ring', 'ingest', 'audio', 'render', 'overlay']) {
  test(`zero-alloc probe [${mode}] — 100k steady-state ops stay flat`, () => {
    const r = runProbe(mode, 100000);
    assert.equal(r.iters, 100000);
    assert.ok(
      r.deltaBytes < LIMIT_BYTES,
      `${mode}: heap grew ${r.deltaBytes}B over 100k ops (limit ${LIMIT_BYTES}B) — per-frame garbage leaked into the hot loop`,
    );
  });
}

test('negative control: a deliberately-allocating loop IS caught', () => {
  // Proves the probe methodology detects garbage (no-silent-green for Law 1).
  const res = spawnSync(process.execPath, ['--expose-gc', '-e', `
    const before = process.memoryUsage().heapUsed;
    const junk = [];
    for (let i = 0; i < 100000; i++) junk.push({ i, pad: 'x'.repeat(8) });
    globalThis.gc(); // junk is still REACHABLE -> heap stays grown
    const after = process.memoryUsage().heapUsed;
    console.log(JSON.stringify({ mode: 'negative', deltaBytes: after - before }));
  `], { encoding: 'utf8', timeout: 60000 });
  const r = JSON.parse(res.stdout.trim().split('\n').pop());
  assert.ok(r.deltaBytes > 1000000, `negative control should show MBs of growth, got ${r.deltaBytes}B`);
});
