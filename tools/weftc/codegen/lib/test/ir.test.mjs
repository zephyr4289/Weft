// Smoke + validation-matrix test for the weftc IR loader (node --test).
// Law 4 spirit for the codegen itself: the gatekeeper is tested.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { loadIr, parseIrJson, IrError } from '../ir.mjs';
import { sizeOf, TYPES } from '../types.mjs';
import { pascal, camel, snake, screaming, safeIdent } from '../names.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const FIXTURES = join(HERE, '..', '..', '..', 'schema', 'fixtures');

function fixture(name) {
  return JSON.parse(readFileSync(join(FIXTURES, `${name}.json`), 'utf8'));
}

test('loads weft_envelope fixture with resolved consts', () => {
  const ir = loadIr(fixture('weft_envelope'));
  assert.equal(ir.name, 'WeftEnvelope');
  assert.equal(ir.byteLength, 16);
  assert.equal(ir.endian, 'little');
  const magic = ir.fields[0];
  assert.equal(magic.const.kind, 'bytes');
  assert.deepEqual(magic.const.bytes, [0x57, 0x45, 0x46, 0x54]); // "WEFT"
  const version = ir.fields[1];
  assert.equal(version.const.kind, 'int');
  assert.equal(version.const.value, 1);
});

test('loads telemetry fixture with u64 schemaId as BigInt', () => {
  const ir = loadIr(fixture('telemetry_frame'));
  assert.equal(ir.schemaId, 0x8f4c1120a9b30012n);
  const sid = ir.fields[0];
  assert.equal(sid.const.kind, 'int');
  assert.equal(sid.const.value, 0x8f4c1120a9b30012n);
  assert.equal(sizeOf('f32', 3), 12);
  assert.equal(sid.size, 8);
});

test('header-less imu fixture: no consts, bounds-only validate path', () => {
  const ir = loadIr(fixture('imu_sample'));
  assert.ok(ir.fields.every((f) => f.const === null));
  assert.equal(ir.byteLength, 32);
});

test('rejects big-endian layouts (Law 2 is structural)', () => {
  const doc = fixture('imu_sample');
  doc.schema.endian = 'big';
  assert.throws(() => loadIr(doc), IrError);
  delete doc.schema.endian;
  assert.throws(() => loadIr(doc), IrError); // absent is also a rejection
});

test('rejects unknown keys, bad version, overlapping and misaligned fields', () => {
  const doc = fixture('imu_sample');

  const sneak = structuredClone(doc);
  sneak.evil = true;
  assert.throws(() => loadIr(sneak), /unknown key "evil"/);

  const old = structuredClone(doc);
  old.irVersion = 2;
  assert.throws(() => loadIr(old), /unsupported irVersion/);

  const overlap = structuredClone(doc);
  overlap.schema.fields[1].offset = 4; // accel would straddle timestampNs
  assert.throws(() => loadIr(overlap), /overlap/);

  const misaligned = structuredClone(doc);
  misaligned.schema.byteLength = 40;
  misaligned.schema.fields = [
    misaligned.schema.fields[0],                    // timestampNs u64 @0
    { name: 'beta', type: 'f64', offset: 12 },      // in-bounds, disjoint, NOT 8-aligned
  ];
  assert.throws(() => loadIr(misaligned), /not 8-byte aligned/);

  const oob = structuredClone(doc);
  oob.schema.fields.push({ name: 'tail', type: 'u64', offset: 28 });
  assert.throws(() => loadIr(oob), /exceeds byteLength/);
});

test('rejects malformed consts', () => {
  const doc = fixture('telemetry_frame');
  const bad = structuredClone(doc);
  bad.schema.fields[0].const = 'not-hex';
  assert.throws(() => loadIr(bad), IrError);
  const bigU16 = structuredClone(fixture('weft_envelope'));
  bigU16.schema.fields[1].const = 70000; // version is u16
  assert.throws(() => loadIr(bigU16), /out of u16 range/);
});

test('parseIrJson surfaces JSON syntax errors as IrError', () => {
  assert.throws(() => parseIrJson('{oops'), IrError);
});

test('names: casing pipeline + reserved-word avoidance', () => {
  assert.equal(pascal('gps_coordinates'), 'GpsCoordinates');
  assert.equal(camel('GpsCoordinates'), 'gpsCoordinates');
  assert.equal(snake('timestampNs'), 'timestamp_ns');
  assert.equal(screaming('headerSize'), 'HEADER_SIZE');
  assert.equal(safeIdent('class', 'ts'), 'class_');
  assert.equal(safeIdent('extension', 'swift'), 'extension_');
  assert.equal(safeIdent('with', 'dart'), 'with_');
  assert.equal(safeIdent('velocity', 'python'), 'velocity');
});

test('TYPES: sizes and alignments match the ABI table', () => {
  assert.equal(TYPES.u64.size, 8);
  assert.equal(TYPES.f64.align, 8);
  assert.equal(TYPES.u16.size, 2);
  assert.equal(sizeOf('u8', 4), 4);
});
