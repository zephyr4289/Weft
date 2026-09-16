// weft.test.ts — @weft/core kernel unit suite
//
// WHY EXISTS: The npm packages shipped with zero unit tests (D-11 evidence gap;
// only the Flutter package carried tests). This suite pins the TS kernel's
// public contract at the package boundary — envelope codec, publish/claim
// semantics, the I6 revocation handshake, and the writer-cursor API added for
// C-kernel parity (weft_w_begin / weft_w_write_payload / weft_debug_view,
// core/c/weft.h). It is NOT a replacement for the litmus suite (litmus is the
// canonical cross-language conformance gate); it is the package-level smoke
// and regression net that `pnpm --filter @weft/core test` runs in CI.
//
// Environment tag for any timing assertions: node-vitest (sandbox).

import { describe, it, expect } from 'vitest';
import {
  Weft,
  WEFT_MAGIC,
  WEFT_VERSION_1,
  PubResult,
  DecodeResult,
  envelopeEncode,
  envelopeEncodeV1,
  envelopeDecode,
  negotiate,
  pat,
  mix32,
  xorshift32,
} from '../src/index';

// ---------------------------------------------------------------------------
// Envelope codec (03-ENVELOPE §1, §2)
// ---------------------------------------------------------------------------

describe('envelope codec', () => {
  it('v1 roundtrip: encode then decode returns the same fields', () => {
    const buf = new ArrayBuffer(64);
    const dv = new DataView(buf);
    envelopeEncodeV1(dv, 0, 42, 32); // payload_len must fit avail - header_size
    const r = envelopeDecode(dv, 0, 64);
    expect(r.ok).toBe(true);
    expect(r.result).toBe(DecodeResult.Ok);
    expect(r.version).toBe(WEFT_VERSION_1);
    expect(r.headerSize).toBe(16);
    expect(r.seq).toBe(42);
    expect(r.payloadLen).toBe(32);
  });

  it('writes the WEFT magic little-endian', () => {
    const buf = new ArrayBuffer(64);
    const dv = new DataView(buf);
    envelopeEncodeV1(dv, 0, 1, 4);
    const bytes = new Uint8Array(buf);
    expect(bytes[0]).toBe(0x57); // 'W'
    expect(bytes[1]).toBe(0x45); // 'E'
    expect(bytes[2]).toBe(0x46); // 'F'
    expect(bytes[3]).toBe(0x54); // 'T'
    expect(dv.getUint32(0, true)).toBe(WEFT_MAGIC);
  });

  it('DECODE_SHORT when avail < 16', () => {
    const dv = new DataView(new ArrayBuffer(15));
    expect(envelopeDecode(dv, 0, 15).result).toBe(DecodeResult.Short);
  });

  it('DECODE_BAD_MAGIC when magic differs', () => {
    const dv = new DataView(new ArrayBuffer(32));
    envelopeEncodeV1(dv, 0, 1, 8);
    dv.setUint32(0, 0xdeadbeef, true); // corrupt magic
    expect(envelopeDecode(dv, 0, 32).result).toBe(DecodeResult.BadMagic);
  });

  it('DECODE_BAD_HEADER when header_size < 16 or > avail', () => {
    const dv = new DataView(new ArrayBuffer(32));
    envelopeEncodeV1(dv, 0, 1, 8);
    dv.setUint16(6, 12, true); // header_size < 16
    expect(envelopeDecode(dv, 0, 32).result).toBe(DecodeResult.BadHeader);
    dv.setUint16(6, 40, true); // header_size > avail
    expect(envelopeDecode(dv, 0, 32).result).toBe(DecodeResult.BadHeader);
  });

  it('DECODE_SHORT when payload_len exceeds avail - header_size', () => {
    const dv = new DataView(new ArrayBuffer(32));
    envelopeEncodeV1(dv, 0, 1, 8);
    dv.setUint32(12, 100, true); // payload_len > 32 - 16
    expect(envelopeDecode(dv, 0, 32).result).toBe(DecodeResult.Short);
  });

  it('custom-version envelope fills trailing header bytes with 0xAA (L8b convention)', () => {
    const dv = new DataView(new ArrayBuffer(48));
    envelopeEncode(dv, 0, 2, 32, 7, 8); // version=2, header_size=32
    const bytes = new Uint8Array(dv.buffer);
    for (let i = 16; i < 32; i++) expect(bytes[i]).toBe(0xaa);
    const r = envelopeDecode(dv, 0, 48);
    expect(r.ok).toBe(true);
    expect(r.version).toBe(2);
    expect(r.headerSize).toBe(32);
  });

  it('negotiate picks the highest reader version <= writer version', () => {
    expect(negotiate(2, [1, 2])).toBe(2);
    expect(negotiate(1, [1, 2])).toBe(1);
    expect(negotiate(3, [1, 2])).toBe(2);
    expect(negotiate(1, [2, 3])).toBe(0); // BIND_INCOMPATIBLE
    expect(negotiate(5, [])).toBe(0);
  });
});

