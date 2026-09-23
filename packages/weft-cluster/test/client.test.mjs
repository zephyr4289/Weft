// client.test.mjs — ClusterClient over loopback-shm + UDP transports.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  ClusterClient, ShmFabric, LoopbackShmTransport, UdpTransport, FrameHandle,
  encodeWcn1Into, decodeWcn1Into, WC_FLAG_INLINE_PAYLOAD, WC,
} from '../src/index.js';

const enc = new TextEncoder();
const dec = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

test('shm: publish -> subscribe async iterator, seq monotonic, payload intact', async () => {
  const fabric = new ShmFabric();
  const pub = new ClusterClient({ nodeId: 1, transport: new LoopbackShmTransport(fabric) });
  const con = new ClusterClient({ nodeId: 2, transport: new LoopbackShmTransport(fabric) });
  await pub.start(); await con.start();

  const sub = con.subscribe('telemetry');
  // The yielded handle is REUSED (zero-copy contract): snapshot what we keep.
  const got = [];
  const consumer = (async () => {
    for await (const f of sub) {
      got.push({
        seqLo: f.seqLo, srcNode: f.srcNode,
        text: dec.decode(new Uint8Array(f.dv.buffer,
          f.dv.byteOffset + f.payloadOffset, f.payloadLen)),
      });
      if (got.length === 100) break;
    }
  })();
  // Let the consumer start waiting, then publish.
  await sleep(5);
  for (let i = 1; i <= 100; i++) {
    pub.publish('telemetry', enc.encode(`frame-${i}`));
  }
  await consumer;

  assert.equal(got.length, 100);
  for (let i = 0; i < 100; i++) {
    assert.equal(got[i].srcNode, 1);
    assert.equal(got[i].seqLo, i + 1, `seq monotonic at ${i}`);
    assert.equal(got[i].text, `frame-${i + 1}`, `payload intact at ${i}`);
  }
  assert.equal(sub.gapCount, 0);
  assert.equal(sub.staleCount, 0);
  await sub.stop(); await pub.stop(); await con.stop();
});

test('shm: two subscribers fan out independently; unsubscribe stops flow', async () => {
  const fabric = new ShmFabric();
  const pub = new ClusterClient({ nodeId: 1, transport: new LoopbackShmTransport(fabric) });
  const a = new ClusterClient({ nodeId: 2, transport: new LoopbackShmTransport(fabric) });
  const b = new ClusterClient({ nodeId: 3, transport: new LoopbackShmTransport(fabric) });
  await pub.start(); await a.start(); await b.start();

  const sa = a.subscribe('ticks');
  const sb = b.subscribe('ticks');
  const ia = sa[Symbol.asyncIterator]();
  const ib = sb[Symbol.asyncIterator]();
  const ra = [], rb = [];
  const pa = (async () => { for await (const f of { [Symbol.asyncIterator]: () => ia }) { ra.push(f); if (ra.length === 10) break; } })();
  const pb = (async () => { for await (const f of { [Symbol.asyncIterator]: () => ib }) { rb.push(f); if (rb.length === 10) break; } })();
  await sleep(5);
  for (let i = 0; i < 10; i++) pub.publish('ticks', enc.encode(`t${i}`));
  await Promise.all([pa, pb]);
  assert.equal(ra.length, 10);
  assert.equal(rb.length, 10);
  assert.equal(fabric.subscriberCount('ticks'), 2);

  await sa.stop();
  await sleep(5);
  assert.equal(fabric.subscriberCount('ticks'), 1, 'ring dropped after unsubscribe');
  await sb.stop();
  assert.equal(fabric.subscriberCount('ticks'), 0);
  await pub.stop(); await a.stop(); await b.stop();
});

test('shm: tryLatest reads newest without advancing the iterator', async () => {
  const fabric = new ShmFabric();
  const pub = new ClusterClient({ nodeId: 1, transport: new LoopbackShmTransport(fabric) });
  const con = new ClusterClient({ nodeId: 2, transport: new LoopbackShmTransport(fabric) });
  await pub.start(); await con.start();
  const sub = con.subscribe('latest');
  const h = new FrameHandle().bind(sub.ring.bytes, 0);
  assert.equal(sub.tryLatest(h), -1, 'empty ring');
  pub.publish('latest', enc.encode('first'));
  await sleep(2);
  pub.publish('latest', enc.encode('second'));
  await sleep(2);
  assert.equal(sub.tryLatest(h), WC.WC_OK);
  const bytes = new Uint8Array(h.dv.buffer, h.dv.byteOffset + h.payloadOffset, h.payloadLen);
  assert.equal(dec.decode(bytes), 'second');
  await sub.stop(); await pub.stop(); await con.stop();
});

