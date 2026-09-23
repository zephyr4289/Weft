// replay.test.ts — RFC 0019 R-series, TS port (mirrors core/c/weft_replay_test.c).
//
// The pinned parity vectors are the CONTRACT: init 0x8a769a0111cf3af3 and
// the 100k soak 0x26beb484733ecde0 must match the C reference exactly —
// they are what fixtures/xlang-replay byte-compares across all runtimes.

import { describe, it, expect } from 'vitest';
import {
  replayNew, replayInit, replayStep, replayFold, replaySerialize,
  replayFnv1a, hashHex, ReplayResult, ReplayState,
} from '../src/replay';
import type { TraceEvent } from '../src/trace';

// The shared deterministic scenario (RFC-0019 fixture grammar — mirrored
// EXACTLY by core/c/replay_runner.c and every other port's emitter).
function scenNext(c: ScenarioState, state: { v: number }): TraceEvent {
  const xs = (x: number): number => {
    let y = x | 0;
    y = (y ^ ((y << 13) | 0)) | 0;
    y = (y ^ (y >>> 17)) | 0;
    y = (y ^ ((y << 5) | 0)) | 0;
    return y | 0;
  };
  state.v = xs(state.v) | 0;
  const op = state.v & 15;
  const u = state.v >>> 0;
  if (op < 7) {
    c.seq = (c.seq + 1) >>> 0;
    const len = Math.floor(u / 16) % 1024;
    if (!c.revoked) {
      c.bufseq[c.w_work] = c.seq;
      const old = c.latest;
      c.latest = c.w_work; c.w_work = old;
      return { kind: 1, aux: len, data: c.seq };
    }
    c.epoch = (c.epoch + 1) >>> 0;
    return { kind: 3, aux: c.epoch & 0xffff, data: c.seq };
  }
  if (op < 12) {
    const data = c.bufseq[c.latest];
    const mine = c.latest;
    c.latest = c.r_work; c.r_work = mine;
    return { kind: 2, aux: 0, data };
  }
  if (op === 12) {
    if (!c.revoked) { c.revoked = 1; return { kind: 4, aux: 0, data: c.epoch }; }
    return { kind: 5, aux: 0, data: c.epoch };
  }
  if (op === 13) {
    if (c.revoked) { c.revoked = 0; return { kind: 5, aux: 0, data: c.epoch }; }
    return { kind: 6, aux: 0, data: Math.floor(u / 16) % 8 };
  }
  if (op === 14) return { kind: 7, aux: 0, data: c.seq };
  return { kind: 8, aux: 0, data: c.seq };
}

interface ScenarioState {
  latest: number; w_work: number; r_work: number; epoch: number;
  revoked: number; seq: number; bufseq: [number, number, number];
}
function scenInit(): ScenarioState {
  return { latest: 0, w_work: 1, r_work: 2, epoch: 0, revoked: 0, seq: 0,
           bufseq: [0, 0, 0] };
}

