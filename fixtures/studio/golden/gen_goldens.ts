// Golden fixture generator — deterministic, zero entropy.
// Writes: golden/<target>.golden + manifest.json (sha256 + schema hash + layout facts)
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { parseSchema, validateRefs } from '../../../packages/studio/src/engine/schema.ts';
import { computeLayout } from '../../../packages/studio/src/engine/layout.ts';
import { generate, CODEGEN_TARGETS } from '../../../packages/studio/src/engine/codegen.ts';

const HERE = new URL('.', import.meta.url).pathname;
const SRC = readFileSync(HERE + 'canonical.weft', 'utf8');

const r = parseSchema(SRC);
if (!r.ok || !r.doc) {
  console.error('canonical schema failed to parse', r.diagnostics);
  process.exit(1);
}
const refs = validateRefs(r.doc);
if (refs.length) { console.error('ref errors', refs); process.exit(1); }
const lr = computeLayout(r.doc);
if (!lr.ok) { console.error('layout errors', lr.diagnostics); process.exit(1); }

mkdirSync(HERE, { recursive: true });
const manifest: Record<string, unknown> = {
  schema_hash: r.doc.hash,
  structs: lr.order.map((name) => {
    const L = lr.layouts.get(name)!;
    return {
      name, size: L.size, alignment: L.alignment,
      used: L.usedBytes, padding: L.paddingBytes,
      efficiency: Math.round(L.efficiency * 1000) / 1000,
      leaves: L.leaves.map((l) => ({ f: `${l.struct}.${l.field}`, t: l.type, off: l.offset, size: l.size, w: l.writer })),
      pads: L.pads.map((p) => ({ off: p.offset, size: p.size })),
    };
  }),
  hazards64: lr.hazards64.length,
  hazards128: lr.hazards128.length,
  goldens: {} as Record<string, string>,
};

for (const t of CODEGEN_TARGETS) {
  const src = generate(t, r.doc, lr);
  const file = `golden-${t}.golden`;
  writeFileSync(HERE + file, src);
  manifest.goldens[t] = createHash('sha256').update(src).digest('hex');
  console.log(`golden ${t}: ${src.length}B ${manifest.goldens[t].slice(0, 12)}…`);
}

writeFileSync(HERE + 'manifest.json', JSON.stringify(manifest, null, 2) + '\n');
console.log('manifest.json written; schema hash', r.doc.hash);
