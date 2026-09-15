// fanout.test.ts — @weft/core RFC-0004 fan-out driver-layer conformance suite
//
// WHY EXISTS: RFC 0004 (Multi-Consumer Fan-Out Heddles) was accepted as a
// driver-layer pattern (round-6-adjudication §4) on the strength of a
// simulation-only spike (evidence/D-17/fanout_prototype.log). This suite is
// the package-level mechanical acceptance battery for the production
// implementation (src/fanout.ts): ring geometry, the stamp-then-fill writer
// protocol, per-reader fresh/drop accounting, the graceful-skip tear
// discipline, zero steady-state allocation (Law 2), and a cross-thread
// protocol litmus where an INDEPENDENT writer implementation (a plain JS
// worker coding the RFC text, not importing the class) hammers the ring
// while real readers validate every claimed byte. It is the F-series
// counterpart of the kernel's L-series; the kernel litmus remains the
// canonical gate for kernel semantics — this battery covers the userland
// driver layer only.
//
// Environment tag for any timing-sensitive numbers: node-vitest (sandbox).

import { describe, it, expect } from 'vitest';
import { Worker } from 'node:worker_threads';
import {
  WeftFanoutBroadcaster,
  WeftFanoutReader,
} from '../src/index';

// ---------------------------------------------------------------------------
// Shared deterministic payload fixture (the fanout analog of 04-LITMUS §0.1
// pat()): integer-valued, hence EXACT in float32 (integers < 2^24) — the
// ring payload is Float32, so the fixture must round-trip bit-exactly.
// ---------------------------------------------------------------------------

function expectedF(seq: number, i: number): number {
  return (seq * 131 + i * 37) % 9973;
}

function fillFrame(view: Float32Array, seq: number): void {
  for (let i = 0; i < view.length; i++) view[i] = expectedF(seq, i);
}

