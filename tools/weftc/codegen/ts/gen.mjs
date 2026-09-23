#!/usr/bin/env node
// gen.mjs — weftc TypeScript backend (`--target=ts`).
//
// Emits, per IR struct, a flyweight view class in three coordinated files:
//   <name>-view.ts    — canonical typed source (TS consumers)
//   <name>-view.js    — ESM runtime (Node/Deno/Bun/browsers; zero deps)
//   <name>-view.d.ts  — declarations with byte-offset JSDoc
// plus an index barrel (via emitIndex). Deterministic: identical IR ->
// byte-identical output.
//
// Design laws (Mission Briefing Pillar 1 §3):
//   Law 1  hot path (getters/setters/with*-chainers) allocates nothing:
//          no `new`, no literals, no template strings, no string ops.
//          bind() allocates one DataView — cold path, once per ring swap.
//   Law 2  every multi-byte DataView access passes explicit `true`
//          (little-endian), bit-exact with C/Rust/GPU buffers.
//   Law 3  runtime uses only ArrayBuffer/SharedArrayBuffer/DataView — no
//          node imports, no browser globals; runs on Node, Deno, Bun, Web.
//   Law 4  validateHeader(avail?) — non-throwing handshake; mirrors the
//          kernel's weft_envelope_decode() decision table for envelope
//          schemas (bounds + consts + payload availability).

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseIrJson } from '../lib/ir.mjs';
import { TS_ACCESS } from '../lib/types.mjs';
import { banner, docComment } from '../lib/emitter.mjs';
import { pascal, camel, screaming, safeIdent } from '../lib/names.mjs';

const GENERATOR = 'weftc-managed/1.0.0';

/**
 * Normalize the IR path recorded in banners: keep only the repo-relative
 * suffix from `tools/weftc/` onward so regeneration is byte-identical on
 * any machine/checkout (the determinism gate depends on this).
 */
function stableIrPath(p) {
  const i = p.lastIndexOf('tools/weftc/');
  return i >= 0 ? p.slice(i) : p;
}

function tsType(type) {
  if (type === 'u64' || type === 'i64') return 'bigint';
  if (type === 'bool') return 'boolean';
  return 'number';
}

function hasLittleEndianArg(type) {
  return !(type === 'u8' || type === 'i8' || type === 'bool');
}

function kebab(name) {
  return name
    .replace(/([a-z0-9])([A-Z])/g, '$1-$2')
    .replace(/([A-Z]+)([A-Z][a-z])/g, '$1-$2')
    .toLowerCase();
}

function memberSpecs(ir) {
  return ir.fields
    .filter((f) => f.role !== 'reserved')
    .map((f) => {
      const ident = safeIdent(camel(f.name), 'ts');
      return {
        f,
        ident,
        upper: screaming(f.name),
        isArray: f.count > 1,
        isBytesConst: f.count > 1 && f.type === 'u8' && f.const?.kind === 'bytes',
        readOnly: f.const !== null,
        access: TS_ACCESS[f.type],
        leArg: hasLittleEndianArg(f.type),
        tsT: tsType(f.type),
        doc: f.doc,
      };
    });
}

function layoutTable(ir) {
  const lines = [' * Layout (little-endian):', ' *'];
  for (const f of ir.fields) {
    const hex = f.offset.toString(16).padStart(2, '0');
    const arr = f.count > 1 ? `[${f.count}]` : '';
    let c = '';
    if (f.const) {
      c = f.const.kind === 'bytes'
        ? `  (const "${String.fromCharCode(...f.const.bytes)}")`
        : `  (const ${f.const.value})`;
    }
    const role = f.role ? `  <${f.role}>` : '';
    lines.push(` *   0x${hex}  ${f.type}${arr}  ${camel(f.name)}${c}${role}`);
  }
  return lines;
}

function classJSDoc(ir) {
  const out = ['/**'];
  out.push(` * Flyweight view over ${ir.name} — zero-allocation managed projection.`);
  if (ir.doc) for (const l of docComment(ir.doc, ' * ')) out.push(l);
  out.push(' *');
  out.push(...layoutTable(ir));
  out.push(' *');
  out.push(' * Steady-state reads and writes allocate nothing (Law 1): instantiate once,');
  out.push(' * then .bind(buffer, offset) for every frame. All multi-byte access is');
  out.push(' * explicitly little-endian (Law 2) and bit-exact with C/Rust/GPU buffers.');
  out.push(' * Runs on Node.js, Deno, Bun and browsers over ArrayBuffer |');
  out.push(' * SharedArrayBuffer | TypedArray windows (Law 3).');
  out.push(' */');
  return out;
}

