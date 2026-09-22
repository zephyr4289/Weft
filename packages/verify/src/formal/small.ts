// small.ts — exhaustive bounded model checks of Weft publication protocols.

import type { SmallModelResult, InvariantResult, ProgressResult } from './types.ts';

const FREE = 0;
const WRITING = 1;
const COMMITTED = 2;
const READ = 3;
const DROPPED = 4;

const ACT_NONE = 0;
const ACT_COMMIT = 1;
const ACT_CONSUME = 2;
const ACT_WCOMMIT = 3;

interface ExploreStats {
  states: number;
  transitions: number;
  maxDepth: number;
  deadlocks: number;
  tornRetriesObserved: number;
}

interface BfsOutput extends ExploreStats {
  adjStart: Int32Array;
  adjTarget: Int32Array;
  adjAct: Uint8Array;
  keys: number[];
}

function bfs(
  initial: number,
  succ: (s: number, out: Int32Array) => number,
  scratch: Int32Array,
  cap: number,
): BfsOutput {
  const seen = new Set<number>([initial]);
  const keys: number[] = [initial];
  const index = new Map<number, number>([[initial, 0]]);
  const queue: number[] = [initial];
  const depth: number[] = [0];

  const edgeFrom: number[] = [];
  const edgeTo: number[] = [];
  const edgeAct: number[] = [];
  let transitions = 0;
  let maxDepth = 0;
  let tornMax = 0;

  for (let qi = 0; qi < queue.length; qi++) {
    const s = queue[qi];
    const d = depth[qi];
    if (d + 1 > maxDepth) maxDepth = d + 1 > maxDepth ? d + 1 : maxDepth;
    const n = succ(s, scratch);
    if (n === 0) continue; // deadlock bookkeeping happens post-hoc via CSR
    for (let k = 0; k < n; k++) {
      const t = scratch[k * 2];
      const act = scratch[k * 2 + 1];
      transitions++;
      let ti = index.get(t);
      if (ti === undefined) {
        if (keys.length >= cap) {
          throw new Error(`small-model state cap ${cap} exceeded — model is misconfigured`);
        }
        ti = keys.length;
        index.set(t, ti);
        keys.push(t);
        queue.push(t);
        depth.push(d + 1);
        const tn = extractTorn(t);
        if (tn > tornMax) tornMax = tn;
      }
      edgeFrom.push(qi);
      edgeTo.push(ti);
      edgeAct.push(act);
    }
  }

  const n = keys.length;
  const adjStart = new Int32Array(n + 1);
  const adjTarget = new Int32Array(edgeTo.length);
  const adjAct = new Uint8Array(edgeAct.length);
  for (let e = 0; e < edgeTo.length; e++) adjStart[edgeFrom[e] + 1]++;
  for (let i = 0; i < n; i++) adjStart[i + 1] += adjStart[i];
  const cursor = Int32Array.from(adjStart);
  for (let e = 0; e < edgeTo.length; e++) {
    const row = edgeFrom[e];
    adjTarget[cursor[row]] = edgeTo[e];
    adjAct[cursor[row]] = edgeAct[e];
    cursor[row]++;
  }

  let deadlocks = 0;
  for (let i = 0; i < n; i++) {
    if (adjStart[i + 1] === adjStart[i]) deadlocks++;
  }

  return {
    states: n,
    transitions,
    maxDepth,
    deadlocks,
    tornRetriesObserved: tornMax,
    adjStart,
    adjTarget,
    adjAct,
    keys,
  };
}

function extractTorn(state: number): number {
  return (state >> 18) & 3;
}

/** Bounded existential progress: is an action in `targetActs` performable
 *  within `bound` steps from EVERY reachable state? */
function progressCheck(
  g: BfsOutput,
  targetActs: number[],
  bound: number,
): { proved: boolean; maxSteps: number } {
  const n = g.states;
  const dist = new Int32Array(n).fill(-1); // -1 = unreached
  const frontier: number[] = [];
  for (let i = 0; i < n; i++) {
    for (let e = g.adjStart[i]; e < g.adjStart[i + 1]; e++) {
      let hit = false;
      for (const a of targetActs) {
        if (g.adjAct[e] === a) {
          hit = true;
          break;
        }
      }
      if (hit) {
        dist[i] = 0;
        frontier.push(i);
        break;
      }
    }
  }
  let maxSteps = 0;
  let d = 0;
  while (frontier.length > 0 && d < bound) {
    const next: number[] = [];
    for (let i = 0; i < n; i++) {
      if (dist[i] !== -1) continue;
      for (let e = g.adjStart[i]; e < g.adjStart[i + 1]; e++) {
        if (dist[g.adjTarget[e]] === d) {
          dist[i] = d + 1;
          next.push(i);
          if (d + 1 > maxSteps) maxSteps = d + 1;
          break;
        }
      }
    }
    for (const x of next) frontier.push(x);
    if (next.length === 0) break;
    d++;
  }
  let proved = true;
  for (let i = 0; i < n; i++) {
    if (dist[i] === -1) {
      proved = false;
      break;
    }
  }
  return { proved, maxSteps };
}