describe('RFC 0019 replay fold (TS port)', () => {
  it('R1: init state + pinned parity hash', () => {
    const s = replayNew();
    expect(s.latest).toBe(0);
    expect(s.w_work).toBe(1);
    expect(s.r_work).toBe(2);
    expect(s.hash).toBe(0x8a769a0111cf3af3n);
    expect(hashHex(s.hash)).toBe('8a769a0111cf3af3');
  });

  it('R2: PUBLISH transition (THE exchange)', () => {
    const s = replayNew();
    expect(replayStep(s, { kind: 1, aux: 64, data: 5 })).toBe(ReplayResult.Ok);
    expect(s.buf[1]).toEqual({ seq: 5, len: 64, ver: 1 });
    expect(s.latest).toBe(1);
    expect(s.w_work).toBe(0);
    expect(s.t_publish).toBe(1);
    // rotation: second publish takes the old latest
    replayStep(s, { kind: 1, aux: 16, data: 6 });
    expect(s.latest).toBe(0);
    expect(s.w_work).toBe(1);
    expect(s.buf[0].seq).toBe(6);
  });

  it('R3: CLAIM validation — disagreement leaves state untouched', () => {
    const s = replayNew();
    replayStep(s, { kind: 1, aux: 64, data: 5 });
    expect(replayStep(s, { kind: 2, aux: 0, data: 5 })).toBe(ReplayResult.Ok);
    expect(s.r_work).toBe(1);
    expect(s.latest).toBe(2);
    const before = s.hash;  // AFTER the good claim — the baseline to defend
    expect(replayStep(s, { kind: 2, aux: 0, data: 99 })).toBe(ReplayResult.Disagree);
    expect(s.hash).toBe(before);
    expect(s.t_claim).toBe(1);
  });

  it('R5: revocation window transitions', () => {
    const s = replayNew();
    replayStep(s, { kind: 1, aux: 8, data: 1 });
    replayStep(s, { kind: 4, aux: 0, data: 0 });
    expect(s.revoked).toBe(1);
    replayStep(s, { kind: 3, aux: 3, data: 9 });
    expect(s.epoch).toBe(3);
    expect(s.t_drop).toBe(1);
    replayStep(s, { kind: 5, aux: 0, data: 3 });
    expect(s.epoch).toBe(3);
    replayStep(s, { kind: 1, aux: 8, data: 2 });
    expect(s.revoked).toBe(0);  // publish implies rebind
  });

  it('R6: 111-byte canonical serialization layout', () => {
    const s = replayNew();
    const ser = replaySerialize(s);
    expect(ser.length).toBe(111);
    expect(ser[8]).toBe(1);   // w_work LE at offset 8
    expect(ser[12]).toBe(2);  // r_work at offset 12
    expect(ser[16]).toBe(0);  // revoked
  });

  it('R7: checkpoint jump equivalence', () => {
    const c = scenInit();
    const state = { v: 0x00C0FFEE | 0 };
    const evs: TraceEvent[] = [];
    for (let i = 0; i < 2000; i++) evs.push(scenNext(c, state));
    const s = replayNew();
    const hashes: bigint[] = [];
    expect(replayFold(s, evs, hashes)).toBe(ReplayResult.Ok);
    // checkpoints every 64 steps (struct copies — in-memory, replayStep
    // never mutates its input's buf, so a shallow struct copy is safe)
    const cps: ReplayState[] = [];
    const t = replayNew();
    for (let i = 0; i < 2000; i++) {
      if (i % 64 === 0) {
        cps.push({ ...t, buf: [{ ...t.buf[0] }, { ...t.buf[1] }, { ...t.buf[2] }] });
      }
      replayStep(t, evs[i]);
    }
    // jump(k) = restore checkpoint k/64 + refold to k
    for (const k of [0, 1, 63, 64, 65, 127, 128, 999, 1921, 1999]) {
      const cp = cps[Math.floor(k / 64)];
      const j: ReplayState = { ...cp, buf: [{ ...cp.buf[0] }, { ...cp.buf[1] }, { ...cp.buf[2] }] };
      for (let i = Math.floor(k / 64) * 64; i < k; i++) replayStep(j, evs[i]);
      const want = k === 0 ? cps[0].hash : hashes[k - 1];  // k=0: init hash
      expect(j.hash).toBe(want);
    }
  });

  it('R8: 100k soak — pinned cross-port parity vector', () => {
    const c = scenInit();
    const state = { v: 0x00C0FFEE | 0 };
    const s = replayNew();
    for (let i = 0; i < 100000; i++) {
      expect(replayStep(s, scenNext(c, state))).toBe(ReplayResult.Ok);
    }
    expect(s.step).toBe(100000);
    expect(s.hash).toBe(0x26beb484733ecde0n);
    expect(s.t_claim).toBeGreaterThan(1000);
    expect(s.t_publish).toBeGreaterThan(1000);
  });

  it('R9: unknown kind refused', () => {
    const s = replayNew();
    expect(replayStep(s, { kind: 99, aux: 0, data: 0 })).toBe(ReplayResult.BadKind);
  });

  it('fnv1a reference vectors', () => {
    expect(replayFnv1a(new Uint8Array(0))).toBe(0xcbf29ce484222325n);
    expect(replayFnv1a(new TextEncoder().encode('a'))).toBe(0xaf63dc4c8601ec8cn);
  });
});
