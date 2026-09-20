#!/usr/bin/env node
// gen.mjs — weftc Swift backend (`--target=swift`).
//
// Emits, per IR struct, a `<Name>.swift` file with a `public final class`
// flyweight bound to an UnsafeRawBufferPointer:
//   - bind(_ buffer: UnsafeRawBufferPointer, byteOffset: Int = 0) — zero-copy,
//     rebindable in place; ideal for mmap regions and shared memory
//   - every multi-byte load goes through `.littleEndian` byte-swap intrinsics
//     (Law 2 holds on ANY host — big-endian included, by construction)
//   - f32[3] fields additionally map to SIMD3<Float> (Apple Silicon vector)
//   - u64 fields expose lo/hi UInt32 split accessors
//   - validateHeader(avail:) — Law 4, non-throwing, kernel decision table
//
// Swift has no runtime feedback from this sandbox: the output is verified by
// static audit (structure, offsets, LE discipline, API surface) — see
// tools/weftc/audit and codegen/swift/test.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseIrJson } from '../lib/ir.mjs';
import { banner, docComment } from '../lib/emitter.mjs';
import { camel, screaming, safeIdent } from '../lib/names.mjs';

const GENERATOR = 'weftc-managed/1.0.0';

function stableIrPath(p) {
  const i = p.lastIndexOf('tools/weftc/');
  return i >= 0 ? p.slice(i) : p;
}

function swiftType(type) {
  const m = {
    u8: 'UInt8', u16: 'UInt16', u32: 'UInt32', u64: 'UInt64',
    i8: 'Int8', i16: 'Int16', i32: 'Int32', i64: 'Int64',
    f32: 'Float', f64: 'Double', bool: 'UInt8',
  };
  return m[type];
}

function swiftStorage(type) {
  // The raw integer type that carries the bits of `type` in the wire format.
  const m = {
    u8: 'UInt8', u16: 'UInt16', u32: 'UInt32', u64: 'UInt64',
    i8: 'UInt8', i16: 'UInt16', i32: 'UInt32', i64: 'UInt64',
    f32: 'UInt32', f64: 'UInt64', bool: 'UInt8',
  };
  return m[type];
}

function needsLittleEndian(type) {
  return !(type === 'u8' || type === 'i8' || type === 'bool');
}

function decodeExpr(type, loadCall) {
  switch (type) {
    case 'u8': case 'i8': return loadCall;
    case 'u16': case 'u32': case 'u64': return `${loadCall}.littleEndian`;
    case 'i16': return `${swiftType(type)}(bitPattern: ${loadCall}.littleEndian)`;
    case 'i32': return `${swiftType(type)}(bitPattern: ${loadCall}.littleEndian)`;
    case 'i64': return `${swiftType(type)}(bitPattern: ${loadCall}.littleEndian)`;
    case 'f32': return `${swiftType(type)}(bitPattern: ${loadCall}.littleEndian)`;
    case 'f64': return `${swiftType(type)}(bitPattern: ${loadCall}.littleEndian)`;
    case 'bool': return `${loadCall} != 0`;
    default: return loadCall;
  }
}

function encodeExpr(type) {
  switch (type) {
    case 'u8': return 'value'; // single byte: no byte-swap applies
    case 'u16': case 'u32': case 'u64': return 'value.littleEndian';
    case 'i8': return 'UInt8(bitPattern: value)';
    case 'i16': return 'UInt16(bitPattern: value).littleEndian';
    case 'i32': return 'UInt32(bitPattern: value).littleEndian';
    case 'i64': return 'UInt64(bitPattern: value).littleEndian';
    case 'f32': return 'value.bitPattern.littleEndian';
    case 'f64': return 'value.bitPattern.littleEndian';
    case 'bool': return '(value ? 1 : 0)';
    default: return 'value';
  }
}

