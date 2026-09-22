// Smoke: schema parse -> layout -> 7-language codegen
import { parseSchema, validateRefs } from '../../../packages/studio/src/engine/schema.ts';
import { computeLayout } from '../../../packages/studio/src/engine/layout.ts';
import { generate, CODEGEN_TARGETS } from '../../../packages/studio/src/engine/codegen.ts';

const SRC = `// canonical demo schema
struct Level {
  price: i64;    // fixed-point 1e-9
  qty: u32;
  flags: u8;
}

struct MarketTick {
  @writer(0) ts_ns: u64;
  @writer(0) bid: Level;
  @writer(1) ask: Level;
  @writer(1) last_px: i64;
  venue: u16;
  @align(64) seq: u64;
}
`;
const r = parseSchema(SRC);
console.log('parse ok:', r.ok, 'diags:', r.diagnostics.length);
for (const d of r.diagnostics) console.log('  diag', d.severity, d.line + ':' + d.col, d.message);
if (!r.doc) process.exit(1);
r.diagnostics.push(...validateRefs(r.doc));
const lr = computeLayout(r.doc);
console.log('layout ok:', lr.ok, 'order:', lr.order.join(','));
for (const name of lr.order) {
  const L = lr.layouts.get(name)!;
  console.log(`  ${name}: size=${L.size} align=${L.alignment} used=${L.usedBytes} pad=${L.paddingBytes} eff=${(L.efficiency * 100).toFixed(1)}%`);
  for (const l of L.leaves) console.log(`    leaf ${l.struct}.${l.field} ${l.type} @${l.offset} w${l.writer}`);
  for (const p of L.pads) console.log(`    pad  ${p.struct} @${p.offset} +${p.size}`);
}
console.log('hazards64:', lr.hazards64.length, 'hazards128:', lr.hazards128.length);
for (const h of lr.hazards64) console.log('  hazard line', h.cacheLine, 'fields', h.fields.join(','), 'writers', h.writers.join(','));
for (const t of CODEGEN_TARGETS) {
  const src = generate(t, r.doc, lr);
  console.log(`--- ${t} (${src.length}B, hash ${require('crypto').createHash('sha256').update(src).digest('hex').slice(0, 12)}) ---`);
  console.log(src.split('\n').slice(0, 6).join('\n'));
}