/**
 * Renders the class. `typed=true` -> TypeScript (access modifiers +
 * annotations); `typed=false` -> plain ESM JavaScript.
 */
function renderClass(ir, typed) {
  const cls = `${ir.name}View`;
  const specs = memberSpecs(ir);
  // Modifiers: `public`/`private` are TS-only keywords.
  const pub = typed ? 'public ' : '';
  const prv = typed ? 'private ' : '';

  const L = [];
  L.push(...classJSDoc(ir));
  L.push(`export class ${cls} {`);
  L.push(`  /** Struct size in bytes. */`);
  L.push(`  ${pub}static ${typed ? 'readonly ' : ''}BYTE_LENGTH${typed ? ': number' : ''} = ${ir.byteLength};`);
  if (ir.schemaId !== null) {
    L.push(`  /** Payload schema handshake (Law 4). */`);
    L.push(`  ${pub}static ${typed ? 'readonly ' : ''}SCHEMA_ID${typed ? ': bigint' : ''} = 0x${ir.schemaId.toString(16)}n;`);
  }
  if (ir.schemaVersion !== null) {
    L.push(`  ${pub}static ${typed ? 'readonly ' : ''}SCHEMA_VERSION${typed ? ': number' : ''} = ${ir.schemaVersion};`);
  }
  if (ir.fields.some((f) => f.role === 'headerSize')) {
    L.push(`  /** Normative triad-1 minimum header size (03-ENVELOPE §2). */`);
    L.push(`  ${pub}static ${typed ? 'readonly ' : ''}MIN_HEADER_SIZE${typed ? ': number' : ''} = 16;`);
  }
  // Per-field offset constants — consumed by tooling and the parity audit.
  for (const f of ir.fields) {
    L.push(`  /** @byteOffset ${f.offset} (${f.type}${f.count > 1 ? `[${f.count}]` : ''}) */`);
    L.push(`  ${pub}static ${typed ? 'readonly ' : ''}${screaming(f.name)}_OFFSET${typed ? ': number' : ''} = ${f.offset};`);
  }
  L.push('');
  if (typed) {
    L.push(`  ${prv}_view!: DataView;`);
  } else {
    L.push(`  ${prv}_view;`);
  }
  L.push(`  ${prv}_byteOffset${typed ? ': number' : ''} = 0;`);
  L.push(`  ${prv}_bound${typed ? ': boolean' : ''} = false;`);
  L.push('');
  L.push(`  /** Absolute byte offset this view is bound at (after window resolution). */`);
  L.push(`  ${pub}get byteOffset()${typed ? ': number' : ''} { return this._byteOffset; }`);
  L.push('');
  L.push(`  /** True once bound; getters require it. */`);
  L.push(`  ${pub}get isBound()${typed ? ': boolean' : ''} { return this._bound; }`);
  L.push('');
  // --- bind() (cold path) ---
  L.push(`  ${pub}bind(buffer${typed ? ': ArrayBufferLike | ArrayBufferView' : ''}, byteOffset${typed ? ': number' : ''} = 0)${typed ? ': this' : ''} {`);
  L.push('    // COLD PATH — one DataView per bind; hot frames never allocate (Law 1).');
  L.push('    // Accepts raw ArrayBuffer/SharedArrayBuffer, or any TypedArray/DataView/');
  L.push('    // Node Buffer window (its .buffer + .byteOffset are honored exactly).');
  L.push(`    const maybeView = buffer${typed ? ' as ArrayBufferView' : ''};`);
  L.push('    let base;');
  L.push('    let baseOffset;');
  L.push('    let baseLen;');
  L.push('    if (typeof maybeView.byteOffset === "number" && typeof maybeView.byteLength === "number" && typeof maybeView.buffer !== "undefined") {');
  L.push('      base = maybeView.buffer;');
  L.push('      baseOffset = maybeView.byteOffset;');
  L.push('      baseLen = maybeView.byteLength;');
  L.push('    } else {');
  L.push(`      base = buffer${typed ? ' as ArrayBufferLike' : ''};`);
  L.push('      baseOffset = 0;');
  L.push('      baseLen = base.byteLength;');
  L.push('    }');
  L.push(`    if (typeof byteOffset !== "number" || !Number.isSafeInteger(byteOffset) || byteOffset < 0 || byteOffset + ${cls}.BYTE_LENGTH > baseLen) {`);
  L.push(`      throw new RangeError("${cls}.bind: window [" + byteOffset + ".." + (byteOffset + ${cls}.BYTE_LENGTH) + ") exceeds buffer of " + baseLen + " bytes");`);
  L.push('    }');
  L.push(`    this._view = new DataView(base, baseOffset + byteOffset, ${cls}.BYTE_LENGTH);`);
  L.push('    this._byteOffset = baseOffset + byteOffset;');
  L.push('    this._bound = true;');
  L.push('    return this;');
  L.push('  }');
  L.push('');
  // --- validateHeader() (Law 4) ---
  L.push(`  ${pub}validateHeader(avail${typed ? '?: number' : ''})${typed ? ': boolean' : ''} {`);
  L.push('    if (!this._bound) { return false; }');
  for (const s of specs) {
    if (!s.f.const) continue;
    const o = s.f.offset;
    if (s.isBytesConst) {
      for (let i = 0; i < s.f.const.bytes.length; i++) {
        const ch = String.fromCharCode(s.f.const.bytes[i]);
        L.push(`    if (this._view.getUint8(${o + i}) !== ${s.f.const.bytes[i]}) { return false; } // "${ch}"`);
      }
    } else if (s.f.type === 'u64' || s.f.type === 'i64') {
      L.push(`    if (this._view.${s.access.get}(${o}, true) !== ${cls}.${s.upper}) { return false; }`);
    } else if (s.f.type === 'bool') {
      L.push(`    if (this._view.getUint8(${o}) !== ${s.f.const.value}) { return false; }`);
    } else if (s.leArg) {
      L.push(`    if (this._view.${s.access.get}(${o}, true) !== ${JSON.stringify(s.f.const.value)}) { return false; }`);
    } else {
      L.push(`    if (this._view.${s.access.get}(${o}) !== ${JSON.stringify(s.f.const.value)}) { return false; }`);
    }
  }
  const hasHeaderSize = ir.fields.some((f) => f.role === 'headerSize');
  const hasPayloadLen = ir.fields.some((f) => f.role === 'payloadLen');
  if (hasHeaderSize && hasPayloadLen) {
    const hs = ir.fields.find((f) => f.role === 'headerSize');
    const pl = ir.fields.find((f) => f.role === 'payloadLen');
    const hsGet = `this._view.${TS_ACCESS[hs.type].get}(${hs.offset}, true)`;
    const plGet = `this._view.${TS_ACCESS[pl.type].get}(${pl.offset}, true)`;
    L.push('    if (avail !== undefined) {');
    L.push(`      const hs = ${hsGet};`);
    L.push(`      const pl = ${plGet};`);
    L.push(`      if (hs < ${cls}.MIN_HEADER_SIZE || hs > avail) { return false; }`);
    L.push('      if (pl > avail - hs) { return false; } // payload bytes must exist (kernel SHORT)');
    L.push('    }');
  }
  L.push('    return true;');
  L.push('  }');

  for (const s of specs) {
    const { f } = s;
    L.push('');
    L.push('  /**');
    if (s.doc) for (const l of docComment(s.doc, '   * ')) L.push(l);
    const hex = f.offset.toString(16).padStart(2, '0');
    if (s.isArray) {
      L.push(`   * @byteOffset 0x${hex} (${f.type}[${f.count}], element stride ${Math.round(f.size / f.count)})`);
    } else {
      L.push(`   * @byteOffset 0x${hex} (${f.type})`);
    }
    if (f.const) L.push('   * @const validated by validateHeader() (Law 4)');
    L.push('   */');

    const o = f.offset;
    if (!s.isArray) {
      const getCall = s.leArg
        ? `this._view.${s.access.get}(${o}, true)`
        : `this._view.${s.access.get}(${o})`;
      const getExpr = f.type === 'bool' ? `${getCall} !== 0` : getCall;
      L.push(`  ${pub}get ${s.ident}()${typed ? `: ${s.tsT}` : ''} {`);
      L.push(`    return ${getExpr};`);
      L.push('  }');
      if (!s.readOnly) {
        const setCall = s.leArg
          ? `this._view.${s.access.set}(${o}, value, true)`
          : `this._view.${s.access.set}(${o}, ${f.type === 'bool' ? 'value ? 1 : 0' : 'value'})`;
        L.push(`  ${pub}set ${s.ident}(value${typed ? `: ${s.tsT}` : ''}) {`);
        L.push(`    ${setCall};`);
        L.push('  }');
        L.push(`  /** Fluent mutator — returns this, allocates nothing (Law 1). */`);
        L.push(`  ${pub}with${pascal(s.ident)}(value${typed ? `: ${s.tsT}` : ''})${typed ? ': this' : ''} {`);
        L.push(`    this.${s.ident} = value;`);
        L.push('    return this;');
        L.push('  }');
      }
      if ((f.type === 'u64' || f.type === 'i64') && !s.isArray) {
        // 64-bit split accessors: primitive-only (number) hot path for
        // nanosecond timestamps — avoids transient BigInt boxing entirely.
        L.push(`  /** Low 32 bits of ${s.ident} @0x${o.toString(16)} (u32 LE) — primitive-only path. */`);
        L.push(`  ${pub}get ${s.ident}Lo()${typed ? ': number' : ''} { return this._view.getUint32(${o}, true); }`);
        if (!s.readOnly) {
          L.push(`  ${pub}set ${s.ident}Lo(value${typed ? ': number' : ''}) { this._view.setUint32(${o}, value, true); }`);
        }
        L.push(`  /** High 32 bits of ${s.ident} @0x${(o + 4).toString(16)} (u32 LE) — primitive-only path. */`);
        L.push(`  ${pub}get ${s.ident}Hi()${typed ? ': number' : ''} { return this._view.getUint32(${o + 4}, true); }`);
        if (!s.readOnly) {
          L.push(`  ${pub}set ${s.ident}Hi(value${typed ? ': number' : ''}) { this._view.setUint32(${o + 4}, value, true); }`);
          L.push(`  /** Fluent mutator — returns this, allocates nothing (Law 1). */`);
          L.push(`  ${pub}with${pascal(s.ident)}Lo(value${typed ? ': number' : ''})${typed ? ': this' : ''} {`);
          L.push(`    this._view.setUint32(${o}, value, true);`);
          L.push('    return this;');
          L.push('  }');
          L.push(`  ${pub}with${pascal(s.ident)}Hi(value${typed ? ': number' : ''})${typed ? ': this' : ''} {`);
          L.push(`    this._view.setUint32(${o + 4}, value, true);`);
          L.push('    return this;');
          L.push('  }');
        }
      }
    } else {
      const stride = f.size / f.count;
      const baseExpr = `${o} + index * ${stride}`;
      const getCall = s.leArg
        ? `this._view.${s.access.get}(${baseExpr}, true)`
        : `this._view.${s.access.get}(${baseExpr})`;
      const getExpr = f.type === 'bool' ? `${getCall} !== 0` : getCall;
      L.push(`  ${pub}get${pascal(s.ident)}At(index${typed ? ': number' : ''})${typed ? `: ${s.tsT}` : ''} {`);
      L.push(`    return ${getExpr};`);
      L.push('  }');
      if (!s.readOnly) {
        const setCall = s.leArg
          ? `this._view.${s.access.set}(${baseExpr}, value, true)`
          : `this._view.${s.access.set}(${baseExpr}, ${f.type === 'bool' ? 'value ? 1 : 0' : 'value'})`;
        L.push(`  ${pub}set${pascal(s.ident)}At(index${typed ? ': number' : ''}, value${typed ? `: ${s.tsT}` : ''})${typed ? ': void' : ''} {`);
        L.push(`    ${setCall};`);
        L.push('  }');
        L.push(`  /** Fluent mutator — returns this, allocates nothing (Law 1). */`);
        L.push(`  ${pub}with${pascal(s.ident)}At(index${typed ? ': number' : ''}, value${typed ? `: ${s.tsT}` : ''})${typed ? ': this' : ''} {`);
        L.push(`    this.set${pascal(s.ident)}At(index, value);`);
        L.push('    return this;');
        L.push('  }');
      }
    }
  }
  L.push('}');
  return L.join('\n') + '\n';
}

