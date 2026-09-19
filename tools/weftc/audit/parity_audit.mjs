// parity_audit.mjs — cross-backend static parity matrix (Pillar 1 P10).
//
// The managed codegen equivalent of the repo's binding-parity guard: every
// generated backend output is parsed and its extracted layout/API matrix is
// compared against the IR. Four backends x N fixtures must agree on:
//
//   byteLength, schemaId/version statics, per-field byte offsets,
//   per-field accessor presence, reserved-field exclusion,
//   validateHeader() presence (Law 4), little-endian discipline markers.
//
// Exit 0 = all rows green; exit 1 = any mismatch (with a printed matrix).
// Node --test wrapper: audit/test/parity_audit.test.mjs.

import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseIrJson } from '../codegen/lib/ir.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const WEFTC = join(HERE, '..');
const CODEGEN = join(WEFTC, 'codegen');
const FIXTURES = join(WEFTC, 'schema', 'fixtures');

function pascal(name) {
  return name.replace(/(^|[_-])([a-z])/g, (m, _s, c) => c.toUpperCase());
}
function camelCase(name) {
  const p = pascal(name);
  return p[0].toLowerCase() + p.slice(1);
}
function snakeCase(name) {
  return pascal(name)
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .toLowerCase();
}
function screaming(name) {
  return snakeCase(name).toUpperCase();
}
function kebab(name) {
  return pascal(name).replace(/([a-z0-9])([A-Z])/g, '$1-$2').toLowerCase();
}

// ---------------------------------------------------------------------------
// Per-backend extractors: file text -> { byteLength, hasSchemaId, offsets,
// accessors:Set<fieldName>, validateHeader:boolean, leMarkers:number }
// ---------------------------------------------------------------------------

function extractTs(ir) {
  const file = join(CODEGEN, 'ts', 'golden', `${kebab(ir.name)}-view.ts`);
  if (!existsSync(file)) return { missing: file };
  const src = readFileSync(file, 'utf8');
  const out = {
    file: `${kebab(ir.name)}-view.ts`,
    byteLength: /static readonly BYTE_LENGTH(?:: number)? = (\d+)/.exec(src)?.[1],
    hasSchemaId: ir.schemaId === null ? true : new RegExp(`static readonly SCHEMA_ID(?:: bigint)? = 0x${ir.schemaId.toString(16)}n`).test(src),
    offsets: {},
    accessors: new Set(),
    validateHeader: /validateHeader\(avail\?: number\): boolean/.test(src),
    leMarkers: (src.match(/, true\)/g) || []).length,
  };
  for (const f of ir.fields) {
    out.offsets[f.name] = new RegExp(`static readonly ${screaming(f.name)}_OFFSET(?:: number)? = ${f.offset}`).test(src);
    const c = camelCase(f.name);
    const C = pascal(f.name);
    const isArr = f.count > 1;
    const pattern = isArr ? `get${C}At\\(index` : `get ${c}\\(\\)`;
    if (new RegExp(pattern).test(src)) out.accessors.add(f.name);
  }
  return out;
}

function extractSwift(ir) {
  const file = join(CODEGEN, 'swift', 'golden', `${ir.name}.swift`);
  if (!existsSync(file)) return { missing: file };
  const src = readFileSync(file, 'utf8');
  const out = {
    file: `${ir.name}.swift`,
    byteLength: /static let byteLength: Int = (\d+)/.exec(src)?.[1],
    hasSchemaId: ir.schemaId === null ? true : new RegExp(`static let schemaID: UInt64 = 0x${ir.schemaId.toString(16).toUpperCase()}`).test(src),
    offsets: {},
    accessors: new Set(),
    validateHeader: /public func validateHeader\(avail: Int\? = nil\) -> Bool/.test(src),
    leMarkers: (src.match(/\.littleEndian/g) || []).length,
  };
  for (const f of ir.fields) {
    out.offsets[f.name] = new RegExp(`static let ${camelCase(f.name)}Offset: Int = ${f.offset}`).test(src);
    const c = camelCase(f.name);
    const C = pascal(f.name);
    const isArr = f.count > 1;
    const pattern = isArr ? `get${C}\\(at index` : `public var ${c}:`;
    if (new RegExp(pattern).test(src)) out.accessors.add(f.name);
  }
  return out;
}

function extractDart(ir) {
  const file = join(CODEGEN, 'dart', 'golden', `${snakeCase(ir.name)}.dart`);
  if (!existsSync(file)) return { missing: file };
  const src = readFileSync(file, 'utf8');
  const out = {
    file: `${snakeCase(ir.name)}.dart`,
    byteLength: /static const int byteLength = (\d+);/.exec(src)?.[1],
    hasSchemaId: ir.schemaId === null ? true : new RegExp(`static const int schemaId = 0x${ir.schemaId.toString(16).toUpperCase()};`).test(src),
    offsets: {},
    accessors: new Set(),
    validateHeader: /bool validateHeader\(\[int\? avail\]\)/.test(src),
    leMarkers: (src.match(/Endian\.little/g) || []).length,
  };
  for (const f of ir.fields) {
    out.offsets[f.name] = new RegExp(`static const int ${camelCase(f.name)}Offset = ${f.offset};`).test(src);
    const c = camelCase(f.name);
    const C = pascal(f.name);
    const isArr = f.count > 1;
    const pattern = isArr ? `${C}At\\(int index\\)` : `${DART_TYPE_KIND(f.type)} get ${c} `;
    if (new RegExp(pattern).test(src)) out.accessors.add(f.name);
  }
  return out;
}