// ---------------------------------------------------------------------------
// Pattern helpers (04-LITMUS §0.1, §0.2 — A5 cross-language parity)
// ---------------------------------------------------------------------------

describe('pattern helpers', () => {
  it('pat(seq, i) is deterministic and byte-valued', () => {
    expect(pat(1, 0)).toBe(pat(1, 0));
    expect(pat(1, 0)).not.toBe(pat(2, 0));
    for (let i = 0; i < 64; i++) {
      const b = pat(12345, i);
      expect(b).toBeGreaterThanOrEqual(0);
      expect(b).toBeLessThanOrEqual(255);
    }
  });

  it('mix32 stays in u32 range', () => {
    for (const x of [0, 1, 0x7fffffff, 0x80000000, 0xffffffff]) {
      const v = mix32(x);
      expect(v).toBeGreaterThanOrEqual(0);
      expect(v).toBeLessThanOrEqual(0xffffffff);
    }
  });

  it('xorshift32 reseeds state 0 and advances deterministically', () => {
    const s = { v: 0 };
    const a = xorshift32(s);
    expect(a).not.toBe(0);
    expect(xorshift32(s)).not.toBe(a);
  });
});

// ---------------------------------------------------------------------------
// Kernel: publish/claim lifecycle (02 §2, §4)
// ---------------------------------------------------------------------------

describe('Weft publish/claim', () => {
  it('null frame invariant (04-LITMUS §0.6): first claim yields a valid seq=0 frame, not garbage', () => {
    const w = new Weft(256);
    const slot = w.claim();
    expect(slot).toBeGreaterThanOrEqual(0);
    expect(slot).toBeLessThanOrEqual(2);
    expect(w.rSeq()).toBe(0);
    expect(w.rMagic()).toBe(WEFT_MAGIC);
    expect(w.rPayloadLen()).toBe(256);
  });

  it('claim before publish returns slot 0 (latest holds the null frame)', () => {
    const w = new Weft(64);
    expect(w.claim()).toBe(0);
  });

  it('publish then claim exposes the published seq and payload_len', () => {
    const w = new Weft(64);
    w.fillPayload(7, 64);
    expect(w.publish(7, 64)).toBe(PubResult.Ok);
    w.claim();
    expect(w.rSeq()).toBe(7);
    expect(w.rPayloadLen()).toBe(64);
    expect(w.verifyHeld(7, 64)).toBe(true);
  });

  it('latest-wins: rapid publishes surface only the freshest frame to the reader', () => {
    const w = new Weft(64);
    for (let seq = 1; seq <= 10; seq++) {
      w.fillPayload(seq, 64);
      w.publish(seq, 64);
    }
    w.claim();
    expect(w.rSeq()).toBe(10);
    expect(w.verifyHeld(10, 64)).toBe(true);
  });

  it('claim never fails and always yields a slot in 0..2', () => {
    const w = new Weft(64);
    for (let i = 0; i < 100; i++) {
      const slot = w.claim();
      expect(slot).toBeGreaterThanOrEqual(0);
      expect(slot).toBeLessThanOrEqual(2);
    }
  });

  it('telemetry counts publishes and claims', () => {
    const w = new Weft(64);
    for (let seq = 1; seq <= 5; seq++) {
      w.fillPayload(seq, 64);
      w.publish(seq, 64);
    }
    for (let i = 0; i < 3; i++) w.claim();
    expect(w.tPublish()).toBe(5n);
    expect(w.tClaim()).toBe(3n);
    expect(w.tDrop()).toBe(0n);
  });
});