function validateFrame(view: Float32Array, seq: number): boolean {
  for (let i = 0; i < view.length; i++) {
    if (view[i] !== expectedF(seq, i)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Ring geometry and construction
// ---------------------------------------------------------------------------

describe('fanout construction and geometry', () => {
  it('defaults to a 4-slot ring (RFC 0004 §Reference)', () => {
    const b = new WeftFanoutBroadcaster(16);
    expect(b.slotCount).toBe(4);
    expect(b.payloadFloats).toBe(16);
  });

  it('SAB is sized exactly: 16B control + 8B/slot stamps + payload', () => {
    // M=4, P=16: 16 + 4*8 + 4*16*4 = 304 bytes.
    const b = new WeftFanoutBroadcaster(16, 4);
    expect(b.sab.byteLength).toBe(16 + 4 * 8 + 4 * 16 * 4);
    const b8 = new WeftFanoutBroadcaster(640, 8);
    expect(b8.sab.byteLength).toBe(16 + 8 * 8 + 8 * 640 * 4);
  });

  it('rejects non-integer or non-positive geometry', () => {
    expect(() => new WeftFanoutBroadcaster(0)).toThrow();
    expect(() => new WeftFanoutBroadcaster(-4)).toThrow();
    expect(() => new WeftFanoutBroadcaster(1.5)).toThrow();
    expect(() => new WeftFanoutBroadcaster(16, 1)).toThrow(); // ring needs >= 2
    expect(() => new WeftFanoutBroadcaster(16, 2.5)).toThrow();
  });

  it('initial state: no frame, nothing resident, zero telemetry', () => {
    const b = new WeftFanoutBroadcaster(8);
    const s = b.debugStats();
    expect(s.latestSeq).toBe(0n);
    expect(s.publishes).toBe(0n);
    expect(s.slotStamps).toEqual([0n, 0n, 0n, 0n]);
  });
});

// ---------------------------------------------------------------------------
// Writer protocol — stamp-then-fill bracket
// ---------------------------------------------------------------------------

describe('fanout writer protocol', () => {
  it('publish() before any begin() is a detectable no-op', () => {
    const b = new WeftFanoutBroadcaster(8);
    expect(b.publish()).toBe(0);
    expect(b.debugStats().latestSeq).toBe(0n);
    expect(b.debugStats().publishes).toBe(0n);
  });

  it('begin() invalidates the target slot BEFORE the fill (tear bracket)', () => {
    const b = new WeftFanoutBroadcaster(8, 4);
    const v = b.begin(); // wSeq=1 -> slot 0
    expect(b.debugStats().slotStamps[0]).toBe(0n); // invalidated on begin
    fillFrame(v, 1);
    expect(b.publish()).toBe(1);
    expect(b.debugStats().slotStamps[0]).toBe(1n); // re-stamped on publish
    expect(b.debugStats().latestSeq).toBe(1n);
    // Next begin touches slot 1 only; slot 0's stamp survives untouched.
    b.begin();
    const st = b.debugStats().slotStamps;
    expect(st[0]).toBe(1n);
    expect(st[1]).toBe(0n);
  });

  it('begin() returns cached per-slot views — zero allocation per call (Law 2)', () => {
    const b = new WeftFanoutBroadcaster(8, 4);
    const seen = new Set<Float32Array>();
    for (let f = 0; f < 10; f++) seen.add(b.begin());
    expect(seen.size).toBe(4); // exactly the M cached views across 10 begins
  });

  it('publish() returns the monotonically increasing frame seq', () => {
    const b = new WeftFanoutBroadcaster(8, 4);
    for (let f = 1; f <= 12; f++) {
      fillFrame(b.begin(), f);
      expect(b.publish()).toBe(f);
    }
    expect(b.debugStats().publishes).toBe(12n);
    expect(b.debugStats().latestSeq).toBe(12n);
  });

  it('an abandoned begin() is observable: its slot stays invalidated', () => {
    const b = new WeftFanoutBroadcaster(8, 4);
    b.begin(); // wSeq=1, slot 0 invalidated, never published
    const s = b.debugStats();
    expect(s.slotStamps[0]).toBe(0n);
    expect(s.latestSeq).toBe(0n); // nothing completed
  });
});

// ---------------------------------------------------------------------------
// Reader semantics — single-threaded deterministic schedules
// ---------------------------------------------------------------------------

describe('fanout reader semantics', () => {
  it('claim() before any publish: not fresh, seq 0', () => {
    const b = new WeftFanoutBroadcaster(8);
    const r = b.createReader();
    const c = r.claim();
    expect(c.fresh).toBe(false);
    expect(c.seq).toBe(0);
    expect(c.dropped).toBe(0);
  });

  it('one publish -> fresh claim with seq 1, dropped 0', () => {
    const b = new WeftFanoutBroadcaster(16);
    fillFrame(b.begin(), 1);
    b.publish();
    const r = b.createReader();
    const c = r.claim();
    expect(c.fresh).toBe(true);
    expect(c.seq).toBe(1);
    expect(c.dropped).toBe(0);
    expect(validateFrame(r.view(), 1)).toBe(true);
  });

  it('claim() with no newer frame: not fresh, seq retained', () => {
    const b = new WeftFanoutBroadcaster(16);
    fillFrame(b.begin(), 1);
    b.publish();
    const r = b.createReader();
    r.claim();
    const c2 = r.claim();
    expect(c2.fresh).toBe(false);
    expect(c2.seq).toBe(1);
  });

  it('dropped counts frames completed without this reader observing them', () => {
    const b = new WeftFanoutBroadcaster(16);
    fillFrame(b.begin(), 1);
    b.publish();
    const r = b.createReader();
    r.claim(); // lastSeq = 1
    for (let f = 2; f <= 6; f++) {
      fillFrame(b.begin(), f);
      b.publish();
    }
    const c = r.claim(); // jumps to frame 6; frames 2..5 dropped
    expect(c.fresh).toBe(true);
    expect(c.seq).toBe(6);
    expect(c.dropped).toBe(4);
    expect(validateFrame(r.view(), 6)).toBe(true);
  });

  it('ring overwrite: after M+3 publishes the claim yields the LATEST frame', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    for (let f = 1; f <= 7; f++) {
      fillFrame(b.begin(), f);
      b.publish();
    }
    const r = b.createReader();
    const c = r.claim();
    expect(c.seq).toBe(7); // never a stale frame older than latest
    expect(c.dropped).toBe(6); // first claim over 7 published frames
    expect(validateFrame(r.view(), 7)).toBe(true);
  });

  it('N readers are independent: different rates, separate accounting', () => {
    const b = new WeftFanoutBroadcaster(32, 4);
    const every = b.createReader();
    const third = b.createReader();
    for (let f = 1; f <= 30; f++) {
      fillFrame(b.begin(), f);
      b.publish();
      every.claim();
      if (f % 3 === 0) third.claim();
    }
    const se = every.stats();
    const st = third.stats();
    expect(se.fresh).toBe(30);
    expect(se.drops).toBe(0);
    expect(st.fresh).toBe(10);
    expect(st.drops).toBe(20);
    // Both end on the same newest frame.
    expect(every.claim().seq).toBe(30);
    expect(third.claim().seq).toBe(30);
  });

  it('reader buffers and claim records are distinct per reader', () => {
    const b = new WeftFanoutBroadcaster(8);
    const r1 = b.createReader();
    const r2 = b.createReader();
    expect(r1.view()).not.toBe(r2.view());
    expect(r1.claim()).not.toBe(r2.claim());
  });

  it('claimed seq is monotonic non-decreasing across a run', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    const r = b.createReader();
    let prev = 0;
    for (let f = 1; f <= 200; f++) {
      fillFrame(b.begin(), f);
      b.publish();
      if (f % 7 === 3) continue; // arbitrary non-claiming ticks (gaps jump)
      const c = r.claim();
      expect(c.seq).toBeGreaterThanOrEqual(prev);
      prev = c.seq;
    }
    expect(prev).toBe(200);
  });

  it('attach parity: a reader built from the raw SAB equals createReader()', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    for (let f = 1; f <= 5; f++) {
      fillFrame(b.begin(), f);
      b.publish();
    }
    const attached = new WeftFanoutReader(b.sab, 16, 4);
    const c = attached.claim();
    expect(c.fresh).toBe(true);
    expect(c.seq).toBe(5);
    expect(validateFrame(attached.view(), 5)).toBe(true);
  });

  it('attach rejects a geometry mismatch instead of tearing', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    expect(() => new WeftFanoutReader(b.sab, 32, 4)).toThrow(/geometry/);
    expect(() => new WeftFanoutReader(b.sab, 16, 8)).toThrow(/geometry/);
  });
});

