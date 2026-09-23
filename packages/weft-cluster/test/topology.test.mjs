// topology.test.mjs — MembershipTable seqlock + GossipEngine SWIM semantics.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  MembershipTable, GossipEngine, NodeHandle, NODE_FLAG_ALIVE, NODE_FLAG_LEAVING,
} from '../src/topology.js';
import { WC } from '../src/errors.js';

test('membership: upsert + hash-index lookup round-trip', () => {
  const t = new MembershipTable(16);
  t.upsert({ nodeId: 7, addr: 0x0100007f, gossipPort: 5001, flags: NODE_FLAG_ALIVE,
    lastSeenMs: 100, incarnation: 0, dataPort: 6001, topicCount: 3 });
  t.upsert({ nodeId: 123456, addr: 0x0a000001, gossipPort: 5002, flags: NODE_FLAG_ALIVE,
    lastSeenMs: 200, incarnation: 2, dataPort: 6002, topicCount: 0 });
  const out = new NodeHandle();
  assert.equal(t.lookupInto(7, out), WC.WC_OK);
  assert.equal(out.nodeId, 7);
  assert.equal(out.gossipPort, 5001);
  assert.equal(out.dataPort, 6001);
  assert.equal(out.topicCount, 3);
  assert.equal(t.lookupInto(123456, out), WC.WC_OK);
  assert.equal(out.incarnation, 2);
  assert.equal(out.lastSeenMs, 200);
  assert.equal(t.lookupInto(999, out), WC.WC_E_UNKNOWN_NODE);
});

test('membership: open addressing survives heavy collisions', () => {
  const t = new MembershipTable(64);
  for (let i = 1; i <= 40; i++) {
    t.upsert({ nodeId: i * 8, addr: 0, gossipPort: 5000 + i, flags: NODE_FLAG_ALIVE,
      lastSeenMs: i, incarnation: 0, dataPort: 0, topicCount: 0 });
  }
  const out = new NodeHandle();
  for (let i = 1; i <= 40; i++) {
    assert.equal(t.lookupInto(i * 8, out), WC.WC_OK, `node ${i * 8}`);
    assert.equal(out.gossipPort, 5000 + i);
  }
  assert.equal(t.lookupInto(3, out), WC.WC_E_UNKNOWN_NODE);
});

test('gossip: 3 engines converge to a full membership view (socketless)', () => {
  const wires = [];
  const mk = (id, port) => new GossipEngine({
    nodeId: id, gossipPort: port, fanout: 4,
    send: (bytes, target) => {
      // loop the datagram straight into the target engine
      const w = wires.find((w) => w.port === target.port);
      if (w) w.engine.onDatagram(bytes, { address: '127.0.0.1', port });
    },
  });
  const e1 = mk(1, 5101), e2 = mk(2, 5102), e3 = mk(3, 5103);
  wires.push({ port: 5101, engine: e1 }, { port: 5102, engine: e2 }, { port: 5103, engine: e3 });
  for (const e of [e1, e2, e3]) for (const o of [e2, e3, e1]) if (o !== e) e.join('127.0.0.1', o.gossipPort);
  // Two full rounds of gossip — convergence.
  for (let r = 0; r < 4; r++) for (const e of [e1, e2, e3]) e.tick();
  for (const e of [e1, e2, e3]) {
    const ids = e.members().map((m) => m.nodeId).sort();
    assert.deepEqual(ids, [1, 2, 3], `engine ${e.nodeId} sees everyone`);
  }
  // The seqlock mirror agrees with the authoritative view.
  const out = new NodeHandle();
  assert.equal(e1.table.lookupInto(3, out), WC.WC_OK);
  assert.equal(out.gossipPort, 5103);
});

test('gossip: stale incarnation ignored, higher incarnation wins', () => {
  let now = 1000;
  const clock = () => now;
  const e1 = new GossipEngine({ nodeId: 1, gossipPort: 5201, now: clock,
    send: (b, t) => { if (t.port === 5202) e2.onDatagram(b, { address: '127.0.0.1', port: 5201 }); } });
  const e2 = new GossipEngine({ nodeId: 2, gossipPort: 5202, now: clock,
    send: (b, t) => { if (t.port === 5201) e1.onDatagram(b, { address: '127.0.0.1', port: 5202 }); } });
  e1.join('127.0.0.1', 5202); e2.join('127.0.0.1', 5201);
  e1.tick(); e2.tick(); // learn each other
  assert.equal(e1.table.size(), 2);
  // Replayed entry with LOWER incarnation must not downgrade.
  const me2 = e2.table.view.get(2);
  me2.incarnation = 5;
  e2.table._publishAll();
  e2.tick(); // announces incarnation 5
  assert.equal(e1.table.view.get(2).incarnation, 5);
  now = 1100;
  e2.tick(); // same incarnation, fresh last_seen
  assert.equal(e1.table.view.get(2).lastSeenMs, 1100);
});

