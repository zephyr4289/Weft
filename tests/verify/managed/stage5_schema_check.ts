// stage5_schema_check.ts — Stage 5 validator: scorecard schema + formal
// artifact + determinism prerequisites.
//
// Usage: node stage5_schema_check.ts <scorecard.json> [formal.json]
// Exit 0 iff the scorecard validates, all theorems are PROVED, the wide
// tally reached >= 1e7 states, and chaos curves have the mandated shape.
import * as fs from 'node:fs';

function fail(msg: string): never {
  console.error(`stage5: FAIL ${msg}`);
  process.exit(1);
}

const [cardPath, formalPath] = process.argv.slice(2);
if (!cardPath) fail('usage: stage5_schema_check.ts <scorecard.json> [formal.json]');

let card: Record<string, unknown>;
try {
  card = JSON.parse(fs.readFileSync(cardPath, 'utf8')) as Record<string, unknown>;
} catch (e) {
  fail(`scorecard not parseable: ${(e as Error).message}`);
}

const errors: string[] = [];
if (card.schema !== 'weft-verify-scorecard/1') errors.push('schema mismatch');
for (const key of ['tool', 'node', 'platform', 'verdict', 'formal', 'allocation', 'chaos', 'resilience', 'package']) {
  if (!(key in card)) errors.push(`missing key: ${key}`);
}

const formal = card.formal as Record<string, unknown> | undefined;
if (formal === undefined) {
  errors.push('formal section missing');
} else {
  const theorems = formal.theorems as Array<Record<string, unknown>> | undefined;
  if (!Array.isArray(theorems) || theorems.length < 9) {
    errors.push('theorems must contain >= 9 entries');
  } else {
    for (const t of theorems) {
      if (t.verdict !== 'PROVED') errors.push(`theorem not PROVED: ${String(t.id)}`);
    }
  }
  const small = formal.small as Array<Record<string, unknown>> | undefined;
  if (!Array.isArray(small) || small.length !== 2) errors.push('formal.small must have 2 models');
  else {
    for (const m of small) {
      if (m.deadlocks !== 0) errors.push(`model ${String(m.model)} has deadlocks`);
      if (m.verdict !== 'PROVED') errors.push(`model ${String(m.model)} not PROVED`);
    }
  }
  const wide = formal.wide as Record<string, unknown> | undefined;
  if (wide === undefined) errors.push('formal.wide missing');
  else {
    const explored = Number(wide.explored);
    if (!Number.isFinite(explored) || explored < 10_000_000) {
      errors.push(`wide.explored ${explored} < 10,000,000`);
    }
    if (wide.deadlineHit === true) errors.push('wide exploration hit the deadline');
  }
}

const chaos = card.chaos as Record<string, unknown> | undefined;
if (chaos !== undefined) {
  const thermal = chaos.thermal as Record<string, unknown> | undefined;
  const bus = chaos.bus as Record<string, unknown> | undefined;
  const network = chaos.network as Record<string, unknown> | undefined;
  if (thermal?.pass !== true) errors.push('chaos.thermal not PASS');
  if (!Array.isArray(thermal?.curve) || (thermal!.curve as unknown[]).length !== 7) {
    errors.push('thermal curve must have 7 steps (3.2GHz..800MHz)');
  }
  if (bus?.pass !== true) errors.push('chaos.bus not PASS');
  if (network?.pass !== true) errors.push('chaos.network not PASS');
  const rates = network?.rates as Array<Record<string, unknown>> | undefined;
  if (!Array.isArray(rates) || rates.length !== 5) errors.push('network rates must have 5 points');
}

const alloc = card.allocation as Record<string, unknown> | undefined;
if (alloc !== undefined) {
  const summary = alloc.summary as Record<string, unknown> | undefined;
  if (summary?.compliant !== true) errors.push('allocation summary not compliant');
}

if (card.verdict !== 'PASS') errors.push('scorecard verdict not PASS');

// optional cross-check against the raw formal artifact
if (formalPath !== undefined) {
  try {
    const raw = JSON.parse(fs.readFileSync(formalPath, 'utf8')) as Record<string, unknown>;
    if (raw.schema !== 'weft-verify-formal/1') errors.push('formal artifact schema mismatch');
    if (raw.verdict !== 'PASS') errors.push('formal artifact verdict not PASS');
  } catch (e) {
    errors.push(`formal artifact unreadable: ${(e as Error).message}`);
  }
}

if (errors.length > 0) {
  console.error(`stage5: FAIL ${errors.length} error(s):`);
  for (const e of errors) console.error(`  - ${e}`);
  process.exit(1);
}
console.log('stage5: scorecard schema, theorems, tally >= 1e7 and chaos shape all VALID');
process.exit(0);
