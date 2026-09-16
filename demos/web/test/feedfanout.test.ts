// feedfanout.test.ts — W6 fan-out feed litmus: the RFC-0004 driver layer
// under feed pressure.
//
// WHY EXISTS: W6 turns the demo matrix's feed workload into a real
// ingestion regime (50 delta messages folded per display frame). This
// suite proves the fan-out driver layer carries that regime correctly:
//
//   1. Multi-cadence consumers (main thread): one real feed/engine
//      publisher, three readers claiming at 60/30/10 Hz divisors — exact
//      per-reader drop strides, frame-seq identity (payload[0] ===
//      claim.seq), and full-payload checksum validation on EVERY fresh
//      claim.
//
//   2. Cross-thread protocol litmus: an INDEPENDENT worker-side writer
//      (plain JS in an eval'd worker, coding the ring protocol AND the W6
//      frame contract from their spec text — importing nothing) hammers
//      the ring with folded feed frames as fast as it can; real
//      WeftFanoutReader instances on this thread validate every claimed
//      byte. The F-series cross-thread tradition, applied to the feed
//      workload: the WIRE is what gets validated, not class
//      self-consistency.
//
// Environment tag for any timing-sensitive numbers: node-vitest (sandbox).

import { describe, it, expect } from 'vitest';
import { Worker } from 'node:worker_threads';
import { WeftFanoutBroadcaster, WeftFanoutReader } from '@weft/core';
import {
  SyntheticL2Feed,
  L2BookEngine,
  w6Checksum,
  W6_FLOAT_COUNT,
  W6_FEED_TO_DISPLAY,
  W6_MSG_FIELDS,
  W6_HEADER_FLOATS,
  W6_LEVELS,
  W6_LADDER_FIELDS,
} from '../src/workloads/l2feed';

/// Structural validation of a claimed W6 frame (cheap subset — the
/// checksum already covers every byte).
function validateFeedFrame(f: Float32Array, seq: number): boolean {
  if (f[0] !== seq) return false; // frame-seq identity
  if (w6Checksum(f) !== f[7]) return false; // full-payload integrity
  const lad = W6_HEADER_FLOATS;
  if (f[lad] >= f[lad + 3]) return false; // uncrossed top of book
  for (let i = 0; i < 4; i++) {
    const o = lad + i * W6_LADDER_FIELDS;
    if (f[o + 1] < 0 || f[o + 4] < 0) return false; // sizes non-negative
  }
  return true;
}

// ---------------------------------------------------------------------------
// Multi-cadence consumers — exact accounting + per-claim integrity.
// ---------------------------------------------------------------------------

describe('fanout feed multi-cadence consumers', () => {
  it('60/30/10 Hz readers: exact drop strides, frame identity, checksum on every claim', () => {
    const N = 5000;
    const b = new WeftFanoutBroadcaster(W6_FLOAT_COUNT, 4);
    const feed = new SyntheticL2Feed();
    const engine = new L2BookEngine();
    const batch = new Float64Array(W6_FEED_TO_DISPLAY * W6_MSG_FIELDS);
    const consumers = [
      { name: 'ladder', divisor: 1, reader: b.createReader() },
      { name: 'tape', divisor: 2, reader: b.createReader() },
      { name: 'stats-hud', divisor: 6, reader: b.createReader() },
    ];
    let integrityFailures = 0;

    for (let t = 1; t <= N; t++) {
      feed.nextTick(batch);
      engine.setGrid(feed.midC, feed.bidOff, feed.askOff);
      engine.beginTick();
      engine.applyBatch(batch, W6_FEED_TO_DISPLAY);
      engine.exportFrame(b.begin(), t);
      b.publish();
      for (const c of consumers) {
        if (t % c.divisor === 0) {
          const claim = c.reader.claim();
          if (claim.fresh) {
            if (claim.dropped !== c.divisor - 1) integrityFailures++;
            if (!validateFeedFrame(c.reader.view(), claim.seq)) integrityFailures++;
          } else {
            integrityFailures++; // cannot happen in this schedule
          }
        }
      }
    }
    expect(integrityFailures).toBe(0);
    // Closed forms (telescoping): reads = fresh = floor(N/d) claims at
    // t = d, 2d, ...; drops = lastClaimedSeq - fresh (exact telescoping).
    for (const c of consumers) {
      const st = c.reader.stats();
      const expectedFresh = Math.floor(N / c.divisor);
      expect(st.reads).toBe(expectedFresh);
      expect(st.fresh).toBe(expectedFresh);
      expect(st.drops).toBe(expectedFresh * c.divisor - expectedFresh);
      expect(st.skippedMidOverwrite).toBe(0); // single-threaded schedule
      expect(st.tornExhausted).toBe(0);
    }
    // All three consumers ended on the same final frame.
    for (const c of consumers) {
      expect(c.reader.claim().seq).toBe(N);
    }
  });
});