// ---------------------------------------------------------------------------
// Writer cursor API — C-kernel parity (weft_w_begin / weft_w_write_payload)
// ---------------------------------------------------------------------------

describe('Weft writer cursor', () => {
  it('wBegin returns a live view over the working payload region', () => {
    const w = new Weft(32);
    const cursor = w.wBegin();
    expect(cursor).toBeInstanceOf(Uint8Array);
    expect(cursor.length).toBe(32);
    cursor[0] = 0xab;
    // Live view: the byte is in the underlying SAB at buf[w_work]+16.
    const slot = Atomics.load(w.ctrl, Weft.SLOT_W_WORK);
    expect(w.dv.getUint8(w.bufOffset(slot) + 16)).toBe(0xab);
  });

  it('writes through wBegin are readable by the reader after publish', () => {
    const w = new Weft(16);
    const cursor = w.wBegin();
    for (let i = 0; i < 16; i++) cursor[i] = i * 3;
    expect(w.publish(1, 16)).toBe(PubResult.Ok);
    w.claim();
    const read = w.rReadSlice(16, 16);
    for (let i = 0; i < 16; i++) expect(read[i]).toBe(i * 3);
  });

  it('wBegin caches views per slot: zero allocation per call (Law 2)', () => {
    const w = new Weft(32);
    const a = w.wBegin();
    const b = w.wBegin();
    expect(b).toBe(a); // same slot -> identical cached view object
  });

  it('the cursor rotates to a different cached view after publish', () => {
    const w = new Weft(32);
    const before = w.wBegin();
    w.publish(1, 32);
    const after = w.wBegin();
    expect(after).not.toBe(before); // w_work rotated to the old latest slot
    // And the rotation is stable: two calls agree again.
    expect(w.wBegin()).toBe(after);
  });

  it('wWritePayload roundtrips arbitrary user bytes end-to-end', () => {
    const w = new Weft(64);
    const src = new Uint8Array(64);
    for (let i = 0; i < 64; i++) src[i] = (i * 7 + 11) & 0xff;
    expect(w.wWritePayload(src)).toBe(0);
    expect(w.publish(3, 64)).toBe(PubResult.Ok);
    w.claim();
    const out = w.rReadSlice(16, 64);
    for (let i = 0; i < 64; i++) expect(out[i]).toBe(src[i]);
  });

  it('wWritePayload rejects oversize input and writes nothing (C contract)', () => {
    const w = new Weft(16);
    const before = new Uint8Array(w.wBegin()); // snapshot of current contents
    const tooBig = new Uint8Array(17);
    expect(w.wWritePayload(tooBig)).toBe(-1);
    const now = w.wBegin();
    for (let i = 0; i < 16; i++) expect(now[i]).toBe(before[i]);
  });

  it('wBeginFloat32 provides a typed cursor over the same region', () => {
    const w = new Weft(64); // 16 floats
    const f32 = w.wBeginFloat32();
    expect(f32).toBeInstanceOf(Float32Array);
    expect(f32.length).toBe(16);
    f32[0] = 1.5;
    f32[15] = -2.25;
    expect(w.publish(1, 64)).toBe(PubResult.Ok);
    w.claim();
    const out = w.rReadSlice(16, 64);
    const outF32 = new Float32Array(out.buffer, out.byteOffset, 16);
    expect(outF32[0]).toBeCloseTo(1.5);
    expect(outF32[15]).toBeCloseTo(-2.25);
  });

  it('wBeginFloat32 caches per slot: zero allocation per call (Law 2)', () => {
    const w = new Weft(64);
    expect(w.wBeginFloat32()).toBe(w.wBeginFloat32());
  });

  it('demo-scale hot loop: 1,000 publish/claim cycles through the public cursor API with stable views', () => {
    const w = new Weft(4096);
    const seenViews = new Set<Uint8Array>();
    for (let seq = 1; seq <= 1000; seq++) {
      const cursor = w.wBegin();
      seenViews.add(cursor);
      cursor[0] = seq & 0xff;
      cursor[1] = (seq >> 8) & 0xff;
      expect(w.publish(seq, 4096)).toBe(PubResult.Ok);
      w.claim();
      const out = w.rReadSlice(16, 2);
      expect(out[0]).toBe(seq & 0xff);
      expect(out[1]).toBe((seq >> 8) & 0xff);
    }
    // Exactly the 3 cached views — no per-frame view allocation across 1,000 frames.
    expect(seenViews.size).toBe(3);
  });
});

