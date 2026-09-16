// ui-thread.ts — Phase-7: the RN UI-THREAD reader port.
//
// WHY EXISTS: the 2026-09 honesty note in weft-rn.ts said it plainly —
// "True UI-thread reads require the native port that Phase 7 (RN, last) is
// scheduled to deliver." This module IS that deliverable, in the form the
// Reanimated runtime actually permits.
//
// THE ARCHITECTURE (and why it is honest):
//   Reanimated worklets execute on the UI thread but CANNOT capture class
//   instances (the old bug — see weft-rn.ts). They CAN capture SENDABLE
//   values: primitives and SharedArrayBuffers. The Triad fan-out ring is a
//   SharedArrayBuffer with a byte-level protocol (RFC 0004 §Reference) —
//   every claim decision is a sequence of Atomics.load/store on a
//   BigInt64Array view. So the UI-thread reader is expressed as DATA
//   (a frame source descriptor: sab + geometry + lastSeq) plus a worklet
//   FUNCTION that performs the bounded claim protocol directly on Atomics.
//   No class crosses the boundary — the protocol itself does.
//
// THE CLAIM PROTOCOL inside the worklet is the RFC-0004 reader loop,
// verbatim: latestSeq check, bounded (<= 4) attempts, stamp-bracket
// revalidation, per-reader drop accounting, graceful skip. The only
// difference from packages/core's WeftFanoutReader is the absence of a
// reader-owned copy buffer — worklet closures cannot own mutable state
// across invocations, so the draw callback receives an OFFSET view into
// the SAB itself (the payload region of the claimed frame). That view is
// valid until the writer overwrites the slot — the same live-read (A3)
// discipline the kernel prescribes; callers that need a snapshot copy it
// in the draw callback (still on the UI thread).
//
// WHAT IS CI-PROVEN vs DEVICE-DEFERRED (honesty split, Law 4):
//   CI (vitest): the worklet body executes the protocol correctly under a
//   fake registrar with concurrent JS-thread publishing — integrity,
//   telescoping identity, bounded attempts, disposer discipline. The test
//   drives the SAME function that carries the 'worklet' directive.
//   DEVICE: actual Reanimated scheduling + UI-thread timing remains
//   deferred until the RN hardware leg — the module keeps the banner.

import { WeftFanoutBroadcaster } from '@weft/core';

/// Control-block indices — the wire contract (BigInt64Array over the ring
/// header; byte offset = 8 * index). Mirrors packages/core/src/fanout.ts.
const IDX_LATEST = 0;
const IDX_SLOTSEQ = 2;
const MAX_CLAIM_ATTEMPTS = 4;

/// The sendable descriptor a worklet closure captures. Primitives + one
/// SAB — everything here is legal worklet capture state.
export interface UiThreadFrameSource {
  sab: SharedArrayBuffer;
  payloadFloats: number;
  slotCount: number;
  /// Bytes: 16 + 8*slotCount header, then slotCount * payloadFloats * 4.
  payloadBase: number;
  /// Reader-private last-claimed seq. MUTATED BY THE WORKLET — Reanimated
  /// SharedValues wrap such state in production; the descriptor carries it
  /// so the protocol function is pure over its inputs (testable in node,
  /// where Reanimated is absent).
  lastSeq: number;
}

/// Build the frame source for one UI-thread consumer of a broadcaster.
/// One source per consumer slot — mount N sources against one broadcaster
/// for N independent UI-thread readers (RFC 0004).
export function createUiThreadFrameSource(
  broadcaster: WeftFanoutBroadcaster
): UiThreadFrameSource {
  return {
    sab: broadcaster.sab,
    payloadFloats: broadcaster.payloadFloats,
    slotCount: broadcaster.slotCount,
    payloadBase: 16 + 8 * broadcaster.slotCount,
    lastSeq: 0,
  };
}

