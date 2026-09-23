// test/ring.test.mjs — seqlock ring: create/attach, commit paths, acquire
// paths, torn-read simulation, overrun, wait loop, SAB smoke (Law 1/2/4).
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  WeftTensorRing, LayoutError, DLPackCode, OFF_PRODUCER_SEQ, SOFF_SLOT_FLAGS,
  SLOT_FLAG_COMMITTED, SLOT_HEADER_SIZE,
} from '../src/index.js';

const F32 = { code: DLPackCode.FLOAT, bits: 32 };

test('create + typed commit + acquireLatest returns exact values, zero surprises', () => {
  const ring = WeftTensorRing.create({
    slotCount: 4, payloadCap: 24, dtype: F32, shape: [2, 3], schemaId: 42,
  });
  assert.equal(ring.producerSeq, 0);
  assert.equal(ring.acquireLatest(), null, 'empty ring -> null');

  const src = new Float32Array([1.5, -2.25, 0.125, 64.0, 0.5, 3.0]);
  const seq = ring.commit(src, { ts: 1_000_000n, fourcc: 'F32 ' });
  assert.equal(seq, 1);

  const v = ring.acquireLatest();
  assert.notEqual(v, null);
  assert.equal(v.seq, 1);
  assert.equal(v.timestampNs, 1_000_000);
  assert.equal(v.fourcc, 'F32 ');
  assert.equal(v.rank, 2);
  assert.deepEqual(Array.from(v.shape.slice(0, 2)), [2, 3]);
  assert.deepEqual(Array.from(v.stridesElems.slice(0, 2)), [3, 1]);
  // exact IEEE-754 reads through both the flat accessor and the payload view
  for (let i = 0; i < 6; i++) assert.equal(v.getF32(i), src[i]);
  const view = v.payloadView();
  const f32v = new Float32Array(view.buffer, view.byteOffset, 6);
  assert.deepEqual(Array.from(f32v), Array.from(src));
  // re-acquire without new frame keeps returning the same latest frame
  assert.equal(ring.acquireLatest().seq, 1);
});

test('byte commit path (Uint8Array) with short payload (bounded loop, no throw)', () => {
  const ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 16, dtype: { code: DLPackCode.UINT, bits: 8 },
    shape: [16],
  });
  const bytes = Uint8Array.from([1, 2, 3]); // short on purpose
  const seq = ring.commit(bytes, { timestampNs: 5 });
  assert.equal(seq, 1);
  const v = ring.acquireLatest();
  assert.equal(v.payloadLength, 3);
  const out = new Uint8Array(3);
  assert.equal(v.copyPayloadInto(out), 3);
  assert.deepEqual(Array.from(out), [1, 2, 3]);
});

test('acquireLatest(lastSeq) returns null when nothing new; acquireFrame bounds', () => {
  const ring = WeftTensorRing.create({
    slotCount: 4, payloadCap: 4, dtype: { code: DLPackCode.UINT, bits: 8 }, shape: [4],
  });
  for (let i = 1; i <= 3; i++) ring.commit(Uint8Array.of(i, i, i, i));
  assert.equal(ring.acquireLatest(3), null, 'nothing newer than 3');
  assert.notEqual(ring.acquireLatest(2), null);
  assert.equal(ring.acquireFrame(2).getU8(0), 2);
  assert.equal(ring.acquireFrame(4), null, 'future seq');
  assert.equal(ring.acquireFrame(0), null);
});

test('overrun: slotCount=2, 5 commits -> old seq gone, latest correct', () => {
  const ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 4, dtype: { code: DLPackCode.UINT, bits: 8 }, shape: [4],
  });
  for (let i = 1; i <= 5; i++) ring.commit(Uint8Array.of(i, i, i, i));
  assert.equal(ring.producerSeq, 5);
  assert.equal(ring.acquireFrame(1), null, 'seq 1 lapped');
  assert.equal(ring.acquireFrame(4).getU8(0), 4);
  const latest = ring.acquireLatest();
  assert.equal(latest.seq, 5);
  assert.equal(latest.getU8(0), 5);
  assert.ok(ring.stats.overruns >= 1, 'overrun accounted');
});

test('torn read: uncommitted slot is invisible, then visible after commit marker', () => {
  const ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 4, dtype: { code: DLPackCode.UINT, bits: 8 }, shape: [4],
  });
  ring.commit(Uint8Array.of(9, 9, 9, 9));
  // Simulate a producer mid-write on the NEXT slot: magic + header fields
  // written with flags=0 (torn marker) — per LAYOUT-V1 §4 steps 1-3.
  const dv = ring._dv;
  const base = ring.layout.headerSize + 1 * ring.layout.slotStride;
  dv.setUint8(base + 0, 0x57); dv.setUint8(base + 1, 0x46);
  dv.setUint8(base + 2, 0x52); dv.setUint8(base + 3, 0x4d); // "WFRM"
  dv.setUint32(base + SOFF_SLOT_FLAGS, 0, true); // torn marker
  ring._dv.setUint32(base + 4, 4, true); // payload_len
  ring._dv.setUint32(base + 8, 2, true); // seq=2
  ring._dv.setUint32(base + 16, 77, true); // ts
  ring._futex[1] = 0;
  Atomics.store(ring._futex, 0, 2); // publish seq=2 (hi=0)
  ring.stats.tornReads = 0;
  const during = ring.acquireLatest();
  assert.equal(during, null, 'uncommitted slot must be invisible (seqlock)');
  assert.ok(ring.stats.tornReads >= 1, 'torn read accounted');
  // Producer finishes: payload + commit marker
  ring._slot8[1].set(Uint8Array.of(7, 7, 7, 7));
  dv.setUint32(base + SOFF_SLOT_FLAGS, SLOT_FLAG_COMMITTED, true);
  const after = ring.acquireLatest();
  assert.notEqual(after, null);
  assert.equal(after.seq, 2);
  assert.equal(after.getU8(0), 7);
  assert.equal(after.timestampNs, 77);
});