// ---------------------------------------------------------------------------
// I6 — writer revocation handshake (02 §6)
// ---------------------------------------------------------------------------

describe('I6 revocation handshake', () => {
  it('publish after revoke returns DROPPED_REVOKED and ACKs via epoch', () => {
    const w = new Weft(64);
    const preEpoch = w.epoch();
    w.revoke();
    expect(w.publish(1, 64)).toBe(PubResult.DroppedRevoked);
    expect(w.epoch()).toBe(preEpoch + 1);
    expect(w.tDrop()).toBe(1n);
    expect(w.tPublish()).toBe(0n);
  });

  it('reclaim observes the ACK after the writer drops a publish', () => {
    const w = new Weft(64);
    const preEpoch = w.epoch();
    w.revoke();
    // Simulate the writer noticing revocation on its next publish:
    w.publish(1, 64);
    // Simulate a concurrent publish arriving late (second ACK):
    w.publish(2, 64);
    expect(w.reclaim(preEpoch, 1000)).toBe(true);
  });

  it('reclaim times out when no writer ACKs', () => {
    const w = new Weft(64);
    const preEpoch = w.epoch();
    w.revoke();
    // No writer exists; nothing ACKs.
    const t0 = Date.now();
    expect(w.reclaim(preEpoch, 50)).toBe(false);
    expect(Date.now() - t0).toBeGreaterThanOrEqual(45); // bounded wait honored
  });
});

// ---------------------------------------------------------------------------
// Debug view — advisory inspection (weft_debug_view parity, AXIOM T)
// ---------------------------------------------------------------------------

