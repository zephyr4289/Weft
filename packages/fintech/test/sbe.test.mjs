// sbe.test.mjs — schema-driven SBE flyweight decoding over the golden stream.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { SBE_SCHEMA, buildSbeStream } from '../../../tests/adapters/managed/fixtures/generate.mjs';
import { SbeDecoder } from '../src/sbe.js';

const here = dirname(fileURLToPath(import.meta.url));

test('golden SBE stream: 4096 records, values exact, extensions skipped', () => {
  const schema = JSON.parse(readFileSync(join(here, '../../../tests/adapters/managed/fixtures/sbe-schema.json'), 'utf8'));
  const stream = readFileSync(join(here, '../../../tests/adapters/managed/fixtures/sbe-stream.bin'));
  const dec = new SbeDecoder(schema);
  const seen = [];
  const visit = (templateId, vec, n) => {
    seen.push(templateId);
    assert.equal(n, 3);
    // reused vector: values read before the next record overwrites them
  };
  const n = dec.process(stream, stream.length, visit);
  assert.equal(n, 4096);
  assert.equal(dec.truncated, 0);
  assert.equal(dec.skipped, 0);
  // every 8th record is the 1002 extension template (40B) — visited, skipped
  let ext = 0;
  for (const t of seen) if (t === 1002) ext++;
  assert.equal(ext, 512);
});

test('field values are exact per record (seq = i+1, bid = 100000 + i%7)', () => {
  const schema = JSON.parse(JSON.stringify(SBE_SCHEMA));
  const stream = buildSbeStream(64);
  const dec = new SbeDecoder(schema);
  let i = 0;
  const visit = (templateId, vec) => {
    assert.equal(vec[0], i + 1);            // seq
    assert.equal(vec[1], (i + 1) * 250000); // tsNs
    assert.equal(vec[2], 100000 + (i % 7)); // bidPrice
    i++;
  };
  assert.equal(dec.process(stream, stream.length, visit), 64);
  assert.equal(i, 64);
});

test('unknown templates are skipped by transport length (forward compat)', () => {
  const schema = JSON.parse(JSON.stringify(SBE_SCHEMA));
  // record with templateId 9999, blockLength 24
  const rec = Buffer.alloc(4 + 8 + 24);
  rec.writeUInt32LE(32, 0);
  rec.writeUInt16LE(24, 4);
  rec.writeUInt16LE(9999, 6);
  rec.writeUInt16LE(1, 8);
  rec.writeUInt16LE(0, 10);
  const dec = new SbeDecoder(schema);
  let visited = 0;
  assert.equal(dec.process(rec, rec.length, () => visited++), 0);
  assert.equal(dec.skipped, 1);
  assert.equal(visited, 0);
});

test('truncated stream fails closed (no partial record visits)', () => {
  const schema = JSON.parse(JSON.stringify(SBE_SCHEMA));
  const stream = buildSbeStream(8);
  const dec = new SbeDecoder(schema);
  let visited = 0;
  const n = dec.process(stream, 20, () => visited++); // cut mid-record-2
  assert.equal(visited, n); // only complete records visited
  assert.ok(dec.truncated >= 1);
});
