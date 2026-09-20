#!/usr/bin/env node
// weftc.mjs — the weftc managed-codegen CLI (Engineer 3 slice, Pillar 1).
//
//   node tools/weftc/weftc.mjs --ir <file|dir> --target <list|all> --out <dir>
//   node tools/weftc/weftc.mjs --ir tools/weftc/schema/fixtures --target all \
//        --out tools/weftc/codegen --check      # CI determinism gate
//
// Targets: ts | swift | dart | py | all (comma separated).
// For a directory --ir, every *.json fixture is generated in one pass and the
// TS backend receives a barrel index.
//
// --check never writes: it regenerates in memory and byte-compares against
// --out. Any drift exits 1 with a diff list (no silent green, ever).
//
// Adapter note (Engineer 1): when the weftc Core AST lands, its IrSchema is
// mapped to IR v1 (tools/weftc/schema/README.md §5) and dispatched here; the
// backends are unchanged.

import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync, existsSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseIrJson } from './codegen/lib/ir.mjs';
import { generateTs, emitIndex } from './codegen/ts/gen.mjs';
import { generateSwift } from './codegen/swift/gen.mjs';
import { generateDart } from './codegen/dart/gen.mjs';
import { generatePython } from './codegen/python/gen.mjs';

const TARGETS = {
  ts: { gen: generateTs, dir: 'ts/golden' },
  swift: { gen: generateSwift, dir: 'swift/golden' },
  dart: { gen: generateDart, dir: 'dart/golden' },
  py: { gen: generatePython, dir: 'python/golden' },
};

function usage(code = 2) {
  console.error(`usage:
  weftc --ir <file.json|dir> --target <ts,swift,dart,py|all> --out <dir> [--check]

examples:
  weftc --ir tools/weftc/schema/fixtures/telemetry_frame.json --target ts --out generated/ts
  weftc --ir tools/weftc/schema/fixtures --target all --out tools/weftc/codegen --check
`);
  process.exit(code);
}

function collectIrPaths(irArg) {
  const st = statSync(irArg);
  if (st.isFile()) return [irArg];
  if (st.isDirectory()) {
    return readdirSync(irArg)
      .filter((f) => f.endsWith('.json') && !f.endsWith('.schema.json'))
      .sort()
      .map((f) => join(irArg, f));
  }
  usage();
}

function generateAll(irPaths, targets) {
  /** outputs: Map<targetDir, Map<fileName, content>> */
  const outputs = new Map();
  const names = [];
  const oneIrPath = irPaths.length === 1 ? irPaths[0] : irPaths[0]; // stable banner uses first path
  for (const p of irPaths) {
    const ir = parseIrJson(readFileSync(p, 'utf8'));
    names.push(ir.name);
    for (const t of targets) {
      const { gen, dir } = TARGETS[t];
      if (!outputs.has(dir)) outputs.set(dir, new Map());
      const bucket = outputs.get(dir);
      const { files } = gen(ir, { irPath: p });
      for (const [name, content] of files) bucket.set(name, content);
    }
  }
  // TS barrel across all structs
  if (targets.includes('ts')) {
    const bucket = outputs.get(TARGETS.ts.dir);
    const { files } = emitIndex(names, { irPath: oneIrPath });
    for (const [name, content] of files) bucket.set(name, content);
  }
  return outputs;
}

function main() {
  const argv = process.argv.slice(2);
  const get = (flag) => {
    const i = argv.indexOf(flag);
    return i >= 0 ? argv[i + 1] : undefined;
  };
  const has = (flag) => argv.includes(flag);
  if (has('--help') || has('-h')) usage(0);

  const irArg = get('--ir');
  const targetArg = get('--target') ?? 'all';
  const outArg = get('--out');
  const check = has('--check');
  if (!irArg || !outArg) usage();

  const irPaths = collectIrPaths(resolve(irArg));
  const requested = targetArg === 'all' ? Object.keys(TARGETS) : targetArg.split(',').map((s) => s.trim()).filter(Boolean);
  for (const t of requested) {
    if (!TARGETS[t]) {
      console.error(`weftc: unknown target "${t}" (known: ${Object.keys(TARGETS).join(', ')})`);
      process.exit(2);
    }
  }

  const outputs = generateAll(irPaths, requested);

  if (!check) {
    for (const [dir, files] of [...outputs].sort()) {
      const outDir = resolve(join(resolve(outArg), dir));
      mkdirSync(outDir, { recursive: true });
      for (const [name, content] of [...files].sort()) {
        writeFileSync(join(outDir, name), content);
        console.error(`wrote ${join(outDir, name)}`);
      }
    }
    console.error(`weftc: generated ${requested.join(', ')} for ${irPaths.length} schema(s) -> ${outArg}`);
    return;
  }

  // --check: byte-compare, never write.
  const drift = [];
  for (const [dir, files] of outputs) {
    for (const [name, content] of files) {
      const p = join(resolve(outArg), dir, name);
      if (!existsSync(p)) {
        drift.push(`missing: ${p}`);
        continue;
      }
      const committed = readFileSync(p, 'utf8');
      if (committed !== content) {
        drift.push(`drift:   ${p}`);
      }
    }
  }
  if (drift.length) {
    console.error(`weftc --check: ${drift.length} file(s) out of sync with the generators:`);
    for (const d of drift) console.error(`  ${d}`);
    console.error('regenerate with: node tools/weftc/weftc.mjs --ir tools/weftc/schema/fixtures --target all --out tools/weftc/codegen');
    process.exit(1);
  }
  console.error(`weftc --check: ${irPaths.length} schema(s) x ${requested.join(', ')} byte-identical to committed outputs`);
}

const SELF = fileURLToPath(import.meta.url);
if (process.argv[1] && resolve(process.argv[1]) === SELF) {
  main();
}
