#!/usr/bin/env node
// test.mjs — Perfetto bridge golden gate (RFC 0016 §7, Law 4).
//
// The bridge is DETERMINISTIC: identical inputs -> byte-identical JSON.
// This test regenerates the synthetic capture in-process, byte-compares
// against tools/perfetto/golden/trace.json, and re-checks the pinned
// sha256 (double lock: content AND digest, the digest catching any
// cross-process divergence). Any change — a reordered key, a tie-break,
// a float sneaking in — is a hard failure. No silent green.

import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { selftest } from './weftrec2perfetto.mjs';

// selftest() prints its own scorecard; silence it for this gate
const origLog = console.log;
console.log = () => {};
const { json, bad } = selftest();
console.log = origLog;

const golden = readFileSync(new URL('./golden/trace.json', import.meta.url), 'utf8');
const digest = createHash('sha256').update(json).digest('hex');
const checks = [
  [bad === 0, 'selftest invariants (8/8)'],
  [json === golden, 'byte-identical to golden/trace.json'],
  [digest === createHash('sha256').update(golden).digest('hex'), 'sha256 agreement'],
  // pinned cross-process determinism: produced by
  // `node weftrec2perfetto.mjs --selftest`; must be stable on every runner.
  [digest === '1dde7195c0328f4c9349cf5ad8af6de21b202e681e1c2e3d45acacbe354d4d05',
    'pinned digest (cross-process determinism)'],
];
let fails = 0;
for (const [ok, name] of checks) {
  console.log(`  ${ok ? 'PASS' : 'FAIL'} ${name}`);
  if (!ok) fails++;
}
console.log(`== perfetto golden gate: ${checks.length - fails}/${checks.length} ==`);
process.exit(fails === 0 ? 0 : 1);
