// test/fixtures.test.mjs — committed golden fixtures parse field-by-field
// through heddle-core; manifests agree with the bytes; determinism re-check.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

import { HotPlaneView, makeLaneOut, makeHeaderOut, HPL1, MAGIC_U32 } from '../src/index.js';

const HERE = join(dirname(fileURLToPath(import.meta.url)), '../../../fixtures/heddle2');

const CASES = [
  { name: 'hpl1-basic-4x8', lanes: 4, samples: 8 },
  { name: 'hpl1-edge-1x2', lanes: 1, samples: 2 },
];

for (const c of CASES) {
  test(`fixture ${c.name}: manifest sha256 matches bytes`, () => {
    const bin = readFileSync(join(HERE, `${c.name}.bin`));
    const manifest = JSON.parse(readFileSync(join(HERE, `${c.name}.json`), 'utf8'));
    const sha = createHash('sha256').update(bin).digest('hex');
    assert.equal(sha, manifest.sha256);
    assert.equal(bin.byteLength, manifest.totalBytes);
  });

  test(`fixture ${c.name}: validates and parses field-by-field through heddle-core`, () => {
    const bin = readFileSync(join(HERE, `${c.name}.bin`));
    // view over a non-shared copy proves the plain-ArrayBuffer path
    const buf = new ArrayBuffer(bin.byteLength);
    new Uint8Array(buf).set(bin);
    const manifest = JSON.parse(readFileSync(join(HERE, `${c.name}.json`), 'utf8'));
    const v = new HotPlaneView(buf);
    assert.equal(v.laneCount, c.lanes);
    assert.equal(v.samplesPerLane, c.samples);
    // header
    const h = makeHeaderOut();
    assert.equal(v.readHeader(h), HPL1.OK);
    assert.equal(h.tickHz, manifest.header.tickHz);
    assert.equal(h.globalMin, manifest.header.globalMin);
    assert.equal(h.globalMax, manifest.header.globalMax);
    assert.equal(h.globalAvg, manifest.header.globalAvg);
    assert.equal(h.globalCurrent, manifest.header.globalCurrent);
    assert.equal(h.lastPublishNsLo, manifest.header.lastPublishNs);
    // lanes: stats + ring contents newest-first
    const out = makeLaneOut();
    const recent = new Float64Array(c.samples);
    for (let l = 0; l < c.lanes; l++) {
      assert.equal(v.readLane(l, out), HPL1.OK, `lane ${l}`);
      const m = manifest.lanes[l];
      assert.equal(out.current, m.current, `lane ${l} current`);
      assert.equal(out.min, m.min);
      assert.equal(out.max, m.max);
      assert.equal(out.avg, m.avg);
      assert.equal(out.samplesSeenLo, m.samplesSeen);
      assert.equal(out.head, m.head);
      assert.equal(out.flags, m.laneFlags);
      assert.equal(out.publishNsLo, m.lanePublishNs);
      const n = v.readRecent(l, c.samples, recent);
      assert.equal(n, c.samples);
      // newest-first ring == reversed published order (head wrapped full circle)
      for (let k = 0; k < c.samples; k++) {
        assert.equal(recent[k], m.values[m.values.length - 1 - k], `lane ${l} ring[${k}]`);
      }
    }
  });
}

test('fixture magic is the pinned HPL1 magic', () => {
  const bin = readFileSync(join(HERE, 'hpl1-basic-4x8.bin'));
  assert.equal(new DataView(new Uint8Array(bin).buffer).getUint32(0, true), MAGIC_U32);
});