test('gossip: LEAVING is sticky at equal incarnation, revive needs higher', () => {
  const events = [];
  const e1 = new GossipEngine({ nodeId: 1, gossipPort: 5301,
    send: () => {}, onEvent: (ev) => events.push(ev) });
  // Node 2 announces LEAVING at incarnation 3.
  e1._merge({ nodeId: 2, addr: 0, gossipPort: 5302, dataPort: 0,
    flags: NODE_FLAG_LEAVING, lastSeenMs: 10, incarnation: 3 }, 10);
  assert.ok(e1.table.view.get(2).flags & NODE_FLAG_LEAVING);
  // Same incarnation ALIVE cannot revive.
  e1._merge({ nodeId: 2, addr: 0, gossipPort: 5302, dataPort: 0,
    flags: NODE_FLAG_ALIVE, lastSeenMs: 20, incarnation: 3 }, 20);
  assert.ok(e1.table.view.get(2).flags & NODE_FLAG_LEAVING);
  // Higher incarnation revives.
  e1._merge({ nodeId: 2, addr: 0, gossipPort: 5302, dataPort: 0,
    flags: NODE_FLAG_ALIVE, lastSeenMs: 30, incarnation: 4 }, 30);
  assert.equal(e1.table.view.get(2).flags & NODE_FLAG_LEAVING, 0);
});

test('gossip: SWIM refutation — falsely-leaving node bumps incarnation', () => {
  let now = 100;
  const clock = () => now;
  const e1 = new GossipEngine({ nodeId: 1, gossipPort: 5401, now: clock, send: () => {} });
  const me = e1.table.view.get(1);
  const incBefore = me.incarnation;
  // A gossip entry claims node 1 is LEAVING at incarnation 7.
  e1.onDatagram(buildWgs1ForTest([{ nodeId: 1, entryFlags: NODE_FLAG_LEAVING, incarnation: 7 }]),
    { address: '127.0.0.1', port: 999 });
  assert.equal(me.incarnation, 8, 'refutation bumps incarnation past the claim');
  assert.equal(me.flags & NODE_FLAG_LEAVING, 0, 'and stays ALIVE');
  assert.ok(me.incarnation > incBefore);
  now = 101;
});

test('gossip: silent nodes suspect then evicted (fake clock)', () => {
  let now = 1000;
  const clock = () => now;
  const events = [];
  const e1 = new GossipEngine({ nodeId: 1, gossipPort: 5501, now: clock,
    suspectMs: 500, evictMs: 1000, send: () => {},
    onEvent: (ev) => events.push(ev.type + ':' + ev.nodeId) });
  e1._merge({ nodeId: 2, addr: 0, gossipPort: 5502, dataPort: 0,
    flags: NODE_FLAG_ALIVE, lastSeenMs: 1000, incarnation: 1 }, 1000);
  now = 1300; e1.tick(); // still fresh
  assert.equal(e1.table.size(), 2);
  now = 1700; e1.tick(); // > suspectMs, < evictMs — suspected but retained
  assert.equal(e1.table.size(), 2);
  now = 2200; e1.tick(); // > evictMs — evicted
  assert.equal(e1.table.size(), 1);
  assert.ok(events.includes('evict:2'));
});

test('gossip: graceful leave announces and propagates LEAVING', () => {
  const seen = [];
  const e2 = new GossipEngine({ nodeId: 2, gossipPort: 5602,
    send: () => {}, onEvent: (ev) => seen.push(ev) });
  const e1 = new GossipEngine({ nodeId: 1, gossipPort: 5601,
    send: (bytes, target) => e2.onDatagram(bytes, { address: '127.0.0.1', port: 5602 }) });
  e1.join('127.0.0.1', 5602);
  e1.tick(); e2.tick(); // mutual discovery
  assert.equal(e2.table.size(), 2);
  e1.leave(); // LEAVING burst straight to e2
  assert.ok(e2.table.view.get(1).flags & NODE_FLAG_LEAVING, 'peer marked LEAVING');
});

// Minimal hand-rolled WGS1 builder for refutation tests (normative layout).
import { encodeWgs1Into, writeGossipEntryInto } from '../src/wire.js';
function buildWgs1ForTest(entries) {
  const dv = new DataView(new ArrayBuffer(48 + entries.length * 32));
  let off = encodeWgs1Into(dv, 0, { senderNode: 99, entryCount: entries.length,
    roundLo: 1, roundHi: 0, tsLo: 0, tsHi: 0, bootLo: 0, bootHi: 0, senderFlags: 1 });
  for (const e of entries) {
    off = writeGossipEntryInto(dv, off, { nodeId: e.nodeId, addr: 0,
      gossipPort: 0, entryFlags: e.entryFlags, lastSeenLo: 0, lastSeenHi: 0,
      incarnation: e.incarnation, dataPort: 0 });
  }
  return new Uint8Array(dv.buffer);
}
