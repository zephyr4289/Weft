/**
 * litmus.ts — the WASM port's litmus battery (issue #18-1 acceptance:
 * "WASM build passes all litmus tests (L1–L8)").
 *
 * Node's built-in test runner (node --test, Node >= 22.6 with type
 * stripping; CI uses Node 24). The wasm module IS the frozen C core
 * compiled by Emscripten — these tests prove the semantics survive the
 * boundary (binding + ABI + memory views), which is what "byte-identical
 * behavior with native C" means for a port of the same sources.
 *
 *   WL1-tear          held payload views stay byte-exact while the writer
 *                     double-buffers two more frames past them
 *   WL2-writer-steps  t_wsteps <= 2 * publishes
 *   WL3-reader-steps  t_rsteps <= 2 * claims
 *   WL4-freshness     newest-complete wins; drops telescope exactly
 *   WL5-progress      unpaced bursts all surface; claims progress
 *   WL6-ownership     seeded randomized interleave; seq/payload agreement
 *   WL7-revocation    revoke -> publish drops -> reclaim ACKs (I6)
 *   WL8-envelope      payload bytes/magic discipline + zero-copy views
 *   WF-*              fan-out: roundtrip, drop accounting, ctrl-block
 *                     layout (the cross-port wire contract), 100-frame
 *                     batch, zero-copy ring/frame buffers
 */
import test from "node:test";
import assert from "node:assert/strict";
import { loadWeftWasm } from "../src/weft-wasm.ts";

const WORDS = 64;              // 256-byte payloads
const PAYLOAD = WORDS * 4;

/** The house deterministic payload word (weft_mix32, mirrored). */
function mix32(v: number): number {
  v >>>= 0;
  v = Math.imul(v ^ (v >>> 16), 0x21f0aaad) >>> 0;
  v = Math.imul(v ^ (v >>> 15), 0x735a2d97) >>> 0;
  return (v ^ (v >>> 15)) >>> 0;
}
function tword(seq: number, w: number): number {
  return mix32((Math.imul(seq, 2654435761) + w) >>> 0);
}
function frameU32(seq: number): Uint32Array {
  const a = new Uint32Array(WORDS);
  for (let w = 0; w < WORDS; w++) a[w] = tword(seq, w);
  return a;
}
function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

const W = await loadWeftWasm();

test("setup: module loads (wasm C core + BigInt seqs)", () => {
  assert.ok(W, "module loaded");
});

test("WL1-tear: held payload views stay byte-exact across overwrites", () => {
  const k = W.kernel(PAYLOAD);
  for (let seq = 1; seq <= 300; seq++) {
    assert.equal(k.publish(seq, frameU32(seq)), true, `publish ${seq}`);
    const got = k.claim();
    assert.equal(got, BigInt(seq));
    const held = k.payload();           // zero-copy view of the claimed frame
    // writer double-buffers two more frames while we hold the view
    k.publish(seq + 1, frameU32(seq + 1));
    k.publish(seq + 2, frameU32(seq + 2));
    const expected = new Uint8Array(frameU32(seq).buffer, 0, PAYLOAD);
    assert.ok(bytesEqual(held, expected),
      `held frame ${seq} byte-exact across two overwrites`);
    k.claim(); // catch up
  }
  k.destroy();
});

test("WL2-writer-steps / WL3-reader-steps: step budgets hold", () => {
  const k = W.kernel(PAYLOAD);
  const N = 2000;
  for (let seq = 1; seq <= N; seq++) {
    k.publish(seq, frameU32(seq));
    k.claim();
  }
  const t = k.telemetry();
  assert.ok(t.wsteps <= 2n * BigInt(N), `wsteps ${t.wsteps} <= ${2 * N}`);
  assert.ok(t.rsteps <= 2n * BigInt(N), `rsteps ${t.rsteps} <= ${2 * N}`);
  k.destroy();
});

test("WL4-freshness: newest complete wins, drops telescope", () => {
  const k = W.kernel(PAYLOAD);
  k.publish(1, frameU32(1));
  k.publish(2, frameU32(2));
  k.publish(3, frameU32(3));
  assert.equal(k.claim(), 3n, "newest complete frame wins");
  assert.equal(k.telemetry().publish, 3n);
  // t_drop counts REVOKED publishes (the kernel's Law: writer-side drops),
  // zero here — reader-missed-frames accounting is the fan-out's dropped,
  // proven in WF-roundtrip. Asserted separately to pin the distinction.
  assert.equal(k.telemetry().drop, 0n, "no revoke-drops");
  k.destroy();
});

test("WL5-progress: unpaced bursts all surface", () => {
  const k = W.kernel(PAYLOAD);
  let last = 0n;
  for (let round = 0; round < 50; round++) {
    for (let i = 0; i < 20; i++) {
      const seq = round * 20 + i + 1;
      k.publish(seq, frameU32(seq));
    }
    const got = k.claim();
    assert.ok(got > last, "claims strictly progress");
    last = got;
  }
  assert.equal(last, 1000n);
  k.destroy();
});