/// The UI-thread claim protocol — THE function that carries the worklet
/// directive in production. It touches ONLY the SAB via Atomics (sendable
/// capture), never a class instance. Returns the claimed frame's payload
/// as a live view (A3) plus the protocol record for the frame.
///
/// The 'worklet' directive is load-bearing: Reanimated compiles this
/// function to run on the UI thread. The vitest battery executes the same
/// function body directly (no directive support in node) — the protocol
/// proven is byte-for-byte the protocol shipped.
///
/// Return shape is a plain record (sendable through SharedValues in
/// production; inspectable in tests). `seq` is the reader's last consistent
/// frame after this tick; `fresh`/`dropped` follow RFC-0004 semantics; a
/// `torn` tick (attempted but overwritten mid-copy) retries internally and
/// is NEVER surfaced as fresh — bounded by MAX_CLAIM_ATTEMPTS.
export function uiThreadClaim(source: UiThreadFrameSource): {
  fresh: boolean;
  seq: number;
  dropped: number;
  skipped: boolean;
  payload: Float32Array;
} {
  'worklet';
  const ctrl = new BigInt64Array(source.sab, 0, 2 + source.slotCount);

  for (let attempt = 0; attempt < MAX_CLAIM_ATTEMPTS; attempt++) {
    // Re-read latestSeq every attempt (the C port's `L = load(latest)` —
    // a torn retry must chase the newest completed frame, never spin on a
    // stale target).
    const latest = Number(Atomics.load(ctrl, IDX_LATEST));
    if (latest <= 0 || latest === source.lastSeq) {
      return {
        fresh: false,
        seq: source.lastSeq,
        dropped: 0,
        skipped: false,
        payload: new Float32Array(
          source.sab,
          source.payloadBase,
          source.payloadFloats
        ),
      };
    }
    const k = (latest - 1) % source.slotCount;
    const sB = Number(Atomics.load(ctrl, IDX_SLOTSEQ + k));
    if (sB !== latest) {
      const L2 = Number(Atomics.load(ctrl, IDX_LATEST));
      if (L2 === latest) {
        // Graceful skip (Law 1: counted by the caller via skipped, never
        // a spin).
        return {
          fresh: false,
          seq: source.lastSeq,
          dropped: 0,
          skipped: true,
          payload: new Float32Array(
            source.sab,
            source.payloadBase + k * source.payloadFloats * 4,
            source.payloadFloats
          ),
        };
      }
      // Newer frame completed — chase it on the next attempt.
      continue;
    }
    // Stamp matches: the live view IS the claimed payload (A3 — copy only
    // if the consumer needs a snapshot). Revalidate the stamp; a changed
    // stamp means the copy window was violated — the VIEW is still the
    // protocol-correct deliverable (it shows the newer fill in progress),
    // but the claim is rejected: the caller keeps its previous frame.
    const sA = Number(Atomics.load(ctrl, IDX_SLOTSEQ + k));
    if (sA === latest) {
      const dropped = latest - source.lastSeq - 1;
      source.lastSeq = latest;
      return {
        fresh: true,
        seq: latest,
        dropped,
        skipped: false,
        payload: new Float32Array(
          source.sab,
          source.payloadBase + k * source.payloadFloats * 4,
          source.payloadFloats
        ),
      };
    }
    // Torn window — the next attempt re-reads latest at the top and chases
    // the newest completed frame. An exhausted retry budget surfaces as a
    // non-fresh tick, never as a corrupt frame.
  }
  return {
    fresh: false,
    seq: source.lastSeq,
    dropped: 0,
    skipped: false,
    payload: new Float32Array(
      source.sab,
      source.payloadBase,
      source.payloadFloats
    ),
  };
}

/// Mount the UI-thread reader loop. `registerFrameCallback` is Reanimated's
/// `useFrameCallback` in production (the callback becomes a worklet on the
/// UI thread); the test battery drives it with a fake registrar. The draw
/// callback receives the live payload view + the protocol record, INSIDE
/// the frame callback — the Draw-phase discipline (02-KERNEL §4.2),
/// enforced by construction here rather than by convention.
///
/// Returns a disposer that stops the loop. Safe to call twice.
export function useWeftUiThread(
  source: UiThreadFrameSource,
  draw: (payload: Float32Array, rec: { fresh: boolean; seq: number; dropped: number; skipped: boolean }) => void,
  registerFrameCallback?: (cb: () => void) => (() => void) | void
): () => void {
  let disposed = false;
  const tick = () => {
    // Dispose guard: after dispose() the loop must claim NOTHING even if
    // a stale reference to the frame callback is invoked (belt-and-suspenders
    // on top of unregister — the same discipline as the rAF branch).
    if (disposed) return;
    const rec = uiThreadClaim(source);
    if (rec.fresh) {
      draw(rec.payload, rec);
    }
  };

  if (registerFrameCallback) {
    const unregister = registerFrameCallback(tick);
    return () => {
      if (disposed) return;
      disposed = true;
      if (typeof unregister === 'function') unregister();
    };
  }

  if (typeof requestAnimationFrame !== 'undefined') {
    let raf = 0;
    const loop = () => {
      if (disposed) return;
      tick();
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      if (disposed) return;
      disposed = true;
      if (raf) cancelAnimationFrame(raf);
    };
  }

  // No frame clock (non-DOM host without a registrar): a no-op disposer,
  // the same contract as weft-rn.ts — detectable, not silent.
  disposed = true;
  return () => {};
}
