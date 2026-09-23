// router.test.mjs — ShardRouter rendezvous hashing: determinism, balance,
// minimal disruption, and the cross-language owner vector (Python mirrors it).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ShardRouter } from '../src/router.js';
import { fnv1a64 } from '../src/wire.js';
import { WC } from '../src/errors.js';

test('router: deterministic owner across calls and instances', () => {
  const r1 = new ShardRouter();
  const r2 = new ShardRouter();
  for (const id of [1, 2, 3, 4]) { r1.addNode(id); r2.addNode(id); }
  const out = {};
  for (const topic of ['telemetry', 'trades', 'video.frames', 'audio.pcm']) {
    const h = fnv1a64(topic);
    const o1 = r1.ownerOf(h);
    r1.ownerInto(h, out);
    assert.equal(out.nodeId, o1);
    assert.equal(r2.ownerOf(h), o1, `cross-instance determinism for ${topic}`);
  }
});

test('router: cross-language owner vector (node seeds 11,22,33,44 / telemetry)', async () => {
  // Frozen vector: the Python SDK must resolve the SAME owner. Recorded here
  // so ci stage 4 catches any hashing divergence between implementations.
  const r = new ShardRouter();
  for (const id of [11, 22, 33, 44]) r.addNode(id);
  const owner = r.ownerOf(fnv1a64('telemetry'));
  assert.ok([11, 22, 33, 44].includes(owner));
  // The full vector is recorded in fixtures/cluster/router_vector.json and
  // asserted byte-exactly; this test keeps the live guard.
  const fs = await import('node:fs');
  const path = new URL('../../../fixtures/cluster/router_vector.json', import.meta.url);
  const vec = JSON.parse(fs.readFileSync(path, 'utf8'));
  assert.equal(vec.algorithm, 'rendezvous-fnv1a64');
  assert.deepEqual(vec.nodes, [11, 22, 33, 44]);
  assert.equal(vec.topics.every((t) => r.ownerOf(fnv1a64(t.topic)) === t.owner), true,
    'every fixture topic resolves to the recorded owner');
});

test('router: 8-node balance within 3x envelope', () => {
  const r = new ShardRouter();
  for (let i = 1; i <= 8; i++) r.addNode(i);
  const counts = new Map();
  const N = 1000;
  for (let i = 0; i < N; i++) {
    const o = r.ownerOf(fnv1a64(`topic-${i}`));
    counts.set(o, (counts.get(o) ?? 0) + 1);
  }
  const vals = [...counts.values()];
  assert.equal(counts.size, 8, 'all nodes own something');
  assert.ok(Math.max(...vals) / Math.min(...vals) < 3,
    `share ratio ${Math.max(...vals) / Math.min(...vals)} within 3x`);
});

test('router: minimal disruption — adding 1 of 9 nodes moves ~1/9 of topics', () => {
  const before = new ShardRouter();
  for (let i = 1; i <= 8; i++) before.addNode(i);
  const owners = new Map();
  for (let i = 0; i < 1000; i++) owners.set(i, before.ownerOf(fnv1a64(`t-${i}`)));
  before.addNode(9);
  let moved = 0;
  for (const [i, owner] of owners) {
    if (before.ownerOf(fnv1a64(`t-${i}`)) !== owner) moved++;
  }
  assert.ok(moved / 1000 < 0.20, `moved ${(moved / 10).toFixed(1)}% < 20%`);
  assert.ok(moved / 1000 > 0.05, `moved ${(moved / 10).toFixed(1)}% > 5% (not zero — node 9 must own something)`);
});

test('router: unrouted topic -> WC_E_TOPIC_UNROUTED', () => {
  const r = new ShardRouter();
  const out = {};
  assert.equal(r.ownerInto(fnv1a64('x'), out), WC.WC_E_TOPIC_UNROUTED);
  assert.equal(r.ownerOf(fnv1a64('x')), null);
  r.addNode(1);
  assert.equal(r.ownerInto(fnv1a64('x'), out), WC.WC_OK);
  assert.equal(out.nodeId, 1);
});

test('router: removeNode stops routing to it; version bumps', () => {
  const r = new ShardRouter();
  r.addNode(1); r.addNode(2);
  const v1 = r.version;
  r.removeNode(1);
  assert.ok(r.version > v1);
  assert.deepEqual(r.nodes, [2]);
  assert.equal(r.ownerOf(fnv1a64('anything')), 2);
});