function renderDts(ir) {
  const cls = `${ir.name}View`;
  const specs = memberSpecs(ir);
  const L = [];
  L.push(...classJSDoc(ir));
  L.push(`export declare class ${cls} {`);
  L.push(`  public static readonly BYTE_LENGTH: number;`);
  if (ir.schemaId !== null) L.push(`  public static readonly SCHEMA_ID: bigint;`);
  if (ir.schemaVersion !== null) L.push(`  public static readonly SCHEMA_VERSION: number;`);
  if (ir.fields.some((f) => f.role === 'headerSize')) {
    L.push(`  public static readonly MIN_HEADER_SIZE: number;`);
  }
  for (const f of ir.fields) {
    const hexO = f.offset.toString(16).padStart(2, '0');
    const arr = f.count > 1 ? `[${f.count}]` : '';
    L.push(`  /** @byteOffset 0x${hexO} (${f.type}${arr}) */`);
    L.push(`  public static readonly ${screaming(f.name)}_OFFSET: number;`);
  }
  L.push(`  public get byteOffset(): number;`);
  L.push(`  public get isBound(): boolean;`);
  L.push(`  public bind(buffer: ArrayBufferLike | ArrayBufferView, byteOffset?: number): this;`);
  L.push(`  public validateHeader(avail?: number): boolean;`);
  for (const s of specs) {
    if (!s.isArray) {
      L.push(`  public get ${s.ident}(): ${s.tsT};`);
      if (!s.readOnly) {
        L.push(`  public set ${s.ident}(value: ${s.tsT});`);
        L.push(`  public with${pascal(s.ident)}(value: ${s.tsT}): this;`);
      }
      if (s.f.type === 'u64' || s.f.type === 'i64') {
        L.push(`  public get ${s.ident}Lo(): number;`);
        if (!s.readOnly) L.push(`  public set ${s.ident}Lo(value: number);`);
        L.push(`  public get ${s.ident}Hi(): number;`);
        if (!s.readOnly) L.push(`  public set ${s.ident}Hi(value: number);`);
      }
    } else {
      L.push(`  public get${pascal(s.ident)}At(index: number): ${s.tsT};`);
      if (!s.readOnly) {
        L.push(`  public set${pascal(s.ident)}At(index: number, value: ${s.tsT}): void;`);
        L.push(`  public with${pascal(s.ident)}At(index: number, value: ${s.tsT}): this;`);
      }
    }
  }
  L.push('}');
  return L.join('\n') + '\n';
}

