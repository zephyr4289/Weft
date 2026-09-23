// weftc Swift backend — static audit suite (node --test).
//
// No Swift toolchain exists in the verification sandbox, so this backend is
// gated by a STATIC parity audit: structural presence, offset tables vs the
// IR, little-endian discipline, Law 4 surface, and byte-identical determinism
// of the generator. The unified cross-backend parity matrix lives in
// tools/weftc/audit.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { parseIrJson } from '../../lib/ir.mjs';
import { generateSwift } from '../gen.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const SWIFT_GOLDEN = join(HERE, '..', 'golden');
const FIXTURES = join(HERE, '..', '..', '..', 'schema', 'fixtures');

const SCHEMAS = ['weft_envelope', 'telemetry_frame', 'imu_sample'];
const ir = (n) => parseIrJson(readFileSync(join(FIXTURES, `${n}.json`), 'utf8'));
const swiftPath = (n) => join(SWIFT_GOLDEN, `${ir(n).name}.swift`);
const swiftSrc = (n) => readFileSync(swiftPath(n), 'utf8');

test('swift: class surface — final class, byteLength, offset constants match IR', () => {
  for (const name of SCHEMAS) {
    const s = ir(name);
    const src = swiftSrc(name);
    assert.match(src, new RegExp(`public final class ${s.name} \\{`));
    assert.match(src, new RegExp(`public static let byteLength: Int = ${s.byteLength}`));
    for (const f of s.fields) {
      const camelName = f.name.charAt(0).toLowerCase() + f.name.slice(1);
      const re = new RegExp(`public static let ${camelName}Offset: Int = ${f.offset}`);
      assert.match(src, re, `${name}.${f.name} offset constant`);
    }
  }
});

test('swift: every multi-byte load/store carries explicit littleEndian', () => {
  for (const name of SCHEMAS) {
    const s = ir(name);
    const src = swiftSrc(name);
    const lines = src.split('\n');
    for (const [i, line] of lines.entries()) {
      const t = line.trim();
      if (t.startsWith('///') || t.startsWith('//')) continue;
      // loadUnaligned(...as: UInt16/32/64.self) without .littleEndian is a bug
      if (/loadUnaligned\([^)]*as: (UInt16|UInt32|UInt64)\.self\)/.test(t) && !/\.littleEndian/.test(t)) {
        assert.fail(`${name}.swift:${i + 1} missing .littleEndian: ${t}`);
      }
    }
    // raw `store(value, at:` (no LE conversion) is legal ONLY for single-byte
    // members (u8/i8/bool). Exact-count check keeps multi-byte stores honest.
    const rawStores = (src.match(/store\((value|value \? 1 : 0|UInt8\(bitPattern: value\)), at:/g) || []).length;
    let singleByteWritables = 0;
    for (const f of s.fields) {
      if (f.role === 'reserved' || f.const) continue;
      if (['u8', 'i8', 'bool'].includes(f.type)) singleByteWritables += 1; // scalar or array setter: one store line each
    }
    assert.equal(rawStores, singleByteWritables,
      `${name}.swift: ${rawStores} raw stores vs ${singleByteWritables} single-byte writables`);
  }
});

test('swift: Law 4 validateHeader(avail:) present; envelope has kernel semantics', () => {
  for (const name of SCHEMAS) {
    assert.match(swiftSrc(name), /public func validateHeader\(avail: Int\? = nil\) -> Bool/);
  }
  const env = swiftSrc('weft_envelope');
  assert.match(env, /if hs < Self\.minHeaderSize \|\| hs > avail \{ return false \}/);
  assert.match(env, /if pl > avail - hs \{ return false \} \/\/ payload bytes must exist/);
  const tel = swiftSrc('telemetry_frame');
  assert.match(tel, /== UInt64\(0x8F4C1120A9B30012\)/);
});

test('swift: SIMD3<Float> mapping emitted for f32[3] with LE element decode', () => {
  const tel = swiftSrc('telemetry_frame');
  assert.match(tel, /public var velocitySIMD: SIMD3<Float>/);
  assert.match(tel, /public func setVelocity\(_ value: SIMD3<Float>\)/);
});

test('swift: u64 lo/hi split accessors emitted', () => {
  const tel = swiftSrc('telemetry_frame');
  assert.match(tel, /public var timestampNsLo: UInt32/);
  assert.match(tel, /public var timestampNsHi: UInt32/);
  assert.match(tel, /public func withTimestampNsLo\(_ value: UInt32\) -> Self/);
});

test('swift: reserved fields have no accessors', () => {
  const tel = swiftSrc('telemetry_frame');
  assert.ok(!/var reserved/.test(tel));
  assert.ok(!/func setReserved/.test(tel));
});

test('swift: bind is UnsafeRawBufferPointer-based (zero copy, rebindable)', () => {
  for (const name of SCHEMAS) {
    assert.match(swiftSrc(name),
      /public func bind\(_ buffer: UnsafeRawBufferPointer, byteOffset: Int = 0\) -> Self/);
  }
});

test('swift: determinism — regeneration byte-identical to committed golden', () => {
  for (const name of SCHEMAS) {
    const s = ir(name);
    const out = generateSwift(s, { irPath: `tools/weftc/schema/fixtures/${name}.json` });
    for (const [fname, content] of out.files) {
      assert.equal(readFileSync(join(SWIFT_GOLDEN, fname), 'utf8'), content, fname);
    }
  }
});
