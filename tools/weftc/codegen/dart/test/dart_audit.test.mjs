// weftc Dart backend — static audit suite (node --test).
//
// No Dart toolchain exists in the verification sandbox: this backend is gated
// by a STATIC parity audit (structure, offset tables vs the IR, LE
// discipline, Law 4 surface, determinism). The unified cross-backend parity
// matrix lives in tools/weftc/audit.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { parseIrJson } from '../../lib/ir.mjs';
import { generateDart } from '../gen.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const DART_GOLDEN = join(HERE, '..', 'golden');
const FIXTURES = join(HERE, '..', '..', '..', 'schema', 'fixtures');

const SCHEMAS = ['weft_envelope', 'telemetry_frame', 'imu_sample'];
const ir = (n) => parseIrJson(readFileSync(join(FIXTURES, `${n}.json`), 'utf8'));
const snakeName = (n) => ir(n).name.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toLowerCase();
const dartPath = (n) => join(DART_GOLDEN, `${snakeName(n)}.dart`);
const dartSrc = (n) => readFileSync(dartPath(n), 'utf8');

test('dart: dual-layer surface — ffi.Struct projection + ByteData flyweight', () => {
  for (const name of SCHEMAS) {
    const s = ir(name);
    const src = dartSrc(name);
    assert.match(src, new RegExp(`final class ${s.name}Ffi extends ffi\\.Struct`));
    assert.match(src, new RegExp(`class ${s.name} \\{`));
    assert.match(src, /import 'dart:ffi' as ffi;/);
    assert.match(src, /import 'dart:typed_data';/);
  }
});

test('dart: ffi field declarations appear in offset order with arrays', () => {
  const src = dartSrc('telemetry_frame');
  let last = -1;
  for (const f of ['schema_id;', 'timestamp_ns;', 'velocity;', 'pressure_pa;', 'state;', 'reserved;', 'gps_coordinates;', 'payload_hash;']) {
    const i = src.indexOf(`external ffi.`);
    const j = src.indexOf(f, last + 1);
    assert.ok(j > last, `${f} must appear in offset order`);
    last = j;
  }
  assert.match(src, /@ffi\.Array\.multi\(\[3\]\)/);
  assert.match(src, /@ffi\.Array\.multi\(\[8\]\)/);
});

test('dart: every multi-byte ByteData access passes Endian.little', () => {
  const multi = /(get|set)(Uint16|Uint32|Uint64|Int16|Int32|Int64|Float32|Float64)\(/;
  for (const name of SCHEMAS) {
    const lines = dartSrc(name).split('\n');
    for (const [i, line] of lines.entries()) {
      const t = line.trim();
      if (t.startsWith('///') || t.startsWith('//')) continue;
      if (!multi.test(t)) continue;
      assert.ok(/Endian\.little\)/.test(t), `${snakeName(name)}.dart:${i + 1} missing Endian.little: ${t}`);
    }
  }
});

test('dart: offset constants match IR', () => {
  for (const name of SCHEMAS) {
    const s = ir(name);
    const src = dartSrc(name);
    for (const f of s.fields) {
      const camelName = f.name.charAt(0).toLowerCase() + f.name.slice(1);
      const re = new RegExp(`static const int ${camelName}Offset = ${f.offset};`);
      assert.match(src, re, `${name}.${f.name}`);
    }
  }
});

test('dart: Law 4 validateHeader present; envelope kernel semantics', () => {
  for (const name of SCHEMAS) {
    assert.match(dartSrc(name), /bool validateHeader\(\[int\? avail\]\)/);
  }
  const env = dartSrc('weft_envelope');
  assert.match(env, /if \(hs < minHeaderSize \|\| hs > avail\) \{ return false; \}/);
  assert.match(env, /if \(pl > avail - hs\) \{ return false; \}/);
});

test('dart: u64 schemaId literal wraps to exact bit pattern; u64 lo/hi split', () => {
  const tel = dartSrc('telemetry_frame');
  assert.match(tel, /static const int schemaId = 0x8F4C1120A9B30012;/);
  assert.match(tel, /int get timestampNsLo => _bd!\.getUint32\(_offset \+ 8, Endian\.little\);/);
  assert.match(tel, /set timestampNsHi\(int value\) => _bd!\.setUint32\(_offset \+ 12, value, Endian\.little\);/);
});

test('dart: fluent with* chainers return the view (zero alloc)', () => {
  const tel = dartSrc('telemetry_frame');
  assert.match(tel, /TelemetryFrame withTimestampNs\(int value\) \{/);
  assert.match(tel, /TelemetryFrame withTimestampNsLo\(int value\) \{/);
  assert.match(tel, /TelemetryFrame withPressurePa\(double value\) \{/);
  assert.match(tel, /TelemetryFrame withVelocityAt\(int index, double value\) \{/);
});

test('dart: reserved fields have no accessors', () => {
  const tel = dartSrc('telemetry_frame');
  assert.ok(!/int get reserved\b/.test(tel));
  assert.ok(!/set reserved\b/.test(tel));
});

test('dart: determinism — regeneration byte-identical to committed golden', () => {
  for (const name of SCHEMAS) {
    const s = ir(name);
    const out = generateDart(s, { irPath: `tools/weftc/schema/fixtures/${name}.json` });
    for (const [fname, content] of out.files) {
      assert.equal(readFileSync(join(DART_GOLDEN, fname), 'utf8'), content, fname);
    }
  }
});