/** @returns {{files: Map<string,string>, base: string}} sorted-deterministic. */
export function generateTs(ir, { irPath = 'ir.json' } = {}) {
  const base = `${kebab(ir.name)}-view`;
  const bannerArgs = { generator: GENERATOR, irPath: stableIrPath(irPath), ir, target: 'ts' };
  const files = new Map();
  files.set(`${base}.ts`, banner(bannerArgs) + renderClass(ir, true));
  files.set(`${base}.js`, banner(bannerArgs) + renderClass(ir, false));
  files.set(`${base}.d.ts`, banner(bannerArgs) + renderDts(ir));
  return { files, base };
}

/** Barrel over multiple struct names (used by the weftc CLI multi-target). */
export function emitIndex(irNames, { irPath = 'ir.json' } = {}) {
  const bannerText = banner({
    generator: GENERATOR,
    irPath: stableIrPath(irPath),
    ir: { name: 'index', byteLength: 0, endian: 'little' },
    target: 'ts',
  });
  const lines = [...irNames].sort().map((n) => `export { ${n}View } from './${kebab(n)}-view.js';`);
  return { files: new Map([['index.ts', bannerText + lines.join('\n') + '\n']]) };
}

// CLI: --ir <path> --out <dir>
function main() {
  const argv = process.argv.slice(2);
  const get = (flag) => {
    const i = argv.indexOf(flag);
    return i >= 0 ? argv[i + 1] : undefined;
  };
  const irPathArg = get('--ir');
  const outArg = get('--out');
  if (!irPathArg || !outArg) {
    console.error('usage: node gen.mjs --ir <schema.json> --out <dir>');
    process.exit(2);
  }
  const irPath = resolve(irPathArg);
  const ir = parseIrJson(readFileSync(irPath, 'utf8'));
  const outDir = resolve(outArg);
  mkdirSync(outDir, { recursive: true });
  const { files } = generateTs(ir, { irPath });
  for (const [name, content] of [...files].sort()) {
    const p = join(outDir, name);
    writeFileSync(p, content);
    console.error(`wrote ${p}`);
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main();
}