// ---------------------------------------------------------------------------
// Cross-thread protocol litmus — independent worker-side feed writer.
//
// The worker restates the ring protocol (ctrl[0]=latestSeq, ctrl[1]=
// publishes, ctrl[2+k]=slotSeq[k], payload at 16+8M; invalidate-before-fill
// bracket) AND the W6 frame contract (header 8 floats, 64-level ladder,
// 64-trade tape, integer-cents model, w6Checksum over everything except
// [7]) from spec text. It folds K=50 synthetic feed messages per frame and
// publishes as fast as it can.
// ---------------------------------------------------------------------------

const INDEPENDENT_FEED_WRITER_SRC = `
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
  // W6 contract restated from l2feed.ts (independent implementation):
  // L=64 levels, T=64 tape, K=50 msgs/tick, integer cents, mod 2^23-1.
  const L = 64, T = 64, K = 50, MOD = 8388607;
  const bidSz = new Int32Array(L), bidN = new Int32Array(L);
  const askSz = new Int32Array(L), askN = new Int32Array(L);
  for (let i = 0; i < L; i++) {
    bidSz[i] = 20 + ((i * 7) % 61); askSz[i] = 20 + ((i * 11) % 61);
    bidN[i] = 1 + ((i * 3) % 7);   askN[i] = 1 + ((i * 5) % 7);
  }
  const tpx = new Int32Array(T), tsz = new Int32Array(T), tsd = new Int32Array(T);
  let tc = 0, midC = 10000, bo = 2, ao = 2, lastPx = 0;
  let s = (0x00c0ffee ^ 0x00000606) >>> 0;
  const rng = () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; };
  const checksum = (p) => {
    let h = 7;
    h = (h * 131 + Math.round(p[0])) % MOD;
    h = (h * 131 + Math.round(p[1])) % MOD;
    h = (h * 131 + Math.round(p[2] * 100)) % MOD;
    h = (h * 131 + Math.round(p[3] * 100)) % MOD;
    h = (h * 131 + Math.round(p[4])) % MOD;
    h = (h * 131 + Math.round(p[5])) % MOD;
    h = (h * 131 + Math.round(p[6] * 100)) % MOD;
    for (let i = 0; i < L; i++) {
      const o = 8 + i * 6;
      h = (h * 131 + Math.round(p[o] * 100)) % MOD;
      h = (h * 131 + Math.round(p[o + 1])) % MOD;
      h = (h * 131 + Math.round(p[o + 2])) % MOD;
      h = (h * 131 + Math.round(p[o + 3] * 100)) % MOD;
      h = (h * 131 + Math.round(p[o + 4])) % MOD;
      h = (h * 131 + Math.round(p[o + 5])) % MOD;
    }
    for (let j = 0; j < T; j++) {
      const o = 8 + L * 6 + j * 3;
      h = (h * 131 + Math.round(p[o] * 100)) % MOD;
      h = (h * 131 + Math.round(p[o + 1])) % MOD;
      h = (h * 131 + Math.round(p[o + 2]) + 1) % MOD;
    }
    return h;
  };
  for (let f = 1; f <= frames; f++) {
    bo = 1 + Math.floor(rng() * 4);
    ao = 1 + Math.floor(rng() * 4);
    let folded = 0;
    for (let mm = 0; mm < K; mm++) {
      if (rng() < 0.25) {
        midC += rng() < 0.5 ? -1 : 1;
        if (midC < 9500) midC = 9500;
        if (midC > 10500) midC = 10500;
      }
      const u = rng();
      const side = rng() < 0.5 ? 0 : 1;
      const szArr = side === 0 ? bidSz : askSz;
      const nArr = side === 0 ? bidN : askN;
      if (u < 0.1) {
        const level = Math.floor(rng() * 4);
        const qty = 1 + Math.floor(rng() * 8);
        const pxC = side === 1 ? midC + ao + level : midC - bo - level;
        const v = szArr[level] - qty; szArr[level] = v < 0 ? 0 : v;
        nArr[level] = nArr[level] > 0 ? nArr[level] - 1 : 0;
        if (szArr[level] === 0) nArr[level] = 0;
        if (tc === T) { tpx.copyWithin(0, 1); tsz.copyWithin(0, 1); tsd.copyWithin(0, 1); tc--; }
        tpx[tc] = pxC; tsz[tc] = qty; tsd[tc] = side === 1 ? 1 : -1; tc++;
        lastPx = pxC;
      } else if (u < 0.3) {
        const level = Math.floor(rng() * L);
        szArr[level] = 5 + Math.floor(rng() * 95);
        nArr[level] = 1 + Math.floor(rng() * 9);
      } else if (u < 0.85) {
        let delta = Math.floor(rng() * 25) - 12;
        if (delta === 0) delta = 1;
        const level = Math.floor(rng() * L);
        const v = szArr[level] + delta; szArr[level] = v < 0 ? 0 : v;
        if (szArr[level] === 0) nArr[level] = 0;
      } else {
        const level = Math.floor(rng() * L);
        const qty = 1 + Math.floor(rng() * 6);
        const v = szArr[level] - qty; szArr[level] = v < 0 ? 0 : v;
        nArr[level] = nArr[level] > 0 ? nArr[level] - 1 : 0;
        if (szArr[level] === 0) nArr[level] = 0;
      }
      folded++;
    }
    // Ring protocol: invalidate -> fill -> re-stamp -> flip latest.
    const k = (f - 1) % M;
    Atomics.store(ctrl, 2 + k, 0n);
    const v = views[k];
    v[0] = f; v[1] = folded; v[2] = midC / 100; v[3] = (bo + ao) / 100;
    let bd = 0, ad = 0;
    for (let i = 0; i < L; i++) { bd += bidSz[i]; ad += askSz[i]; }
    v[4] = bd; v[5] = ad; v[6] = lastPx / 100;
    for (let i = 0; i < L; i++) {
      const o = 8 + i * 6;
      v[o] = (midC - bo - i) / 100; v[o + 1] = bidSz[i]; v[o + 2] = bidN[i];
      v[o + 3] = (midC + ao + i) / 100; v[o + 4] = askSz[i]; v[o + 5] = askN[i];
    }
    for (let j = 0; j < T; j++) {
      const o = 8 + L * 6 + j * 3;
      if (j < tc) { v[o] = tpx[j] / 100; v[o + 1] = tsz[j]; v[o + 2] = tsd[j]; }
      else { v[o] = 0; v[o + 1] = 0; v[o + 2] = 0; }
    }
    v[7] = checksum(v);
    Atomics.store(ctrl, 2 + k, BigInt(f));
    Atomics.store(ctrl, 0, BigInt(f));
    Atomics.add(ctrl, 1, 1n);
  }
  parentPort.postMessage({ kind: 'done', published: frames });
});
`;