function packA(st: Int32Array, ver: Int32Array, ws: number, wh: number, rs: number, rh: number): number {
  return (
    (st[0] | (st[1] << 3) | (st[2] << 6) |
      (ver[0] << 9) | (ver[1] << 11) | (ver[2] << 13) |
      (ws << 15) | (wh << 17) | (rs << 18) | (rh << 20)) >>> 0
  );
}

function succA(s: number, out: Int32Array): number {
  const st = [(s) & 7, (s >> 3) & 7, (s >> 6) & 7];
  const ver = [(s >> 9) & 3, (s >> 11) & 3, (s >> 13) & 3];
  const ws = (s >> 15) & 3;
  const wh = (s >> 17) & 1;
  const rs = (s >> 18) & 3;
  const rh = (s >> 20) & 1;
  let n = 0;

  const emit = (t: number, act: number): void => {
    out[n * 2] = t;
    out[n * 2 + 1] = act;
    n++;
  };

  if (wh === 0) {
    for (let slot = 0; slot < 3; slot++) {
      if (st[slot] === FREE || st[slot] === DROPPED) {
        const nst = st.slice();
        nst[slot] = WRITING;
        emit(packA(nst, ver, slot, 1, rs, rh), ACT_NONE);
      }
    }
  } else {
    const nst = st.slice();
    const nver = ver.slice();
    nst[ws] = COMMITTED;
    nver[ws] = (nver[ws] + 1) & 3;
    emit(packA(nst, nver, 3, 0, rs, rh), ACT_COMMIT);
    const ast = st.slice();
    ast[ws] = DROPPED;
    emit(packA(ast, ver, 3, 0, rs, rh), ACT_NONE);
  }

  if (rh === 0) {
    for (let slot = 0; slot < 3; slot++) {
      if (st[slot] === COMMITTED) {
        const nst = st.slice();
        nst[slot] = READ;
        emit(packA(nst, ver, ws, wh, slot, 1), ACT_NONE);
      }
    }
  } else if (st[rs] === READ) {
    const nst = st.slice();
    nst[rs] = FREE;
    emit(packA(nst, ver, ws, wh, 3, 0), ACT_CONSUME);
  }
  return n;
}

function checkA(s: number): string | null {
  const st = [(s) & 7, (s >> 3) & 7, (s >> 6) & 7];
  const ws = (s >> 15) & 3;
  const wh = (s >> 17) & 1;
  const rs = (s >> 18) & 3;
  const rh = (s >> 20) & 1;
  for (let i = 0; i < 3; i++) {
    if (st[i] > DROPPED) return `slot ${i} state out of range`;
  }
  if (wh === 1 && st[ws] !== WRITING) {
    return `writer holds slot ${ws} in state ${st[ws]} (expected WRITING)`;
  }
  if (rh === 1) {
    if (st[rs] !== READ) return `reader holds slot ${rs} in state ${st[rs]} (expected READ)`;
    if (wh === 1 && ws === rs) return `writer and reader both hold slot ${ws}`;
  }
  let writing = 0;
  for (let i = 0; i < 3; i++) if (st[i] === WRITING) writing++;
  if (wh === 1 && writing !== 1) return `${writing} WRITING slots with writer holding`;
  if (wh === 0 && writing !== 0) return `${writing} WRITING slots with no writer`;
  let reading = 0;
  for (let i = 0; i < 3; i++) if (st[i] === READ) reading++;
  if (rh === 1 && reading !== 1) return `${reading} READ slots with reader holding`;
  if (rh === 0 && reading !== 0) return `${reading} READ slots with no reader`;
  return null;
}

function packB(ver: Int32Array, wpos: number, rpos: number, phase: number, prever: number, torn: number): number {
  return (
    (ver[0] | (ver[1] << 3) | (ver[2] << 6) |
      (wpos << 9) | (rpos << 11) | (phase << 13) | (prever << 15) | (torn << 18)) >>> 0
  );
}

function succB(s: number, out: Int32Array): number {
  const ver = [(s) & 7, (s >> 3) & 7, (s >> 6) & 7];
  const wpos = (s >> 9) & 3;
  const rpos = (s >> 11) & 3;
  const phase = (s >> 13) & 3;
  const prever = (s >> 15) & 7;
  const torn = (s >> 18) & 3;
  let n = 0;

  const emit = (t: number, act: number): void => {
    out[n * 2] = t;
    out[n * 2 + 1] = act;
    n++;
  };

  const nver = ver.slice();
  nver[wpos] = (nver[wpos] + 1) & 7;
  emit(packB(nver, (wpos + 1) % 3, rpos, phase, prever, torn), ACT_WCOMMIT);

  if (phase === 0) {
    emit(packB(ver, wpos, rpos, 1, ver[rpos], torn), ACT_NONE);
  } else if (phase === 1) {
    if (prever === ver[rpos] && (prever & 1) === 0) {
      emit(packB(ver, wpos, rpos, 2, prever, torn), ACT_NONE);
    } else {
      emit(packB(ver, wpos, rpos, 0, prever, (torn + 1) & 3), ACT_NONE);
    }
  } else if (phase === 2) {
    emit(packB(ver, wpos, (rpos + 1) % 3, 0, prever, torn), ACT_CONSUME);
  }
  return n;
}