// ---------------------------------------------------------------------------
// Zero steady-state allocation (Law 2) — identity-stable hot-path objects
// ---------------------------------------------------------------------------

describe('fanout zero-allocation contract (Law 2)', () => {
  it('claim() mutates one preallocated record: identity-stable across 1,000 claims', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    const r = b.createReader();
    const first = r.claim();
    for (let f = 1; f <= 1000; f++) {
      fillFrame(b.begin(), f);
      b.publish();
      const c = r.claim();
      expect(c).toBe(first); // same record object every time
    }
  });

  it('view() is one preallocated buffer: identity-stable across the run', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    const r = b.createReader();
    const v0 = r.view();
    for (let f = 1; f <= 500; f++) {
      fillFrame(b.begin(), f);
      b.publish();
      r.claim();
      expect(r.view()).toBe(v0);
    }
  });

  it('broadcaster begin() hands out only the M cached slot views', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    const seen = new Set<Float32Array>();
    for (let f = 0; f < 100; f++) seen.add(b.begin());
    expect(seen.size).toBe(4);
  });
});

// ---------------------------------------------------------------------------
// Graceful-skip tear discipline — deterministic protocol exercises
// ---------------------------------------------------------------------------

describe('fanout tear discipline (graceful skip)', () => {
  it('a reader skips the tick when its target slot is mid-overwrite', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    fillFrame(b.begin(), 1);
    b.publish();
    const r = b.createReader();
    expect(r.claim().seq).toBe(1); // lastSeq = 1
    for (let f = 2; f <= 4; f++) {
      fillFrame(b.begin(), f);
      b.publish();
    }
    // latest = 4 lives in slot 3. M begins advance wSeq to 8; the 4th begin
    // invalidates slot 3 — the very slot latest points at (frame 8 would
    // reuse frame 4's slot).
    for (let i = 0; i < 4; i++) b.begin();
    const c = r.claim();
    expect(c.fresh).toBe(false); // graceful skip, not a torn frame
    expect(c.seq).toBe(1); // keeps the last consistent frame
    expect(r.stats().skippedMidOverwrite).toBe(1);
    // The in-flight frame completes -> the next claim resumes cleanly.
    fillFrame(b.begin(), 9);
    b.publish();
    const c2 = r.claim();
    expect(c2.fresh).toBe(true);
    expect(c2.seq).toBe(9);
    expect(c2.dropped).toBe(7); // frames 2..8 gap over lastSeq=1
    expect(validateFrame(r.view(), 9)).toBe(true);
  });

  it('abandoned begins make the latest frame skip-observable, not silent', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    for (let f = 1; f <= 4; f++) {
      fillFrame(b.begin(), f);
      b.publish();
    }
    // M begins, none ever published (writer abandoned mid-sequence): the
    // last one invalidates slot 3, where latest = 4 lives.
    for (let i = 0; i < 4; i++) b.begin();
    const r = b.createReader();
    const c = r.claim();
    expect(c.fresh).toBe(false); // skip: slot mid-overwrite, no newer frame
    expect(c.seq).toBe(0);
    expect(r.stats().skippedMidOverwrite).toBe(1);
    // The writer resumes with the next frame; the claim lands cleanly.
    fillFrame(b.begin(), 9);
    b.publish();
    const c2 = r.claim();
    expect(c2.fresh).toBe(true);
    expect(c2.seq).toBe(9);
    // Seq-gap accounting: seqs 5..8 never completed, yet read as 8 drops —
    // abandoned seqs are indistinguishable from missed publishes (declared
    // boundary of the seq-based accounting, same as the test below).
    expect(c2.dropped).toBe(8);
  });

  it('seq-gap accounting counts an abandoned frame as a drop (declared boundary)', () => {
    const b = new WeftFanoutBroadcaster(16, 4);
    b.begin(); // wSeq=1 abandoned
    fillFrame(b.begin(), 2); // wSeq=2
    b.publish();
    const r = b.createReader();
    const c = r.claim();
    // Frame 1 never completed; the seq gap still reads as one drop. The
    // accounting is seq-based, and abandoned seqs are indistinguishable from
    // missed publishes — documented in the RFC refinement.
    expect(c.seq).toBe(2);
    expect(c.dropped).toBe(1);
  });
});

