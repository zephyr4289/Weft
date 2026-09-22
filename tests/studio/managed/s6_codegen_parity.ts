/**
 * Stage 6 — Codegen parity & layout truth.
 * - canonical.weft → 7 targets: sha256 == golden manifest
 * - layout facts (sizes, alignment, offsets, pads, hazards) == manifest
 * - double-generation determinism
 * - codegen latency < 1 ms per target (keystroke budget)
 */

import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { parseSchema, validateRefs } from '../../../packages/studio/src/engine/schema.ts';
import { computeLayout } from '../../../packages/studio/src/engine/layout.ts';
import { generate, CODEGEN_TARGETS } from '../../../packages/studio/src/engine/codegen.ts';

const results = { stage: 6, checks: [], ok: false };
let failures = 0;
function check(name, ok, detail = '') {
  results.checks.push({ name, ok, detail: String(detail).slice(0, 200) });
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

const GOLDEN_DIR = new URL('../../../fixtures/studio/golden/', import.meta.url).pathname;
const src = readFileSync(GOLDEN_DIR + 'canonical.weft', 'utf8');
const manifest = JSON.parse(readFileSync(GOLDEN_DIR + 'manifest.json', 'utf8'));

const parsed = parseSchema(src);
check('canonical schema parses clean', parsed.ok && parsed.diagnostics.length === 0);
if (parsed.doc) parsed.diagnostics.push(...validateRefs(parsed.doc));
const layout = computeLayout(parsed.doc!);
check('layout pass clean', layout.ok);

// schema hash parity
check('schema hash stable', parsed.doc!.hash === manifest.schema_hash,
  `${parsed.doc!.hash} vs ${manifest.schema_hash}`);

// layout facts parity
let layoutOk = true;
const drift: string[] = [];
for (const m of manifest.structs) {
  const L = layout.layouts.get(m.name)!;
  if (L.size !== m.size) drift.push(`${m.name}.size ${L.size}≠${m.size}`);
  if (L.alignment !== m.alignment) drift.push(`${m.name}.align ${L.alignment}≠${m.alignment}`);
  if (L.paddingBytes !== m.padding) drift.push(`${m.name}.pad ${L.paddingBytes}≠${m.padding}`);
  const leafKey = L.leaves.map((l) => `${l.struct}.${l.field}:${l.offset}:${l.size}:w${l.writer}`).join('|');
  const mLeafKey = m.leaves.map((l) => `${l.f}:${l.off}:${l.size}:w${l.w}`).join('|');
  if (leafKey !== mLeafKey) drift.push(`${m.name}.leaves drifted`);
}
check('layout facts match manifest (sizes/aligns/offsets/writers)', layoutOk && drift.length === 0,
  drift.join('; ') || `${manifest.structs.length} structs verified`);
check('false-sharing hazards match manifest',
  layout.hazards64.length === manifest.hazards64 && layout.hazards128.length === manifest.hazards128,
  `64B: ${layout.hazards64.length} (manifest ${manifest.hazards64}) · 128B: ${layout.hazards128.length} (manifest ${manifest.hazards128})`);
check('canonical schema exhibits the teaching hazard', layout.hazards64.length >= 1);

// golden hash parity + determinism + latency
let latencyOk = true;
let maxMs = 0;
for (const target of CODEGEN_TARGETS) {
  const t0 = performance.now();
  const out = generate(target, parsed.doc!, layout);
  const ms = performance.now() - t0;
  if (ms > maxMs) maxMs = ms;
  if (ms > 1.0) latencyOk = false;
  const hash = createHash('sha256').update(out).digest('hex');
  check(`golden ${target}`, hash === manifest.goldens[target],
    `${hash.slice(0, 12)} vs ${manifest.goldens[target].slice(0, 12)}`);
  const out2 = generate(target, parsed.doc!, layout);
  check(`golden ${target} deterministic`, out === out2);
}
check('codegen latency < 1 ms per target', latencyOk, `max ${maxMs.toFixed(3)} ms`);

results.ok = failures === 0;
writeFileSync(new URL('../../../evidence/pillar7/stage-6-codegen-parity.json', import.meta.url), JSON.stringify(results, null, 2));
console.log(results.ok ? 'STAGE 6: PASS' : `STAGE 6: FAIL (${failures})`);
process.exit(results.ok ? 0 : 2);