function checkB(s: number): string | null {
  const phase = (s >> 13) & 3;
  const prever = (s >> 15) & 7;
  const wpos = (s >> 9) & 3;
  const rpos = (s >> 11) & 3;
  if (wpos > 2) return 'writer cursor out of range';
  if (rpos > 2) return 'reader cursor out of range';
  if (phase === 2 && (prever & 1) !== 0) {
    return `stable copy captured from odd (mid-write) version ${prever}`;
  }
  return null;
}

export const SMALL_MODEL_CAP = 2_000_000;

function inv(id: string, name: string, violations: Map<number, string>, states: number): InvariantResult {
  if (violations.size === 0) {
    return { id, name, verdict: 'PROVED', evidence: `0 violations across ${states} reachable states` };
  }
  const first = violations.entries().next();
  const detail = first.done ? 'violation' : first.value[1];
  return { id, name, verdict: 'FAILED', evidence: `${violations.size} violation(s); first: ${detail}` };
}

export function runSmallModels(): { results: SmallModelResult[]; allProved: boolean } {
  const results: SmallModelResult[] = [];
  const scratch = new Int32Array(16);

  {
    const initial = packA([FREE, FREE, FREE], [0, 0, 0], 3, 0, 3, 0);
    const g = bfs(initial, succA, scratch, SMALL_MODEL_CAP);
    const violations = new Map<number, string>();
    for (let i = 0; i < g.keys.length; i++) {
      const v = checkA(g.keys[i]);
      if (v !== null) violations.set(i, v);
    }
    const commitProg = progressCheck(g, [ACT_COMMIT], 10);
    const readProg = progressCheck(g, [ACT_CONSUME], 10);
    const invariants: InvariantResult[] = [
      inv('A-I1', 'writer-holds-WRITING-slot', violations, g.states),
      inv('A-I2', 'writer-reader-slot-exclusion', violations, g.states),
      inv('A-I3', 'single-writer-exclusivity', violations, g.states),
      inv('A-I4', 'single-reader-slot-ownership', violations, g.states),
      inv('A-I5', 'type-integrity', violations, g.states),
    ];
    const progress: ProgressResult[] = [
      { name: 'commit-progress', bound: 10, proved: commitProg.proved, maxSteps: commitProg.maxSteps },
      { name: 'read-progress', bound: 10, proved: readProg.proved, maxSteps: readProg.maxSteps },
    ];
    const proved = violations.size === 0 && g.deadlocks === 0 && commitProg.proved && readProg.proved;
    results.push({
      model: 'triad-handoff',
      states: g.states,
      transitions: g.transitions,
      maxDepth: g.maxDepth,
      deadlocks: g.deadlocks,
      tornRetriesObserved: g.tornRetriesObserved,
      invariants,
      progress,
      verdict: proved ? 'PROVED' : 'FAILED',
    });
  }

  {
    const initial = packB([0, 0, 0], 0, 0, 0, 0, 0);
    const g = bfs(initial, succB, scratch, SMALL_MODEL_CAP);
    const violations = new Map<number, string>();
    for (let i = 0; i < g.keys.length; i++) {
      const v = checkB(g.keys[i]);
      if (v !== null) violations.set(i, v);
    }
    const readProg = progressCheck(g, [ACT_CONSUME], 8);
    const writeProg = progressCheck(g, [ACT_WCOMMIT], 3);
    const invariants: InvariantResult[] = [
      inv('B-J1', 'parity-gated-publication', violations, g.states),
      inv('B-J2', 'cursor-range-integrity', violations, g.states),
    ];
    const progress: ProgressResult[] = [
      { name: 'clean-read-progress', bound: 8, proved: readProg.proved, maxSteps: readProg.maxSteps },
      { name: 'writer-progress', bound: 3, proved: writeProg.proved, maxSteps: writeProg.maxSteps },
    ];
    const proved = violations.size === 0 && g.deadlocks === 0 && readProg.proved && writeProg.proved;
    results.push({
      model: 'seqlock-parity',
      states: g.states,
      transitions: g.transitions,
      maxDepth: g.maxDepth,
      deadlocks: g.deadlocks,
      tornRetriesObserved: g.tornRetriesObserved,
      invariants,
      progress,
      verdict: proved ? 'PROVED' : 'FAILED',
    });
  }

  const allProved = results.every((r) => r.verdict === 'PROVED');
  return { results, allProved };
}