test("WL6-ownership: seeded randomized interleave agrees", () => {
  const k = W.kernel(PAYLOAD);
  let state = 0x1234567;
  const rand = () => {
    state ^= state << 13; state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5; state >>>= 0;
    return state;
  };
  let seq = 0;
  for (let trial = 0; trial < 2000; trial++) {
    if (rand() & 1) {
      seq++;
      k.publish(seq, frameU32(seq));
    } else if (seq > 0) {
      const got = k.claim();
      // Claiming without an intervening publish legally yields the Triad's
      // third (empty) buffer: seq 0 = "nothing new" — the kernel's design.
      assert.ok(got >= 0n && got <= BigInt(seq), "claim within published range");
      if (got > 0n) {
        const pv = k.payload();  // view INTO wasm memory (byteOffset != 0)
        const p = new Uint32Array(pv.buffer, pv.byteOffset, WORDS);
        assert.equal(p[0], tword(Number(got), 0), "claimed payload matches its seq");
        assert.equal(p[WORDS - 1], tword(Number(got), WORDS - 1));
      }
    }
  }
  k.destroy();
});

test("WL7-revocation: revoke drops, reclaim ACKs (I6)", () => {
  const k = W.kernel(PAYLOAD);
  k.publish(1, frameU32(1));
  k.claim();
  const pre = k.revoke();
  assert.equal(k.publish(2, frameU32(2)), false, "publish after revoke drops");
  assert.equal(k.reclaim(pre, 100), true, "reclaim ACKs");
  k.destroy();
});

test("WL8-envelope: payload bytes exact + zero-copy identity", () => {
  const k = W.kernel(PAYLOAD);
  k.publish(42, frameU32(42));
  k.claim();
  const p1 = k.payload();
  const p2 = k.payload();
  // zero-copy: both views alias the SAME wasm memory (same buffer, offset)
  assert.equal(p1.buffer, p2.buffer);
  assert.equal(p1.byteOffset, p2.byteOffset);
  const expected = new Uint8Array(frameU32(42).buffer, 0, PAYLOAD);
  assert.ok(bytesEqual(p1, expected), "payload bytes exact");
  k.destroy();
});

// ---------------------------------------------------------------------------
// Fan-out (WF-series)
// ---------------------------------------------------------------------------

test("WF-roundtrip: fresh claims carry exact bytes; drops telescope", () => {
  const f = W.fanout(PAYLOAD, 4);
  const r = f.reader();
  let totalDrops = 0n;
  let freshCount = 0n;
  let last = 0n;
  for (let seq = 1; seq <= 2000; seq++) {
    const got = f.publish(frameU32(seq));
    assert.equal(got, BigInt(seq));
    const c = r.claim();
    if (c.fresh) {
      totalDrops += c.dropped;
      freshCount++;
      last = c.seq;
      const v = r.view();
      assert.equal(v[0], tword(seq, 0));
      assert.equal(v[WORDS - 1], tword(seq, WORDS - 1));
    }
  }
  // telescoping identity: sum(dropped) + fresh == lastSeq
  assert.equal(totalDrops + freshCount, last);
  f.destroy();
});

test("WF-layout: ring bytes follow the cross-port wire contract", () => {
  const f = W.fanout(PAYLOAD, 4);
  const ctrl = f.ctrlView();
  // fresh ring: latestSeq=0, publishes=0, slotSeq[k]=0
  assert.equal(ctrl[0], 0n);
  assert.equal(ctrl[1], 0n);
  f.publish(frameU32(1));
  const ctrl2 = f.ctrlView();
  assert.equal(ctrl2[0], 1n, "latestSeq @ byte 0");
  assert.equal(ctrl2[1], 1n, "publishes @ byte 8");
  // slot of frame 1 = (1-1) % 4 = 0 -> slotSeq[0] = 1 @ byte 16 + 8*0
  assert.equal(ctrl2[2], 1n, "slotSeq[0] @ byte 16");
  const ring = f.ringBytesView();
  assert.equal(ring.length, 16 + 8 * 4 + 4 * PAYLOAD, "ring size = 16+8M+M*pb");
  f.destroy();
});

test("WF-batch: 100-frame single-flip batch (issue #17-3)", () => {
  const f = W.fanout(PAYLOAD, 4);
  const r = f.reader();
  const N = 100;
  const frames: Uint32Array[] = [];
  for (let s = 1; s <= N; s++) frames.push(frameU32(s));
  const last = f.publishBatch(frames);
  assert.equal(last, 100n);
  const c = r.claim();
  assert.equal(c.fresh, true);
  assert.equal(c.seq, 100n);
  assert.equal(c.dropped, 99n);
  const v = r.view();
  assert.equal(v[0], tword(100, 0), "tail frame bytes exact");
  f.destroy();
});

test("WF-zero-copy: frameBuffer lives in wasm memory (no boundary copy)", () => {
  const f = W.fanout(PAYLOAD, 4);
  const r = f.reader();
  const buf = W.frameBuffer(PAYLOAD);
  const words = new Uint32Array(buf.buffer, buf.byteOffset, WORDS);
  for (let s = 1; s <= 100; s++) {
    for (let w = 0; w < WORDS; w++) words[w] = tword(s, w);
    const seq = f.publish(buf);       // zero-copy: buffer already in wasm
    assert.equal(seq, BigInt(s));
    const c = r.claim();
    if (c.fresh) {
      const v = r.view();
      assert.equal(v[WORDS - 1], tword(s, WORDS - 1));
    }
  }
  W.freeBuffer(buf);
  f.destroy();
});
