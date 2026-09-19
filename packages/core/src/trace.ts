// trace.ts — RFC 0014 .weftrec v4 kernel trace events, TS port.
//
// The event kinds, the packed record (8 bytes: u16 kind | u16 aux | u32
// data), and the deterministic scenario are single-sourced in RFC 0014 and
// mirrored byte-for-byte by every port; the C reference codec
// (core/c/trace_rec.c) containers the stream. The packed stream is the
// cross-port byte-identity surface (fixtures/xlang-trace/): same scenario,
// same bytes — a provable property, not a statistical one.

import { Weft, PubResult, xorshift32 } from './weft.ts';

export const TraceKind = {
  Publish: 1,
  Claim: 2,
  Drop: 3,
  Revoke: 4,
  Ack: 5,
  Stall: 6,
  Tear: 7,
  CanaryFail: 8,
} as const;

export interface TraceEvent {
  kind: number;   // u16
  aux: number;    // u16 (publish: payload_len; else 0)
  data: number;   // u32 (seq / epoch / attempts)
}

/// Pack one event — 8 bytes, the exact C weft_trace_event_pack bit pattern.
export function traceEventPack(e: TraceEvent): Uint8Array {
  const b = new Uint8Array(8);
  b[0] = e.kind & 0xFF; b[1] = (e.kind >>> 8) & 0xFF;
  b[2] = e.aux & 0xFF;  b[3] = (e.aux >>> 8) & 0xFF;
  b[4] = e.data & 0xFF; b[5] = (e.data >>> 8) & 0xFF;
  b[6] = (e.data >>> 16) & 0xFF; b[7] = (e.data >>> 24) & 0xFF;
  return b;
}

export const SCENARIO_PAYLOAD_MAX = 64;
export const SCENARIO_REVOKE_DIVISOR = 2;

/// The deterministic scenario (RFC 0014 §parity-scenario) — identical to
/// core/c/trace_dump.c scenario_run(). Emits the packed stream (no CRC):
/// a pure function of (n, seed). Single-threaded: no scheduler noise, no
/// wall clock anywhere.
export function traceScenario(n: number, seed: number): Uint8Array {
  const w = new Weft(SCENARIO_PAYLOAD_MAX);
  const state = { v: seed ? seed >>> 0 : 0x9E3779B9 };  // boxed: xorshift32 mutates
  let seq = 0;
  const events: TraceEvent[] = [];
  const revokeStep = Math.floor(n / SCENARIO_REVOKE_DIVISOR);

  for (let step = 0; step < n; step++) {
    const plen = xorshift32(state) % (SCENARIO_PAYLOAD_MAX + 1);
    seq += 1;
    w.fillPayload(seq, plen);
    const r = w.publish(seq, plen);
    if (r === PubResult.DroppedRevoked) {
      const ep = Number(w.epoch());
      events.push({ kind: TraceKind.Drop, aux: 0, data: seq >>> 0 });
      events.push({ kind: TraceKind.Ack, aux: 0, data: ep >>> 0 });
    } else {
      events.push({ kind: TraceKind.Publish, aux: plen, data: seq >>> 0 });
    }
    if (xorshift32(state) % 3 === 0) {
      w.claim();
      events.push({ kind: TraceKind.Claim, aux: 0, data: w.rSeq() >>> 0 });
      // The boundary must hold on every claimed frame (TIER4 §3).
      const rWork = w.ctrl[Weft.SLOT_R_WORK];
      const base = w.buf0Offset + rWork * w.bufSize;
      const canary = w.dv.getBigUint64(base + w.bufSize - 8, true);
      if (canary !== BigInt(w.rSeq() >>> 0)) {
        events.push({ kind: TraceKind.CanaryFail, aux: 0, data: w.rSeq() >>> 0 });
        throw new Error(`scenario: canary check FAILED at step ${step}`);
      }
    }
    if (step === revokeStep) {
      const e0 = w.epoch();
      w.revoke();
      events.push({ kind: TraceKind.Revoke, aux: 0, data: e0 });
    }
  }

  const out = new Uint8Array(events.length * 8);
  for (let i = 0; i < events.length; i++) {
    out.set(traceEventPack(events[i]), i * 8);
  }
  return out;
}

export function toHex(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
  return s;
}