// ---------------------------------------------------------------------------
// Canonical RFC scenario — four consumers, deterministic closed form
// (mirrors the D-17 spike scenario and its evidence log shape)
// ---------------------------------------------------------------------------

describe('fanout canonical four-consumer scenario', () => {
  it('exact per-reader accounting over 10,000 frames at 120/60/30/15 Hz', () => {
    const N = 10000;
    const P = 256;
    const b = new WeftFanoutBroadcaster(P, 4);
    const consumers = [
      { name: 'flight-recorder', divisor: 1, reader: b.createReader() },
      { name: 'primary-canvas', divisor: 2, reader: b.createReader() },
      { name: 'minimap', divisor: 4, reader: b.createReader() },
      { name: 'network-viz', divisor: 8, reader: b.createReader() },
    ];
    let integrityFailures = 0;
    for (let t = 1; t <= N; t++) {
      fillFrame(b.begin(), t);
      b.publish();
      for (const c of consumers) {
        if (t % c.divisor === 0) {
          const claim = c.reader.claim();
          if (claim.fresh) {
            // Per-claim invariants: exact drop stride and byte integrity.
            if (claim.dropped !== c.divisor - 1) integrityFailures++;
            if (!validateFrame(c.reader.view(), claim.seq)) integrityFailures++;
          } else {
            integrityFailures++; // cannot happen in this schedule
          }
        }
      }
    }
    expect(integrityFailures).toBe(0);
    // Closed forms: reads = fresh = N/d; drops = N - N/d (telescoping sum).
    for (const c of consumers) {
      const st = c.reader.stats();
      const expectedFresh = N / c.divisor;
      expect(st.reads).toBe(expectedFresh);
      expect(st.fresh).toBe(expectedFresh);
      expect(st.drops).toBe(N - expectedFresh);
      expect(st.skippedMidOverwrite).toBe(0); // single-threaded schedule
      expect(st.tornExhausted).toBe(0);
      expect(c.reader.claim().seq).toBe(N);
    }
  });
});

