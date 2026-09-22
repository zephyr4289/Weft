// wide.ts — >= 1e7 state-space exploration tally.

import type { WideResult } from './types.ts';

const CAPACITY = 1 << 24; // 16,777,216 key slots (~134 MiB float64 + 16 MiB occupancy)
const MASK = CAPACITY - 1;
const CHUNK = 1 << 20;

const MOD256 = 255;

export function runWideTally(target: number, deadlineMs: number): WideResult {
  const startNs = process.hrtime.bigint();
  const tab = new Float64Array(CAPACITY);
  const occ = new Uint8Array(CAPACITY);

  const blocks: Float64Array[] = [new Float64Array(CHUNK)];
  let head = 0;
  let tail = 0;
  const push = (key: number): void => {
    const bi = tail >>> 20;
    if (bi >= blocks.length) blocks.push(new Float64Array(CHUNK));
    blocks[bi][tail & (CHUNK - 1)] = key;
    tail++;
  };

  const insert = (key: number): boolean => {
    const lo = key % 4294967296;
    const hi = (key - lo) / 4294967296;
    let h = (Math.imul(lo ^ 0x9e3779b9, 0x85ebca6b) ^ Math.imul(hi ^ 0xc2b2ae35, 0x27d4eb2f)) | 0;
    h = (h ^ (h >>> 15)) >>> 0;
    let idx = h & MASK;
    while (occ[idx] !== 0) {
      if (tab[idx] === key) return false;
      idx = (idx + 1) & MASK;
    }
    occ[idx] = 1;
    tab[idx] = key;
    return true;
  };

  const dead = Date.now() + deadlineMs;

  const initial = 0;
  insert(initial);
  push(initial);
  let explored = 1;
  let transitions = 0;
  let deadlineHit = false;
  let guard = 0;

  while (head < tail && explored < target) {
    if ((guard++ & 0xffff) === 0 && Date.now() > dead) {
      deadlineHit = true;
      break;
    }
    const blockIdx = head >>> 20;
    const off = head & (CHUNK - 1);
    const key = blocks[blockIdx][off];
    head++;

    const lo = key % 4294967296;
    const hi = (key - lo) / 4294967296;
    const v0 = lo & MOD256;
    const v1 = (lo >>> 8) & MOD256;
    const v2 = (lo >>> 16) & MOD256;
    const v3 = (lo >>> 24) & MOD256;
    const w1 = hi & 3;
    const w2 = (hi >>> 2) & 3;
    const rp = (hi >>> 4) & 3;
    const ph = (hi >>> 6) & 3;
    const pv = (hi >>> 8) & MOD256;
    const tn = (hi >>> 16) & 3;

    const getV = (i: number): number => (i === 0 ? v0 : i === 1 ? v1 : i === 2 ? v2 : v3);

    const pack = (
      a0: number, a1: number, a2: number, a3: number,
      nw1: number, nw2: number, nrp: number, nph: number, npv: number, ntn: number,
    ): number => {
      const nlo = (a0 | (a1 << 8) | (a2 << 16) | (a3 << 24)) >>> 0;
      const nhi = nw1 | (nw2 << 2) | (nrp << 4) | (nph << 6) | (npv << 8) | (ntn << 16);
      return nhi * 4294967296 + nlo;
    };

    {
      const i = w1;
      const nv = (getV(i) + 1) & MOD256;
      const a0 = i === 0 ? nv : v0;
      const a1 = i === 1 ? nv : v1;
      const a2 = i === 2 ? nv : v2;
      const a3 = i === 3 ? nv : v3;
      const t = pack(a0, a1, a2, a3, (w1 + 1) & 3, w2, rp, ph, pv, tn);
      transitions++;
      if (insert(t)) {
        explored++;
        push(t);
        if (explored >= target) break;
      }
    }
    {
      const i = w2;
      const nv = (getV(i) + 1) & MOD256;
      const a0 = i === 0 ? nv : v0;
      const a1 = i === 1 ? nv : v1;
      const a2 = i === 2 ? nv : v2;
      const a3 = i === 3 ? nv : v3;
      const t = pack(a0, a1, a2, a3, w1, (w2 + 1) & 3, rp, ph, pv, tn);
      transitions++;
      if (insert(t)) {
        explored++;
        push(t);
        if (explored >= target) break;
      }
    }
    if (ph === 0) {
      const t = pack(v0, v1, v2, v3, w1, w2, rp, 1, getV(rp), tn);
      transitions++;
      if (insert(t)) {
        explored++;
        push(t);
        if (explored >= target) break;
      }
    }
    else if (ph === 1) {
      const stable = pv === getV(rp) && (pv & 1) === 0;
      const t = stable
        ? pack(v0, v1, v2, v3, w1, w2, rp, 2, pv, tn)
        : pack(v0, v1, v2, v3, w1, w2, rp, 0, pv, (tn + 1) & 3);
      transitions++;
      if (insert(t)) {
        explored++;
        push(t);
        if (explored >= target) break;
      }
    }
    else if (ph === 2) {
      const t = pack(v0, v1, v2, v3, w1, w2, (rp + 1) & 3, 0, pv, tn);
      transitions++;
      if (insert(t)) {
        explored++;
        push(t);
        if (explored >= target) break;
      }
    }
  }

  const endNs = process.hrtime.bigint();
  const seconds = Math.round((Number(endNs - startNs) / 1e6) / 100) / 10;
  return {
    target,
    explored,
    transitions,
    truncated: explored >= target,
    deadlineHit,
    exhaustive: head >= tail && !deadlineHit,
    seconds: Math.min(seconds, 9999),
    capacity: CAPACITY,
    loadFactor: Math.round((explored / CAPACITY) * 1000) / 1000,
  };
}