describe('fanout feed cross-thread protocol litmus', () => {
  it(
    'independent worker feed writer + 3 main-thread readers: zero invalid claims (M=4, P=584, 40k frames = 2M messages)',
    { timeout: 60000 },
    async () => {
      const M = 4;
      const FRAMES = 40000;
      const b = new WeftFanoutBroadcaster(W6_FLOAT_COUNT, M);
      const readers: WeftFanoutReader[] = [
        b.createReader(),
        b.createReader(),
        b.createReader(),
      ];
      const paces = [0, 300, 3000]; // burn work between claims: distinct rates

      const w = new Worker(INDEPENDENT_FEED_WRITER_SRC, { eval: true });
      let finished = false;
      const done = new Promise<{ published: number }>((resolve) => {
        w.on('message', (m: { kind: string; published: number }) => {
          if (m.kind === 'done') {
            finished = true;
            resolve(m);
          }
        });
      });
      w.postMessage({
        kind: 'start',
        sab: b.sab,
        slotCount: M,
        payloadFloats: W6_FLOAT_COUNT,
        frames: FRAMES,
      });

      const burn = (n: number) => {
        let s = 0;
        for (let i = 0; i < n; i++) s += Math.sqrt(i);
        return s;
      };
      let invalid = 0;
      let seqRegression = 0;
      const prevSeq = [0, 0, 0];
      while (!finished) {
        for (let ri = 0; ri < readers.length; ri++) {
          const c = readers[ri].claim();
          if (c.fresh) {
            if (!validateFeedFrame(readers[ri].view(), c.seq)) invalid++;
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
      for (let ri = 0; ri < readers.length; ri++) {
        const c = readers[ri].claim();
        if (c.fresh) {
          if (!validateFeedFrame(readers[ri].view(), c.seq)) invalid++;
          prevSeq[ri] = c.seq;
        }
      }

      // THE headline assertion: zero invalid payloads across 2M folded
      // messages — every fresh claim carried a checksum-consistent,
      // frame-seq-identical, structurally valid book.
      expect(invalid).toBe(0);
      expect(seqRegression).toBe(0);
      for (let ri = 0; ri < readers.length; ri++) {
        const st = readers[ri].stats();
        expect(st.fresh).toBeGreaterThan(0);
        expect(prevSeq[ri]).toBeLessThanOrEqual(msg.published);
        // Telescoping identity: sum(dropped) == lastSeq - freshClaims.
        expect(st.drops).toBe(prevSeq[ri] - st.fresh);
        expect(st.reads).toBeGreaterThanOrEqual(st.fresh);
      }
      // Cross-implementation telemetry agreement.
      expect(b.debugStats().publishes).toBe(BigInt(FRAMES));
      expect(b.debugStats().latestSeq).toBe(BigInt(FRAMES));
      await w.terminate();
    }
  );
});