// ---------------------------------------------------------------------------
// Cross-thread protocol litmus — an independent writer implementation in a
// worker (coding the RFC ring protocol from spec text, NOT importing the
// class) against real WeftFanoutReader instances on this thread.
// The F-series analog of the kernel's cross-language parity harnesses.
// ---------------------------------------------------------------------------

/**
 * Independent publisher: the RFC-0004 ring protocol in plain JS (eval'd in a
 * worker thread). Deliberately does not import WeftFanoutBroadcaster — the
 * litmus validates the WIRE PROTOCOL, not class self-consistency. Layout
 * constants are restated from the RFC reference spec: ctrl[0]=latestSeq,
 * ctrl[1]=publishes, ctrl[2+k]=slotSeq[k], payload at 16+8M.
 */
const INDEPENDENT_PUBLISHER_SRC = `
const { parentPort } = require('node:worker_threads');
parentPort.on('message', (m) => {
  if (m.kind !== 'start') return;
  const sab = m.sab, M = m.slotCount, P = m.payloadFloats, frames = m.frames;
  const ctrl = new BigInt64Array(sab, 0, 2 + M);
  const base = 16 + 8 * M;
  const views = [];
  for (let k = 0; k < M; k++) {
    views.push(new Float32Array(sab, base + k * P * 4, P));
  }
  function expectedF(seq, i) {
    return (seq * 131 + i * 37) % 9973;
  }
  for (let f = 1; f <= frames; f++) {
    const k = (f - 1) % M;
    Atomics.store(ctrl, 2 + k, 0n);          // invalidate before fill
    const v = views[k];
    for (let i = 0; i < P; i++) v[i] = expectedF(f, i);
    Atomics.store(ctrl, 2 + k, BigInt(f));   // re-stamp
    Atomics.store(ctrl, 0, BigInt(f));       // flip latest
    Atomics.add(ctrl, 1, 1n);                // telemetry
  }
  parentPort.postMessage({ kind: 'done', published: frames });
});
`;