test('Law 4: stale seq replays are dropped and counted (datagram path)', () => {
  const fabric = new ShmFabric();
  const client = new ClusterClient({ nodeId: 9, transport: new LoopbackShmTransport(fabric) });
  const sub = client.subscribe('s');
  // Fabricate two datagrams with the SAME seq (a replay) in a scratch buffer.
  const scratch = new ArrayBuffer(128);
  const dv = new DataView(scratch);
  const mk = (seq) => {
    encodeWcn1Into(dv, 0, { flags: WC_FLAG_INLINE_PAYLOAD, srcNode: 5,
      topicLo: sub.topicLo, topicHi: sub.topicHi, seqLo: seq, seqHi: 0,
      tsLo: 0, tsHi: 0, payloadLen: 4, schemaId: 1, crc: 0, rdmaKey: 0 });
    dv.setUint32(64, 0xdeadbeef, true);
  };
  const i32 = new Int32Array(scratch);
  const h = new FrameHandle().bind(scratch, 0);
  const deliver = () => {
    assert.equal(decodeWcn1Into(h, 0), WC.WC_OK, 'fixture decodes');
    client._onTransportFrame(h, 0, 68, i32);
  };
  mk(1);
  deliver();
  assert.equal(sub.ring._nextWrite, 1, 'first frame delivered');
  mk(1); // replay
  deliver();
  assert.equal(sub.ring._nextWrite, 1, 'replay dropped');
  assert.equal(sub.staleCount, 1, 'stale counted');
  mk(2);
  deliver();
  assert.equal(sub.ring._nextWrite, 2, 'fresh seq accepted');
  sub.stop();
});

test('udp: two clients, SUB control plane, live datagram pub/sub', async () => {
  const pubT = new UdpTransport();
  const conT = new UdpTransport();
  const pub = new ClusterClient({ nodeId: 1, transport: pubT, payloadMax: 512 });
  const con = new ClusterClient({ nodeId: 2, transport: conT, payloadMax: 512 });
  pubT.onFrame = (h, base, bytes, i32) => pub._onTransportFrame(h, base, bytes, i32);
  conT.onFrame = (h, base, bytes, i32) => con._onTransportFrame(h, base, bytes, i32);
  await pub.start(); await con.start();

  // Consumer connects to publisher and subscribes.
  conT.addPeer('127.0.0.1', pubT.dataPort);
  const sub = con.subscribe('ping');
  con.resubscribePeers(); // sends CTL_SUB to publisher
  const got = [];
  const consumer = (async () => {
    for await (const f of sub) {
      got.push({
        seqLo: f.seqLo,
        text: dec.decode(new Uint8Array(f.dv.buffer,
          f.dv.byteOffset + f.payloadOffset, f.payloadLen)),
      });
      if (got.length === 20) break;
    }
  })();
  await sleep(30); // let SUB arrive
  for (let i = 1; i <= 20; i++) pub.publish('ping', enc.encode(`ping-${i}`));
  await Promise.race([consumer, sleep(2000).then(() => { throw new Error('udp e2e timeout'); })]);

  assert.equal(got.length, 20);
  for (let i = 0; i < 20; i++) {
    assert.equal(got[i].seqLo, i + 1);
    assert.equal(got[i].text, `ping-${i + 1}`);
  }
  assert.equal(sub.staleCount, 0);
  await sub.stop();
  await pub.stop(); await con.stop();
});

test('udp: publisher with zero subscribers stays silent (no peers dup)', async () => {
  const t = new UdpTransport();
  const c = new ClusterClient({ nodeId: 1, transport: t });
  await t.start();
  for (let i = 0; i < 10; i++) c.publish('lonely', enc.encode('x'));
  assert.equal(c.published, 10);
  assert.equal(t.txCount, 10);
  assert.equal(t.rxCount, 0);
  await c.stop();
});
