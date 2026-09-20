#!/usr/bin/env node
// gen.mjs — weftc Dart backend (`--target=dart`).
//
// Emits, per IR struct, a `<snake_name>.dart` file with TWO coordinated
// layers (Pillar 1 §2.C):
//   1. `final class <Name>Ffi extends ffi.Struct` — the dart:ffi projection
//      for native memory (declared in offset order; the IR's natural
//      alignment guarantees identical layout, audited by the parity gate).
//      Inline arrays use ffi.Array with explicit dimensions.
//   2. `class <Name>View` — the high-level zero-copy ByteData flyweight for
//      Flutter hot paths: bind/validateHeader/getters/setters/with* chainers,
//      every multi-byte access passes Endian.little (Law 2), u64 lo/hi split.
//
// No Dart toolchain in the verification sandbox: the output is verified by
// static audit (structure, offsets, LE discipline, API surface) — see
// tools/weftc/audit and codegen/dart/test.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseIrJson } from '../lib/ir.mjs';
import { banner, docComment } from '../lib/emitter.mjs';
import { camel, snake, screaming, safeIdent } from '../lib/names.mjs';

const GENERATOR = 'weftc-managed/1.0.0';

function stableIrPath(p) {
  const i = p.lastIndexOf('tools/weftc/');
  return i >= 0 ? p.slice(i) : p;
}

const DART_SCALAR = {
  u8: 'int', u16: 'int', u32: 'int', u64: 'int',
  i8: 'int', i16: 'int', i32: 'int', i64: 'int',
  f32: 'double', f64: 'double', bool: 'bool',
};

const FFI_TYPE = {
  u8: 'ffi.UnsignedChar', u16: 'ffi.UnsignedShort', u32: 'ffi.UnsignedInt', u64: 'ffi.Uint64',
  i8: 'ffi.SignedChar', i16: 'ffi.Short', i32: 'ffi.Int', i64: 'ffi.Int64',
  f32: 'ffi.Float', f64: 'ffi.Double', bool: 'ffi.UnsignedChar',
};

const DART_ACCESS = {
  u8:   { get: 'getUint8',   set: 'setUint8',   le: false },
  u16:  { get: 'getUint16',  set: 'setUint16',  le: true },
  u32:  { get: 'getUint32',  set: 'setUint32',  le: true },
  u64:  { get: 'getUint64',  set: 'setUint64',  le: true },
  i8:   { get: 'getInt8',    set: 'setInt8',    le: false },
  i16:  { get: 'getInt16',   set: 'setInt16',   le: true },
  i32:  { get: 'getInt32',   set: 'setInt32',   le: true },
  i64:  { get: 'getInt64',   set: 'setInt64',   le: true },
  f32:  { get: 'getFloat32', set: 'setFloat32', le: true },
  f64:  { get: 'getFloat64', set: 'setFloat64', le: true },
  bool: { get: 'getUint8',   set: 'setUint8',   le: false },
};

function dartValueExpr(type, raw) {
  if (type === 'bool') return `${raw} != 0`;
  return raw;
}

function dartStoreExpr(type) {
  if (type === 'bool') return '(value ? 1 : 0)';
  return 'value';
}

