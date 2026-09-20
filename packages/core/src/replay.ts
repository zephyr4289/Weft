// replay.ts — RFC 0019 deterministic time-travel replay fold, TS port.
//
// Mirror of core/c/weft_replay.{h,c}: a pure state machine over a .weftrec
// v4 event stream that reconstructs the shadow kernel state per event and
// reduces it to a 64-bit FNV-1a state hash. The transition table and the
// 111-byte canonical serialization are NORMATIVE (RFC-0019) — every port
// must produce byte-identical hash logs (fixtures/xlang-replay/).
//
// Pinned parity vectors (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (the RFC-0019 fixture grammar)
//
// u64 care: FNV-1a is computed with BigInt (not a hot path — the fold is a
// debugger); state counters fit doubles (<= 2^53). Shifts use >>> / Math.
// floor to match the C unsigned/arithmetic semantics exactly.

import type { TraceEvent } from './trace';

export const WEFT_REPLAY_CHECKPOINT = 64;

export enum ReplayResult {
  Ok = 0,
  Disagree = -1,
  BadKind = -2,
}

export interface ReplayBuf {
  seq: number;   // u32
  len: number;   // u32
  ver: number;   // u16
}

export interface ReplayState {
  latest: number;
  epoch: number;
  w_work: number;
  r_work: number;
  revoked: number;      // 0/1
  buf: [ReplayBuf, ReplayBuf, ReplayBuf];
  t_publish: number;
  t_claim: number;
  t_drop: number;
  t_invalid: number;
  t_wsteps: number;
  t_rsteps: number;
  t_stall: number;
  t_tear: number;
  t_canary: number;
  step: number;
  hash: bigint;         // u64 as BigInt
}

const FNV_OFFSET = 0xcbf29ce484222325n;
const FNV_PRIME = 0x100000001b3n;

/// FNV-1a 64 over a byte range — one definition shared by every fold path.
export function replayFnv1a(data: Uint8Array): bigint {
  let h = FNV_OFFSET;
  for (let i = 0; i < data.length; i++) {
    h ^= BigInt(data[i]);
    h *= FNV_PRIME;
    h &= 0xffffffffffffffffn;
  }
  return h;
}

/// Canonical little-endian serialization (RFC-0019, 111 bytes).
export function replaySerialize(s: ReplayState): Uint8Array {
  const out = new Uint8Array(111);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, s.latest >>> 0, true);
  dv.setUint32(4, s.epoch >>> 0, true);
  dv.setUint32(8, s.w_work >>> 0, true);
  dv.setUint32(12, s.r_work >>> 0, true);
  out[16] = s.revoked & 1;
  let off = 17;
  for (let i = 0; i < 3; i++) {
    dv.setUint32(off, s.buf[i].seq >>> 0, true); off += 4;
    dv.setUint32(off, s.buf[i].len >>> 0, true); off += 4;
    dv.setUint16(off, s.buf[i].ver & 0xffff, true); off += 2;
  }
  // 6 x u64 counters — values <= 2^53 fit Number exactly; write via BigInt
  const counters = [s.t_publish, s.t_claim, s.t_drop, s.t_invalid,
                    s.t_wsteps, s.t_rsteps];
  for (const c of counters) {
    dv.setBigUint64(off, BigInt(c), true); off += 8;
  }
  dv.setUint32(off, s.t_stall >>> 0, true); off += 4;
  dv.setUint32(off, s.t_tear >>> 0, true); off += 4;
  dv.setUint32(off, s.t_canary >>> 0, true); off += 4;
  dv.setUint32(off, s.step >>> 0, true); off += 4;
  return out;
}

function hashOf(s: ReplayState): bigint {
  return replayFnv1a(replaySerialize(s));
}

/// Reset to the RFC-0019 initial state (including the pinned step-0 hash).
export function replayInit(s: ReplayState): void {
  s.latest = 0;
  s.epoch = 0;
  s.w_work = 1;
  s.r_work = 2;
  s.revoked = 0;
  for (let i = 0; i < 3; i++) s.buf[i] = { seq: 0, len: 0, ver: 1 };
  s.t_publish = 0; s.t_claim = 0; s.t_drop = 0; s.t_invalid = 0;
  s.t_wsteps = 0; s.t_rsteps = 0;
  s.t_stall = 0; s.t_tear = 0; s.t_canary = 0;
  s.step = 0;
  s.hash = hashOf(s);
}

export function replayNew(): ReplayState {
  const s = Object.create(null) as unknown as ReplayState;
  s.buf = [null, null, null] as unknown as [ReplayBuf, ReplayBuf, ReplayBuf];
  replayInit(s);
  return s;
}

/// Apply ONE event. Returns Ok and updates s.hash, or a verdict WITHOUT
/// mutating state (a disagreement leaves the fold at the last good step).
export function replayStep(s: ReplayState, e: TraceEvent): ReplayResult {
  // speculative copy: disagreement leaves the caller's state untouched
  const next: ReplayState = {
    ...s,
    buf: [{ ...s.buf[0] }, { ...s.buf[1] }, { ...s.buf[2] }],
  };
  switch (e.kind) {
    case 1: {  // PUBLISH: aux = len, data = seq
      const w = next.w_work;
      next.buf[w] = { seq: e.data >>> 0, len: e.aux >>> 0, ver: 1 };
      const old = next.latest;
      next.latest = next.w_work;
      next.w_work = old;
      next.revoked = 0;  // modeling rule 2: publish implies rebind
      next.t_publish++;
      next.t_wsteps++;
      break;
    }
    case 2: {  // CLAIM: data = claimed seq
      const mine = next.latest;
      next.latest = next.r_work;
      next.r_work = mine;
      if (next.buf[mine].seq !== (e.data >>> 0)) {
        return ReplayResult.Disagree;  // rule 3: never silent
      }
      next.t_claim++;
      next.t_rsteps++;
      break;
    }
    case 3:  // DROP: aux = epoch at ACK
      next.epoch = e.aux >>> 0;
      next.t_drop++;
      break;
    case 4:  // REVOKE
      next.revoked = 1;
      break;
    case 5:  // ACK: data = epoch after ACK
      next.epoch = e.data >>> 0;
      break;
    case 6: next.t_stall++; break;
    case 7: next.t_tear++; break;
    case 8: next.t_canary++; break;
    default:
      return ReplayResult.BadKind;
  }
  next.step++;
  next.hash = hashOf(next);
  Object.assign(s, next);
  s.buf = next.buf;
  return ReplayResult.Ok;
}

/// Fold n events, writing per-step hashes (BigInt[]) when hashes is given.
export function replayFold(s: ReplayState, evs: TraceEvent[],
                           hashes?: bigint[]): ReplayResult {
  let rc = ReplayResult.Ok;
  for (let i = 0; i < evs.length; i++) {
    rc = replayStep(s, evs[i]);
    if (rc !== ReplayResult.Ok) return rc;
    if (hashes) hashes.push(s.hash);
  }
  return rc;
}

/// Hash as the 16-hex-digit lowercase log token (the fixture surface).
export function hashHex(h: bigint): string {
  return h.toString(16).padStart(16, '0');
}
