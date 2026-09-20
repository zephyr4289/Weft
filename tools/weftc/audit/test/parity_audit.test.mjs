// Cross-backend parity audit — test wrapper (node --test).
//
// Positive: the committed golden outputs of all four backends must agree
// with the IR for every fixture. Negative: a doctored IR (wrong offset)
// must be detected — the audit actually enforces agreement, it doesn't
// just print it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { auditFixture, auditAll } from '../parity_audit.mjs';
import { loadIr } from '../../codegen/lib/ir.mjs';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const FIXTURES = join(HERE, '..', '..', 'schema', 'fixtures');

test('all fixtures x all backends agree with the IR', () => {
  const { ok, results } = auditAll();
  const failures = results.flatMap((r) => r.failures ?? []);
  assert.deepEqual(failures, []);
  assert.equal(ok, true);
  // 3 fixtures x 4 backends = 12 green rows
  const rows = results.flatMap((r) => r.rows);
  assert.equal(rows.filter((r) => r.ok).length, 12);
});

test('negative: a doctored IR is detected, not swallowed', () => {
  const doc = JSON.parse(readFileSync(join(FIXTURES, 'telemetry_frame.json'), 'utf8'));
  const real = loadIr(doc);

  // (a) wrong byteLength must fail every backend row
  const driftLen = structuredClone(real);
  driftLen.byteLength = real.byteLength + 1;
  const r1 = auditFixture('telemetry_frame', driftLen);
  assert.equal(r1.ok, false);
  assert.equal(r1.failures.length, 4, 'one failure per backend');
  assert.ok(r1.failures.every((f) => f.includes('byteLength')));

  // (b) a shifted field offset must fail the offset-constant check
  const driftOffset = structuredClone(real);
  driftOffset.fields[2].offset = 15; // velocity claimed elsewhere
  const r2 = auditFixture('telemetry_frame', driftOffset);
  assert.equal(r2.ok, false);
  assert.ok(r2.failures.some((f) => f.includes('offset constant for velocity')));

  // (c) dropping validateHeader expectations is not possible: IR shape is
  // unchanged, so absence of a change stays green (control)
  const control = auditFixture('telemetry_frame', real);
  assert.equal(control.ok, true);
});

test('audit is deterministic and stable across repeated runs', () => {
  const a = JSON.stringify(auditAll());
  const b = JSON.stringify(auditAll());
  assert.equal(a, b);
});