function DART_TYPE_KIND(type) {
  return (type === 'f32' || type === 'f64') ? 'double' : 'int';
}

function extractPython(ir) {
  const file = join(CODEGEN, 'python', 'golden', `${snakeCase(ir.name)}.py`);
  if (!existsSync(file)) return { missing: file };
  const src = readFileSync(file, 'utf8');
  const out = {
    file: `${snakeCase(ir.name)}.py`,
    byteLength: /BYTE_LENGTH = (\d+)/.exec(src)?.[1],
    hasSchemaId: ir.schemaId === null ? true : new RegExp(`SCHEMA_ID = 0x${ir.schemaId.toString(16).toUpperCase()}`).test(src),
    offsets: {},
    accessors: new Set(),
    validateHeader: /def validate_header\(self, avail=None\) -> bool:/.test(src),
    leMarkers: (src.match(/Struct\("<(?:[^"])/g) || []).length,
  };
  for (const f of ir.fields) {
    out.offsets[f.name] = new RegExp(`${screaming(f.name)}_OFFSET = ${f.offset}`).test(src);
    const s = snakeCase(f.name);
    const isArr = f.count > 1;
    const pattern = isArr ? `def get_${s}_at\\(self, index: int\\):` : `def ${s}\\(self\\):`;
    if (new RegExp(pattern).test(src)) out.accessors.add(f.name);
  }
  return out;
}

// ---------------------------------------------------------------------------

const BACKENDS = [
  ['ts', extractTs],
  ['swift', extractSwift],
  ['dart', extractDart],
  ['py', extractPython],
];

/**
 * Audit one fixture across all backends.
 * @param {string} name fixture name (used to locate generated outputs)
 * @param {object} [irOverride] pre-loaded IR (testing hook for drift sims)
 * @returns {{ok: boolean, rows: object[], failures: string[]}}
 */
export function auditFixture(name, irOverride) {
  const ir = irOverride ?? parseIrJson(readFileSync(join(FIXTURES, `${name}.json`), 'utf8'));
  const rows = [];
  const failures = [];
  for (const [target, extract] of BACKENDS) {
    const e = extract(ir);
    if (e.missing) {
      failures.push(`${target}: missing output ${e.missing}`);
      rows.push({ target, file: '(missing)', ok: false });
      continue;
    }
    const row = { target, file: e.file, checks: [] };
    let ok = true;
    const bad = (msg) => {
      ok = false;
      failures.push(`${target}/${e.file}: ${msg}`);
    };
    if (Number(e.byteLength) !== ir.byteLength) bad(`byteLength ${e.byteLength} != ${ir.byteLength}`);
    else row.checks.push('byteLength');
    if (!e.hasSchemaId) bad('schemaId static mismatch');
    else row.checks.push('schemaId');
    for (const f of ir.fields) {
      if (!e.offsets[f.name]) bad(`offset constant for ${f.name} missing/wrong`);
    }
    if (row.checks.length >= 1 && Object.values(e.offsets).every(Boolean)) row.checks.push('offsets');
    for (const f of ir.fields) {
      if (f.role === 'reserved') {
        if (e.accessors.has(f.name)) bad(`reserved field ${f.name} has an accessor`);
        continue;
      }
      if (!e.accessors.has(f.name)) bad(`accessor for ${f.name} missing`);
    }
    if (Object.values(e.offsets).every(Boolean) && failures.length === 0) row.checks.push('accessors');
    if (!e.validateHeader) bad('validateHeader missing (Law 4)');
    else row.checks.push('validateHeader');
    if (e.leMarkers < 1) bad('no little-endian markers found (Law 2)');
    else row.checks.push('le');
    row.ok = ok;
    rows.push(row);
  }
  return { ok: failures.length === 0, rows, failures };
}

const ALL_FIXTURES = ['weft_envelope', 'telemetry_frame', 'imu_sample'];

export function auditAll() {
  const results = ALL_FIXTURES.map((n) => ({ name: n, ...auditFixture(n) }));
  const ok = results.every((r) => r.ok);
  return { ok, results };
}

function printMatrix(results) {
  console.log('cross-backend parity matrix (static audit)');
  console.log('='.repeat(72));
  for (const r of results) {
    console.log(`${r.name}  ${r.ok ? 'OK' : 'FAIL'}`);
    for (const row of r.rows) {
      console.log(`  ${row.target.padEnd(6)} ${String(row.file).padEnd(28)} ${row.ok ? 'green' : 'FAIL'}  ${(row.checks ?? []).join(',')}`);
    }
  }
  for (const r of results) {
    for (const f of r.failures ?? []) console.log(`  ! ${r.name}: ${f}`);
  }
}

// CLI
function main() {
  const { ok, results } = auditAll();
  printMatrix(results);
  if (!ok) process.exit(1);
  console.log('parity audit: all backends agree with the IR');
}

if (import.meta.url === `file://${process.argv[1]}` || process.argv[1]?.endsWith('parity_audit.mjs')) {
  main();
}