describe('fanout cross-thread protocol litmus', () => {
  it('independent worker writer + 3 main-thread readers: zero torn claims (M=4, P=64, 100k frames)', { timeout: 60000 }, async () => {
    const M = 4, P = 64, FRAMES = 100000;
    const b = new WeftFanoutBroadcaster(P, M);
    const readers = [b.createReader(), b.createReader(), b.createReader()];
    const paces = [0, 300, 3000]; // burn work between claims: distinct rates

    const w = new Worker(INDEPENDENT_PUBLISHER_SRC, { eval: true });
    let finished = false;
    const done = new Promise<{ published: number }>((resolve) => {
      w.on('message', (m: { kind: string; published: number }) => {
        if (m.kind === 'done') { finished = true; resolve(m); }
      });
    });
    w.postMessage({ kind: 'start', sab: b.sab, slotCount: M, payloadFloats: P, frames: FRAMES });

    const burn = (n: number) => { let s = 0; for (let i = 0; i < n; i++) s += Math.sqrt(i); return s; };
    let torn = 0;
    let seqRegression = 0;
    const prevSeq = [0, 0, 0];
    while (!finished) {
      for (let ri = 0; ri < readers.length; ri++) {
        const c = readers[ri].claim();
        if (c.fresh) {
          if (!validateFrame(readers[ri].view(), c.seq)) torn++;
          if (c.seq < prevSeq[ri]) seqRegression++;
          prevSeq[ri] = c.seq;
        }
        burn(paces[ri]);
      }
      // Yield so the worker's completion message can land.
      await new Promise<void>((r) => setImmediate(r));
    }
    const msg = await done;
    // Drain: final claim per reader.
    for (const r of readers) {
      const c = r.claim();
      if (c.fresh && !validateFrame(r.view(), c.seq)) torn++;
    }

    // THE headline assertion: zero torn or invalid payloads across the run.
    expect(torn).toBe(0);
    expect(seqRegression).toBe(0);
    // Every reader made progress and stays behind the writer.
    for (let ri = 0; ri < readers.length; ri++) {
      const st = readers[ri].stats();
      expect(st.fresh).toBeGreaterThan(0);
      expect(prevSeq[ri]).toBeLessThanOrEqual(msg.published);
      // Telescoping identity: sum(dropped) == lastSeq - freshClaims.
      expect(st.drops).toBe(prevSeq[ri] - st.fresh);
      expect(st.reads).toBeGreaterThanOrEqual(st.fresh);
    }
    // Cross-implementation telemetry agreement: the independent publisher
    // incremented the same counter the broadcaster reads.
    expect(b.debugStats().publishes).toBe(BigInt(FRAMES));
    expect(b.debugStats().latestSeq).toBe(BigInt(FRAMES));
    await w.terminate();
  });

  it('geometry variant (M=8, P=640, 50k frames): zero torn claims', { timeout: 60000 }, async () => {
    const M = 8, P = 640, FRAMES = 50000;
    const b = new WeftFanoutBroadcaster(P, M);
    const readers = [b.createReader(), b.createReader()];
    const w = new Worker(INDEPENDENT_PUBLISHER_SRC, { eval: true });
    let finished = false;
    const done = new Promise<{ published: number }>((resolve) => {
      w.on('message', (m: { kind: string; published: number }) => {
        if (m.kind === 'done') { finished = true; resolve(m); }
      });
    });
    w.postMessage({ kind: 'start', sab: b.sab, slotCount: M, payloadFloats: P, frames: FRAMES });

    let torn = 0;
    const prevSeq = [0, 0];
    while (!finished) {
      for (let ri = 0; ri < readers.length; ri++) {
        const c = readers[ri].claim();
        if (c.fresh) {
          if (!validateFrame(readers[ri].view(), c.seq)) torn++;
          prevSeq[ri] = c.seq;
        }
        // Heavier burn: wide payload + slow readers stresses the ring.
        let s = 0; for (let i = 0; i < 5000; i++) s += Math.sqrt(i);
        void s;
      }
      await new Promise<void>((r) => setImmediate(r));
    }
    await done;
    for (const r of readers) {
      const c = r.claim();
      if (c.fresh && !validateFrame(r.view(), c.seq)) torn++;
    }
    expect(torn).toBe(0);
    for (let ri = 0; ri < readers.length; ri++) {
      const st = readers[ri].stats();
      expect(st.fresh).toBeGreaterThan(0);
      expect(prevSeq[ri]).toBeLessThanOrEqual(FRAMES);
      expect(st.drops).toBe(prevSeq[ri] - st.fresh);
    }
    expect(b.debugStats().publishes).toBe(BigInt(FRAMES));
    await w.terminate();
  });
});