function renderSwift(ir, irPath) {
  const cls = ir.name;
  const fields = ir.fields.filter((f) => f.role !== 'reserved');
  const hasHeaderSize = ir.fields.some((f) => f.role === 'headerSize');
  const hasPayloadLen = ir.fields.some((f) => f.role === 'payloadLen');

  const L = [];
  L.push(banner({ generator: GENERATOR, irPath: stableIrPath(irPath), ir, target: 'swift' }));
  L.push('import Foundation');
  L.push('');
  L.push('/// Flyweight view over ' + cls + ' — zero-copy managed projection.');
  if (ir.doc) for (const l of docComment(ir.doc, '/// ')) L.push(l);
  L.push('///');
  L.push('/// Layout (little-endian):');
  for (const f of ir.fields) {
    const hex = f.offset.toString(16).padStart(2, '0');
    const arr = f.count > 1 ? `[${f.count}]` : '';
    L.push(`///   0x${hex}  ${f.type}${arr}  ${camel(f.name)}`);
  }
  L.push('///');
  L.push('/// Steady-state reads and writes allocate nothing (Law 1): create once,');
  L.push('/// then bind(_:) for every frame. All multi-byte access decodes with');
  L.push('/// explicit little-endian conversion (Law 2) — correct on any host.');
  L.push(`public final class ${cls} {`);

  // Statics
  L.push('    public static let byteLength: Int = ' + ir.byteLength);
  if (ir.schemaId !== null) {
    L.push(`    public static let schemaID: UInt64 = 0x${ir.schemaId.toString(16).toUpperCase()}`);
  }
  if (ir.schemaVersion !== null) {
    L.push(`    public static let schemaVersion: Int = ${ir.schemaVersion}`);
  }
  if (hasHeaderSize) {
    L.push('    /// Normative triad-1 minimum header size (03-ENVELOPE §2).');
    L.push('    public static let minHeaderSize: Int = 16');
  }
  for (const f of ir.fields) {
    const arr = f.count > 1 ? `[${f.count}]` : '';
    L.push(`    /// @byteOffset ${f.offset} (${f.type}${arr})`);
    L.push(`    public static let ${camel(f.name)}Offset: Int = ${f.offset}`);
  }
  L.push('');
  L.push('    public private(set) var base: UnsafeRawBufferPointer?');
  L.push('    /// Absolute byte offset within the underlying buffer.');
  L.push('    public private(set) var byteOffset: Int = 0');
  L.push('');
  L.push(`    public var isBound: Bool { base != nil }`);
  L.push('');
  L.push('    public init() {}');
  L.push('');
  L.push('    /// COLD PATH — bind to a raw memory window (mmap region, shared');
  L.push('    /// memory, Data.withUnsafeBytes). Zero copy; rebinds in place.');
  L.push('    @discardableResult');
  L.push(`    public func bind(_ buffer: UnsafeRawBufferPointer, byteOffset: Int = 0) -> Self {`);
  L.push(`        precondition(byteOffset >= 0 && byteOffset + Self.byteLength <= buffer.count,`);
  L.push(`                     "${cls}.bind: window [\\(byteOffset)..\\(byteOffset + Self.byteLength)) exceeds \\(buffer.count) bytes")`);
  L.push('        if let start = buffer.baseAddress {');
  L.push('            self.base = UnsafeRawBufferPointer(start: start.advanced(by: byteOffset), count: Self.byteLength)');
  L.push('        } else {');
  L.push('            self.base = UnsafeRawBufferPointer(start: nil, count: 0)');
  L.push('        }');
  L.push('        self.byteOffset = byteOffset');
  L.push('        return self');
  L.push('    }');
  L.push('');
  L.push('    /// Law 4 handshake — non-throwing; mirrors weft_envelope_decode().');
  L.push(`    public func validateHeader(avail: Int? = nil) -> Bool {`);
  L.push('        guard let b = base else { return false }');
  L.push('        _ = b');
  for (const f of ir.fields) {
    if (f.role === 'reserved') continue;
    if (!f.const) continue;
    const o = f.offset;
    if (f.count > 1 && f.type === 'u8' && f.const.kind === 'bytes') {
      for (let i = 0; i < f.const.bytes.length; i++) {
        const ch = String.fromCharCode(f.const.bytes[i]);
        L.push(`        guard b.load(fromByteOffset: ${o + i}, as: UInt8.self) == ${f.const.bytes[i]} else { return false } // "${ch}"`);
      }
    } else if (f.type === 'u8' || f.type === 'bool') {
      L.push(`        guard b.load(fromByteOffset: ${o}, as: UInt8.self) == ${f.const.value} else { return false }`);
    } else {
      const st = swiftStorage(f.type);
      let lit;
      if (typeof f.const.value === 'bigint') lit = `0x${f.const.value.toString(16).toUpperCase()}`;
      else lit = JSON.stringify(f.const.value);
      const dec = f.type === 'i8' || f.type === 'i16' || f.type === 'i32' || f.type === 'i64'
        ? `${swiftType(f.type)}(bitPattern: ${lit})`
        : f.type === 'f32' || f.type === 'f64'
          ? `${swiftType(f.type)}(bitPattern: ${lit})`
          : lit;
      L.push(`        guard b.loadUnaligned(fromByteOffset: ${o}, as: ${st}.self).littleEndian == ${st}(${dec}) else { return false }`);
    }
  }
  if (hasHeaderSize && hasPayloadLen) {
    const hs = ir.fields.find((f) => f.role === 'headerSize');
    const pl = ir.fields.find((f) => f.role === 'payloadLen');
    const hsT = swiftStorage(hs.type);
    const plT = swiftStorage(pl.type);
    L.push('        if let avail = avail {');
    L.push(`            let hs = Int(b.loadUnaligned(fromByteOffset: ${hs.offset}, as: ${hsT}.self).littleEndian)`);
    L.push(`            let pl = Int(b.loadUnaligned(fromByteOffset: ${pl.offset}, as: ${plT}.self).littleEndian)`);
    L.push('            if hs < Self.minHeaderSize || hs > avail { return false }');
    L.push('            if pl > avail - hs { return false } // payload bytes must exist (kernel SHORT)');
    L.push('        }');
  }
  L.push('        return true');
  L.push('    }');
  L.push('');
  L.push('    // MARK: raw load/store helpers (hot path; allocation-free)');
  L.push('');
  L.push('    @inline(__always)');
  L.push(`    private func raw<T>(_ type: T.Type, at offset: Int) -> T {`);
  L.push('        base!.loadUnaligned(fromByteOffset: offset, as: type)');
  L.push('    }');
  L.push('');
  L.push('    @inline(__always)');
  L.push(`    private func store<T>(_ value: T, at offset: Int) {`);
  L.push('        base!.storeBytes(of: value, toByteOffset: offset, as: T.self)');
  L.push('    }');

  for (const f of fields) {
    const ident = safeIdent(camel(f.name), 'swift');
    const o = f.offset;
    const isArray = f.count > 1;
    const st = swiftStorage(f.type);
    const stride = Math.round(f.size / f.count);
    L.push('');
    if (f.doc) for (const l of docComment(f.doc, '    /// ')) L.push(l);
    L.push(`    /// @byteOffset 0x${o.toString(16).padStart(2, '0')} (${f.type}${isArray ? `[${f.count}]` : ''})`);
    if (!isArray) {
      const loadCall = needsLittleEndian(f.type)
        ? `raw(${st}.self, at: ${o})`
        : `raw(${st}.self, at: ${o})`;
      L.push('    @inline(__always)');
      L.push(`    public var ${ident}: ${swiftType(f.type)} {`);
      L.push(`        ${decodeExpr(f.type, loadCall)}`);
      L.push('    }');
      if (!f.const) {
        L.push('');
        L.push('    @inline(__always)');
        L.push(`    public func set${ident[0].toUpperCase() + ident.slice(1)}(_ value: ${swiftType(f.type)}) {`);
        L.push(`        store(${encodeExpr(f.type)}, at: ${o})`);
        L.push('    }');
        L.push('');
        L.push('    /// Fluent mutator — returns self, allocates nothing (Law 1).');
        L.push('    @discardableResult');
        L.push(`    public func with${ident[0].toUpperCase() + ident.slice(1)}(_ value: ${swiftType(f.type)}) -> Self {`);
        L.push(`        set${ident[0].toUpperCase() + ident.slice(1)}(value)`);
        L.push('        return self');
        L.push('    }');
      }
      if (f.type === 'u64' || f.type === 'i64') {
        L.push('');
        L.push(`    /// Low 32 bits of ${ident} @0x${o.toString(16)} (UInt32 LE) — primitive-only path.`);
        L.push(`    public var ${ident}Lo: UInt32 { raw(UInt32.self, at: ${o}).littleEndian }`);
        L.push(`    /// High 32 bits of ${ident} @0x${(o + 4).toString(16)} (UInt32 LE).`);
        L.push(`    public var ${ident}Hi: UInt32 { raw(UInt32.self, at: ${o + 4}).littleEndian }`);
        if (!f.const) {
          const Cap = ident[0].toUpperCase() + ident.slice(1);
          L.push(`    public func set${Cap}Lo(_ value: UInt32) { store(value.littleEndian, at: ${o}) }`);
          L.push(`    public func set${Cap}Hi(_ value: UInt32) { store(value.littleEndian, at: ${o + 4}) }`);
          L.push('    @discardableResult');
          L.push(`    public func with${Cap}Lo(_ value: UInt32) -> Self { set${Cap}Lo(value); return self }`);
          L.push('    @discardableResult');
          L.push(`    public func with${Cap}Hi(_ value: UInt32) -> Self { set${Cap}Hi(value); return self }`);
        }
      }
    } else if (f.type === 'u8') {
      const getExpr = f.type === 'bool'
        ? `raw(UInt8.self, at: ${o} + index * ${stride}) != 0`
        : `raw(UInt8.self, at: ${o} + index * ${stride})`;
      L.push('    @inline(__always)');
      L.push(`    public func get${ident[0].toUpperCase() + ident.slice(1)}(at index: Int) -> ${f.type === 'bool' ? 'Bool' : 'UInt8'} {`);
      L.push(`        ${getExpr}`);
      L.push('    }');
      if (!f.const) {
        const Cap = ident[0].toUpperCase() + ident.slice(1);
        L.push('');
        L.push(`    public func set${Cap}(at index: Int, _ value: ${f.type === 'bool' ? 'Bool' : 'UInt8'}) {`);
        if (f.type === 'bool') {
          L.push(`        store(value ? 1 : 0, at: ${o} + index * ${stride})`);
        } else {
          L.push(`        store(value, at: ${o} + index * ${stride})`);
        }
        L.push('    }');
        L.push('    @discardableResult');
        L.push(`    public func with${Cap}(at index: Int, _ value: ${f.type === 'bool' ? 'Bool' : 'UInt8'}) -> Self {`);
        L.push(`        set${Cap}(at: index, value)`);
        L.push('        return self');
        L.push('    }');
      }
    } else {
      const Cap = ident[0].toUpperCase() + ident.slice(1);
      const getBody = decodeExpr(f.type, `raw(${st}.self, at: ${o} + index * ${stride})`);
      L.push('    @inline(__always)');
      L.push(`    public func get${Cap}(at index: Int) -> ${swiftType(f.type)} {`);
      L.push(`        ${getBody}`);
      L.push('    }');
      if (!f.const) {
        L.push('');
        L.push(`    public func set${Cap}(at index: Int, _ value: ${swiftType(f.type)}) {`);
        L.push(`        store(${encodeExpr(f.type)}, at: ${o} + index * ${stride})`);
        L.push('    }');
        L.push('    @discardableResult');
        L.push(`    public func with${Cap}(at index: Int, _ value: ${swiftType(f.type)}) -> Self {`);
        L.push(`        set${Cap}(at: index, value)`);
        L.push('        return self');
        L.push('    }');
      }
      if (f.count === 3 && f.type === 'f32') {
        // Apple SIMD mapping (Pillar 1 §2.B): f32[3] <-> SIMD3<Float>.
        L.push('');
        L.push('    /// Apple Silicon vector mapping — element decode is little-endian.');
        L.push(`    public var ${ident}SIMD: SIMD3<Float> {`);
        L.push(`        SIMD3<Float>(get${Cap}(at: 0), get${Cap}(at: 1), get${Cap}(at: 2))`);
        L.push('    }');
        if (!f.const) {
          L.push('');
          L.push('    /// Writes the vector\'s components little-endian, element order 0..<3.');
          L.push(`    public func set${Cap}(_ value: SIMD3<Float>) {`);
          L.push(`        set${Cap}(at: 0, value.x)`);
          L.push(`        set${Cap}(at: 1, value.y)`);
          L.push(`        set${Cap}(at: 2, value.z)`);
          L.push('    }');
          L.push('    @discardableResult');
          L.push(`    public func with${Cap}(_ value: SIMD3<Float>) -> Self {`);
          L.push(`        set${Cap}(value)`);
          L.push('        return self');
          L.push('    }');
        }
      }
    }
  }
  L.push('}');
  return L.join('\n') + '\n';
}

/** @returns {{files: Map<string,string>, base: string}} */
export function generateSwift(ir, { irPath = 'ir.json' } = {}) {
  const base = `${ir.name}.swift`;
  const files = new Map([[base, renderSwift(ir, irPath)]]);
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
  const { files } = generateSwift(ir, { irPath });
  for (const [name, content] of [...files].sort()) {
    const p = join(outDir, name);
    writeFileSync(p, content);
    console.error(`wrote ${p}`);
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main();
}