describe('debugView', () => {
  it('reports the control-block state and a per-slot owner designation', () => {
    const w = new Weft(64);
    w.fillPayload(9, 64);
    w.publish(9, 64);
    w.claim();
    const v = w.debugView();
    expect(v.revoked).toBe(false);
    expect(v.epoch).toBe(0);
    expect(v.tPublish).toBe(1n);
    expect(v.tClaim).toBe(1n);
    expect(v.bufs).toHaveLength(3);
    // (latest, wWork, rWork) is a permutation of (0,1,2); each slot has one owner.
    const owners = v.bufs.map((b) => b.owner).sort();
    expect(owners).toEqual([1, 2, 3]);
    const idx = [v.latest, v.wWork, v.rWork].sort();
    expect(idx).toEqual([0, 1, 2]);
  });

  it('samples envelope fields: the reader-held slot carries the claimed seq', () => {
    const w = new Weft(64);
    w.fillPayload(41, 64);
    w.publish(41, 64);
    w.claim();
    const v = w.debugView();
    const held = v.bufs.find((b) => b.slotIdx === v.rWork);
    expect(held).toBeDefined();
    expect(held!.seq).toBe(41);
    expect(held!.version).toBe(WEFT_VERSION_1);
    expect(held!.headerSize).toBe(16);
    expect(held!.payloadLen).toBe(64);
    expect(held!.owner).toBe(2);
  });

  it('flags midPublishSample when a header is sampled mid-write (seq != 0, bad magic)', () => {
    const w = new Weft(64);
    w.fillPayload(5, 64);
    w.publish(5, 64);
    // Simulate a header torn mid-publish: nonzero seq + corrupt magic in the
    // latest slot.
    w.dv.setUint32(w.bufOffset(Atomics.load(w.ctrl, Weft.SLOT_LATEST)), 0xdeadbeef, true);
    expect(w.debugView().midPublishSample).toBe(true);
  });

  it('reflects revocation state', () => {
    const w = new Weft(64);
    expect(w.debugView().revoked).toBe(false);
    w.revoke();
    expect(w.debugView().revoked).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// Reader envelope parity
// ---------------------------------------------------------------------------

describe('reader envelope accessors', () => {
  it('rHeaderSize returns 16 for v1 frames (weft_r_header_size parity)', () => {
    const w = new Weft(64);
    w.claim();
    expect(w.rHeaderSize()).toBe(16);
  });

  it('rReadSlice returns a live view bounded by the buffer size', () => {
    const w = new Weft(64);
    const s = w.rReadSlice(16, 1 << 20); // request way too much
    // Bounded by buf_size - offset (buf_size = align64(16+64+8,64) = 128).
    expect(s.length).toBe(w.bufSize - 16);
    const none = w.rReadSlice(1 << 20, 8);
    expect(none.length).toBe(0);
  });
});

// ---------------------------------------------------------------------------
// Telemetry counter regime — dual i32 halves (2026-09-16)
//
// The hot path bumps u64 counters as two Int32 atomics with carry; the cold
// getters compose the exact u64 through the BigInt64 view (same 8 bytes).
// These tests pin the carry arithmetic at the 2^32 boundary and the exact
// u64 observable API across it — the property Option A (single wrap-safe
// u32) gives up and the reason Option B ships instead (bench: see
// demos/web/evidence/feed-gc-bench.log — Mode C's telemetry cost closed).
// ---------------------------------------------------------------------------

describe('telemetry dual-half counters', () => {
  it('counts exactly across the 2^32 carry boundary (t_publish)', () => {
    const w = new Weft(64);
    // White-box, documented layout: put the lo half one below the wrap.
    Atomics.store(w.ctrl, Weft.SLOT_T_PUBLISH_LO, -1); // 0xFFFFFFFF
    Atomics.store(w.ctrl, Weft.SLOT_T_PUBLISH_HI, 0);
    expect(w.tPublish()).toBe(0xFFFFFFFFn);
    w.wBegin()[0] = 1;
    w.publish(1, 64);
    // Carried: hi flipped, lo wrapped to 0 — the exact u64 is 2^32.
    expect(w.tPublish()).toBe(0x1_0000_0000n);
    w.wBegin()[0] = 2;
    w.publish(2, 64);
    expect(w.tPublish()).toBe(0x1_0000_0001n);
  });

  it('counts exactly across the 2^32 carry boundary (t_claim)', () => {
    const w = new Weft(64);
    Atomics.store(w.ctrl, Weft.SLOT_T_CLAIM_LO, -1);
    Atomics.store(w.ctrl, Weft.SLOT_T_CLAIM_HI, 0x1234);
    w.claim();
    // The carry increments the hi half: 0x1234_FFFFFFFF + 1 = 0x1235_0000_0000.
    expect(w.tClaim()).toBe(0x1235_0000_0000n);
  });

  it('t_drop carries through the revoked path', () => {
    const w = new Weft(64);
    Atomics.store(w.ctrl, Weft.SLOT_T_DROP_LO, -1);
    Atomics.store(w.ctrl, Weft.SLOT_T_DROP_HI, 0);
    w.revoke();
    expect(w.publish(1, 64)).toBe(PubResult.DroppedRevoked);
    expect(w.tDrop()).toBe(0x1_0000_0000n);
  });

  it('canary: dual-u32 write keeps the exact BigInt(seq) bit pattern, sign-extended', () => {
    // verifyHeld for the positive domain (pat-filled frame), then the raw
    // canary BYTES for the negative domain — the two's-complement u64 that
    // setBigUint64(BigInt(seq)) produced, now produced by two u32 writes.
    const w = new Weft(64);
    w.fillPayload(7, 64);
    expect(w.publish(7, 64)).toBe(PubResult.Ok);
    w.claim();
    expect(w.verifyHeld(7, 64)).toBe(true);

    const w2 = new Weft(64);
    w2.fillPayload(1, 64);
    w2.publish(-5, 64);
    w2.claim();
    const raw = w2.rReadSlice(w2.bufSize - 8, 8);
    expect(raw.length).toBe(8);
    const dv = new DataView(raw.buffer, raw.byteOffset, 8);
    expect(dv.getBigUint64(0, true)).toBe(0xFFFFFFFFFFFFFFFBn); // BigInt(-5)
  });

  it('t_wsteps/t_rsteps are allocation-free counters (number domain)', () => {
    const w = new Weft(64);
    w.wBegin()[0] = 1;
    w.publish(1, 64);
    w.claim();
    expect(w.t_wsteps).toBe(1);
    expect(w.t_rsteps).toBe(1);
    expect(typeof w.t_wsteps).toBe('number');
  });
});