test('zero-copy begin/finish path matches convenience commit', () => {
  const ring = WeftTensorRing.create({ slotCount: 2, payloadCap: 32, dtype: F32, shape: [8] });
  const h = ring.beginCommit();
  const f = h.payloadTyped;
  for (let i = 0; i < 8; i++) f[i] = i * 0.5;
  const seq = ring.finishCommit(h, 32, { ts: 10n });
  assert.equal(seq, 1);
  const v = ring.acquireLatest();
  assert.equal(v.getF32(3), 1.5);
  assert.equal(v.timestampNs, 10);
});

test('commit rejects wrong dtype / oversized payloads (fail-closed)', () => {
  const ring = WeftTensorRing.create({ slotCount: 2, payloadCap: 4, dtype: F32, shape: [1] });
  // NOTE: the 64B-aligned stride pads the requested cap — use the ACTUAL cap.
  assert.throws(() => ring.commit(new Int32Array(1)), (e) =>
    e instanceof LayoutError && e.code === 'WTR1_COMMIT_DTYPE');
  assert.throws(() => ring.commit(new Float32Array(ring.payloadCap / 4 + 1), {}), (e) =>
    e instanceof LayoutError && e.code === 'WTR1_COMMIT_RANGE');
  const h = ring.beginCommit();
  assert.throws(() => ring.finishCommit(h, ring.payloadCap + 1), (e) => e.code === 'WTR1_COMMIT_RANGE');
});

test('create() input validation is fail-closed', () => {
  const base = { slotCount: 2, payloadCap: 4, dtype: F32, shape: [1] };
  assert.throws(() => WeftTensorRing.create({ ...base, slotCount: 1 }), (e) => e.code === 'WTR1_BAD_SLOT_COUNT');
  assert.throws(() => WeftTensorRing.create({ ...base, payloadCap: 0 }), (e) => e.code === 'WTR1_BAD_PAYLOAD_CAP');
  assert.throws(() => WeftTensorRing.create({ ...base, shape: [] }), (e) => e.code === 'WTR1_BAD_RANK');
  assert.throws(() => WeftTensorRing.create({ ...base, shape: [0] }), (e) => e.code === 'WTR1_BAD_SHAPE');
  assert.throws(() => WeftTensorRing.create({ ...base, shape: [1, 2, 3] }), (e) => e.code === 'WTR1_TOO_SMALL');
});

test('waitForNewFrame resolves via injected scheduler; timeout path returns null', async () => {
  const ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 4, dtype: { code: DLPackCode.UINT, bits: 8 }, shape: [4],
  });
  ring.commit(Uint8Array.of(1, 1, 1, 1)); // seed seq 1; watermark = 1
  let polls = 0;
  const scheduler = {
    async wait(r) {
      polls++;
      if (polls === 1) r.commit(Uint8Array.of(2, 2, 2, 2)); // seq 2 arrives
      return true;
    },
  };
  const seq = await ring.waitForNewFrame(1, 1000, scheduler);
  assert.equal(seq, 2, 'advances past the watermark');
  assert.equal(polls >= 1, true);

  const nullScheduler = { async wait() { return true; } };
  const t0 = Date.now();
  const out = await ring.waitForNewFrame(2, 30, nullScheduler);
  assert.equal(out, null, 'timeout -> null');
  assert.ok(Date.now() - t0 >= 25, 'respected the timeout');
});

test('SharedArrayBuffer smoke: create(shared) + atomics publish + acquire', () => {
  const ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 32, dtype: F32, shape: [8], shared: true,
  });
  assert.equal(ring.isShared, true);
  assert.equal(ring.layout.flags & 2, 2, 'shared flag on the wire');
  ring.commit(new Float32Array(8).fill(0.5), { ts: 1n });
  const v = ring.acquireLatest();
  assert.equal(v.getF32(0), 0.5);
  // attach() revalidation on the same shared buffer
  const attached = WeftTensorRing.attach(ring.buffer);
  assert.equal(attached.producerSeq, 1);
});

test('ring stats counters are sealed and monotonic', () => {
  const ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 4, dtype: { code: DLPackCode.UINT, bits: 8 }, shape: [4],
  });
  ring.commit(Uint8Array.of(1, 2, 3, 4));
  ring.acquireLatest();
  ring.acquireLatest(1);
  assert.equal(ring.stats.commits, 1);
  assert.equal(ring.stats.acquireCalls, 2);
  assert.ok(Object.isSealed(ring.stats), 'stats shape is sealed (no dynamic keys in hot loops)');
  ring.stats.commits = 0; // writable by design (monotonic counters), shape frozen
  assert.equal(ring.stats.commits, 0);
});