function renderDart(ir, irPath) {
  const cls = ir.name;             // high-level view class
  const ffiCls = `${ir.name}Ffi`;  // dart:ffi struct projection
  const fields = ir.fields.filter((f) => f.role !== 'reserved');
  const hasHeaderSize = ir.fields.some((f) => f.role === 'headerSize');
  const hasPayloadLen = ir.fields.some((f) => f.role === 'payloadLen');

  const L = [];
  L.push(banner({ generator: GENERATOR, irPath: stableIrPath(irPath), ir, target: 'dart' })
    .replace(/^\/\/ /gm, '// '));
  L.push("import 'dart:ffi' as ffi;");
  L.push("import 'dart:typed_data';");
  L.push('');
  L.push('/// Zero-copy managed projections over ' + ir.name + '.');
  if (ir.doc) for (const l of docComment(ir.doc, '/// ')) L.push(l);
  L.push('///');
  L.push('/// Layout (little-endian):');
  for (const f of ir.fields) {
    const hex = f.offset.toString(16).padStart(2, '0');
    const arr = f.count > 1 ? `[${f.count}]` : '';
    L.push(`///   0x${hex}  ${f.type}${arr}  ${camel(f.name)}`);
  }
  L.push('///');
  L.push('/// Steady-state reads and writes allocate nothing (Law 1): create the');
  L.push('/// view once, then bind() for every frame. Every multi-byte access');
  L.push('/// passes Endian.little explicitly (Law 2).');

  // ---------------- layer 1: dart:ffi struct ----------------
  L.push('');
  L.push(`/// dart:ffi projection for native memory (offset-order declaration;`);
  L.push(`/// the IR's natural alignment guarantees an identical layout).`);
  L.push(`final class ${ffiCls} extends ffi.Struct {`);
  for (const f of ir.fields) {
    const arr = f.count > 1 ? `[${f.count}]` : '';
    L.push(`  /// @byteOffset ${f.offset} (${f.type}${arr})`);
    if (f.count > 1) {
      L.push(`  @ffi.Array.multi([${f.count}])`);
      L.push(`  external ffi.Array<${FFI_TYPE[f.type]}> ${snake(f.name)};`);
    } else {
      L.push(`  external ${FFI_TYPE[f.type]} ${snake(f.name)};`);
    }
  }
  L.push('}');
  L.push('');

  // ---------------- layer 2: ByteData flyweight view ----------------
  L.push(`/// High-level zero-copy view over a ByteData window (Flutter hot path).`);
  L.push(`class ${cls} {`);
  L.push(`  static const int byteLength = ${ir.byteLength};`);
  if (ir.schemaId !== null) {
    // Dart hex int literals wrap to the exact 64-bit two's-complement bit
    // pattern — lossless for u64 schema IDs (VM ints are 64-bit).
    L.push(`  static const int schemaId = 0x${ir.schemaId.toString(16).toUpperCase()};`);
  }
  if (ir.schemaVersion !== null) {
    L.push(`  static const int schemaVersion = ${ir.schemaVersion};`);
  }
  if (hasHeaderSize) {
    L.push('  /// Normative triad-1 minimum header size (03-ENVELOPE §2).');
    L.push('  static const int minHeaderSize = 16;');
  }
  for (const f of ir.fields) {
    const arr = f.count > 1 ? `[${f.count}]` : '';
    L.push(`  /// @byteOffset ${f.offset} (${f.type}${arr})`);
    L.push(`  static const int ${camel(f.name)}Offset = ${f.offset};`);
  }
  L.push('');
  L.push('  ByteData? _bd;');
  L.push('  int _offset = 0;');
  L.push('');
  L.push('  /// True once bound.');
  L.push('  bool get isBound => _bd != null;');
  L.push('');
  L.push('  /// Absolute offset within the bound ByteData.');
  L.push('  int get byteOffset => _offset;');
  L.push('');
  L.push('  /// COLD PATH — rebinds this instance in place (zero copy).');
  L.push(`  ${cls} bind(ByteData bd, [int byteOffset = 0]) {`);
  L.push('    if (byteOffset < 0 || byteOffset + byteLength > bd.lengthInBytes) {');
  L.push(`      throw RangeError('${cls}.bind: window [$byteOffset..\${byteOffset + byteLength}) exceeds \${bd.lengthInBytes} bytes');`);
  L.push('    }');
  L.push('    _bd = bd;');
  L.push('    _offset = byteOffset;');
  L.push('    return this;');
  L.push('  }');
  L.push('');
  L.push('  /// Law 4 handshake — non-throwing; mirrors weft_envelope_decode().');
  L.push(`  bool validateHeader([int? avail]) {`);
  L.push('    final bd = _bd;');
  L.push('    if (bd == null) { return false; }');
  for (const f of ir.fields) {
    if (f.role === 'reserved') continue;
    if (!f.const) continue;
    const o = f.offset;
    if (f.count > 1 && f.type === 'u8' && f.const.kind === 'bytes') {
      for (let i = 0; i < f.const.bytes.length; i++) {
        const ch = String.fromCharCode(f.const.bytes[i]);
        L.push(`    if (bd.getUint8(_offset + ${o + i}) != ${f.const.bytes[i]}) { return false; } // "${ch}"`);
      }
    } else if (f.type === 'u8' || f.type === 'bool') {
      L.push(`    if (bd.getUint8(_offset + ${o}) != ${f.const.value}) { return false; }`);
    } else {
      const a = DART_ACCESS[f.type];
      let lit;
      if (typeof f.const.value === 'bigint') {
        const v = f.const.value;
        if (v > 0xFFFFFFFFn) {
          lit = `((0x${(v >> 32n).toString(16).toUpperCase()} << 32) | 0x${(v & 0xFFFFFFFFn).toString(16).toUpperCase()})`;
        } else {
          lit = `0x${v.toString(16).toUpperCase()}`;
        }
      } else {
        lit = JSON.stringify(f.const.value);
      }
      L.push(`    if (bd.${a.get}(_offset + ${o}${a.le ? ', Endian.little' : ''}) != ${lit}) { return false; }`);
    }
  }
  if (hasHeaderSize && hasPayloadLen) {
    const hs = ir.fields.find((f) => f.role === 'headerSize');
    const pl = ir.fields.find((f) => f.role === 'payloadLen');
    const hsA = DART_ACCESS[hs.type];
    const plA = DART_ACCESS[pl.type];
    L.push('    if (avail != null) {');
    L.push(`      final hs = bd.${hsA.get}(_offset + ${hs.offset}${hsA.le ? ', Endian.little' : ''});`);
    L.push(`      final pl = bd.${plA.get}(_offset + ${pl.offset}${plA.le ? ', Endian.little' : ''});`);
    L.push('      if (hs < minHeaderSize || hs > avail) { return false; }');
    L.push('      if (pl > avail - hs) { return false; } // payload bytes must exist (kernel SHORT)');
    L.push('    }');
  }
  L.push('    return true;');
  L.push('  }');

  for (const f of fields) {
    const ident = safeIdent(camel(f.name), 'dart');
    const o = f.offset;
    const isArray = f.count > 1;
    const a = DART_ACCESS[f.type];
    const stride = Math.round(f.size / f.count);
    const leArg = a.le ? ', Endian.little' : '';
    const Cap = ident[0].toUpperCase() + ident.slice(1);
    L.push('');
    if (f.doc) for (const l of docComment(f.doc, '  /// ')) L.push(l);
    L.push(`  /// @byteOffset 0x${o.toString(16).padStart(2, '0')} (${f.type}${isArray ? `[${f.count}]` : ''})`);
    if (!isArray) {
      const getRaw = `bd.${a.get}(_offset + ${o}${leArg})`;
      L.push(`  ${DART_SCALAR[f.type]} get ${ident} {`);
      L.push('    final bd = _bd!;');
      L.push(`    return ${dartValueExpr(f.type, getRaw)};`);
      L.push('  }');
      if (!f.const) {
        L.push('');
        L.push(`  set ${ident}(${DART_SCALAR[f.type]} value) {`);
        L.push('    final bd = _bd!;');
        L.push(`    bd.${a.set}(_offset + ${o}${leArg ? ', ' : ''}${dartStoreExpr(f.type)}${leArg.replace(', Endian.little', ', Endian.little')});`);
        L.push('  }');
        L.push('');
        L.push('  /// Fluent mutator — returns this, allocates nothing (Law 1).');
        L.push(`  ${cls} with${Cap}(${DART_SCALAR[f.type]} value) {`);
        L.push(`    ${ident} = value;`);
        L.push('    return this;');
        L.push('  }');
      }
      if (f.type === 'u64' || f.type === 'i64') {
        L.push('');
        L.push(`  /// Low 32 bits of ${ident} @0x${o.toString(16)} — primitive-only path.`);
        L.push(`  int get ${ident}Lo => _bd!.${DART_ACCESS.u32.get}(_offset + ${o}, Endian.little);`);
        L.push(`  /// High 32 bits of ${ident} @0x${(o + 4).toString(16)}.`);
        L.push(`  int get ${ident}Hi => _bd!.${DART_ACCESS.u32.get}(_offset + ${o + 4}, Endian.little);`);
        if (!f.const) {
          L.push(`  set ${ident}Lo(int value) => _bd!.${DART_ACCESS.u32.set}(_offset + ${o}, value, Endian.little);`);
          L.push(`  set ${ident}Hi(int value) => _bd!.${DART_ACCESS.u32.set}(_offset + ${o + 4}, value, Endian.little);`);
          L.push(`  ${cls} with${Cap}Lo(int value) { ${ident}Lo = value; return this; }`);
          L.push(`  ${cls} with${Cap}Hi(int value) { ${ident}Hi = value; return this; }`);
        }
      }
    } else if (f.type === 'u8') {
      const getRaw = `bd.getUint8(_offset + ${o} + index * ${stride})`;
      L.push(`  ${DART_SCALAR[f.type]} get${Cap}At(int index) {`);
      L.push('    final bd = _bd!;');
      L.push(`    return ${dartValueExpr(f.type, getRaw)};`);
      L.push('  }');
      if (!f.const) {
        L.push('');
        L.push(`  void set${Cap}At(int index, ${DART_SCALAR[f.type]} value) {`);
        L.push('    final bd = _bd!;');
        L.push(`    bd.setUint8(_offset + ${o} + index * ${stride}, ${dartStoreExpr(f.type)});`);
        L.push('  }');
        L.push(`  ${cls} with${Cap}At(int index, ${DART_SCALAR[f.type]} value) {`);
        L.push(`    set${Cap}At(index, value);`);
        L.push('    return this;');
        L.push('  }');
      }
    } else {
      const getRaw = `bd.${a.get}(_offset + ${o} + index * ${stride}${leArg})`;
      L.push(`  ${DART_SCALAR[f.type]} get${Cap}At(int index) {`);
      L.push('    final bd = _bd!;');
      L.push(`    return ${dartValueExpr(f.type, getRaw)};`);
      L.push('  }');
      if (!f.const) {
        L.push('');
        L.push(`  void set${Cap}At(int index, ${DART_SCALAR[f.type]} value) {`);
        L.push('    final bd = _bd!;');
        L.push(`    bd.${a.set}(_offset + ${o} + index * ${stride}, ${dartStoreExpr(f.type)}${leArg});`);
        L.push('  }');
        L.push(`  ${cls} with${Cap}At(int index, ${DART_SCALAR[f.type]} value) {`);
        L.push(`    set${Cap}At(index, value);`);
        L.push('    return this;');
        L.push('  }');
      }
    }
  }
  L.push('}');
  return L.join('\n') + '\n';
}

/** @returns {{files: Map<string,string>, base: string}} */
export function generateDart(ir, { irPath = 'ir.json' } = {}) {
  const base = `${snake(ir.name)}.dart`;
  const files = new Map([[base, renderDart(ir, irPath)]]);
  return { files, base };
}

// CLI
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
  const { files } = generateDart(ir, { irPath });
  for (const [name, content] of [...files].sort()) {
    const p = join(outDir, name);
    writeFileSync(p, content);
    console.error(`wrote ${p}`);
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main();
}
