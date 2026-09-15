// fanout.ts — RFC 0004: Multi-Consumer Fan-Out Heddles (driver layer)
//
// WHY EXISTS: ARCHITECTURE.md §Q2 (fan-out snapshot policy) is decided by
// [RFC 0004](rfcs/0004-fanout-heddles.md), ACCEPTED as a driver-layer pattern
// (round-6-adjudication §4): 1 writer, N readers, userland-only, zero kernel
// surface. The kernel Triad is 1:1 (single writer, single reader) by design;
// applications that need a primary canvas, a minimap, a flight recorder, and
// a network visualizer on one stream cannot bind N readers to one Triad. This
// module implements the RFC's seqlock ring on its own SharedArrayBuffer.
//
// PROTOCOL (implements the RFC §Reference-level specification):
//   Layout (one SAB, all offsets 8-byte aligned):
//     ctrl[0]         latestSeq — seq of the newest COMPLETED frame (i64, atomic)
//     ctrl[1]         publishes — telemetry, one atomic add per publish (i64)
//     ctrl[2..2+M)    slotSeq[k] — frame seq resident in slot k (i64, atomic);
//                     0 = INVALIDATED (a fill is in progress, or was abandoned)
//     payload region  M slots x payloadFloats x 4 bytes (Float32)
//   Frames are numbered from 1; latestSeq = 0 means "no frame yet".
//   Frame f lives in slot (f-1) mod M for its entire published life.
//
//   Writer (single, by contract — the same contract as the kernel's writer):
//     begin():   wSeq += 1; k = (wSeq-1) mod M;
//                slotSeq[k] <- 0      // invalidate BEFORE the fill
//                return cached Float32 view of slot k
//     publish(): slotSeq[k] <- wSeq; latestSeq <- wSeq; publishes += 1
//
//   The invalidate-before-fill step is the bracket the D-17 spike omitted:
//   without it, a reader mid-copy of frame wSeq-M would tear undetected while
//   the writer overwrites the same slot. Stamp-then-fill brackets the payload
//   exactly the way the kernel brackets its envelope->canary->exchange
//   (core/ts/weft.ts publish()).
//
//   Reader (N, independent; each owns its pre-allocated copy buffer — Law 2):
//     claim():   L = latestSeq
//                if L <= 0 or L == lastSeq: not fresh
//                else bounded (<= MAX_CLAIM_ATTEMPTS):
//                  k = (L-1) mod M; sB = slotSeq[k]
//                  if sB != L: the writer has begun overwriting slot k
//                    (stamp 0) or completed newer frames — re-read latestSeq;
//                    if unchanged, SKIP this tick (Law 1: no spin; a skipped
//                    tick is observable in stats, never silent); else chase
//                    the newer frame and retry
//                  copy slot k -> reader buffer; sA = slotSeq[k]
//                  if sA == L: consistent frame L — slot stamps are strictly
//                    monotonic per slot, so an unchanged stamp proves no
//                    overwrite began during the copy; report
//                    dropped = L - lastSeq - 1 and advance lastSeq
//                  else: torn copy detected; retry on the newest frame
//                attempts exhausted: not fresh, counted, never a spin
//
//   Memory model: same stance as the kernel — non-atomic Float32 payload
//   writes bracketed by seqcst BigInt64 stores. TS Atomics are seqcst-only,
//   stronger than the catalog's AcqRel — safe but noisier (06-PITFALLS §4).
//
//   Boundary of the claim (Law 4): fanout frames are NOT triad envelopes —
//   no magic, version, or payload_len; the slot stamp IS the frame id. Each
//   reader pays one Float32 copy per fresh claim: the price of N-reader
//   support at zero kernel surface (the kernel's 1:1 slot swap stays
//   zero-copy). A slow reader observes dropped frames (t_drop > 0) exactly as
//   the RFC's boundary section states. seq is surfaced as Number — exact for
//   values < 2^53; the i64 counter cannot reach 2^53 in any real session at
//   10^6 publishes/s (declared, not assumed).

/// Claim result record. MUTATED IN PLACE per claim — the record is
/// reader-owned and identity-stable, so the hot path allocates nothing
/// (Law 2). Read the fields synchronously after claim(); do not retain the
/// record across claims expecting a snapshot.
export interface FanoutClaim {
  fresh: boolean;
  seq: number;
  dropped: number;
}

/// Advisory reader statistics (cold path — allocates; AXIOM T applies:
/// advisory, never a correctness reference).
export interface FanoutReaderStats {
  reads: number;
  fresh: number;
  drops: number;
  /// Ticks skipped because the targeted slot was mid-overwrite and no newer
  /// frame had completed — the graceful-skip path, bounded by construction.
  skippedMidOverwrite: number;
  /// Claims that exhausted the bounded retry. The reader keeps its last
  /// consistent frame rather than spinning (Law 1).
  tornExhausted: number;
}

/// Advisory broadcaster state (cold path — allocates).
export interface FanoutDebugStats {
  latestSeq: bigint;
  publishes: bigint;
  slotCount: number;
  payloadFloats: number;
  slotStamps: bigint[];
}

/// Control-block indices (BigInt64Array slots).
const IDX_LATEST = 0;
const IDX_PUBLISHES = 1;
const IDX_SLOTSEQ = 2; // + k

/// Bounded claim attempts. A retry only happens when a NEWER frame completed
/// during the claim; the newer frame's own slot is self-consistent, so
/// convergence is immediate. 4 is generous, not tuned.
const MAX_CLAIM_ATTEMPTS = 4;

/// Control-block length in i64 slots.
function ctrlLength(slotCount: number): number { return 2 + slotCount; }

/// Byte offset of the payload region (after the control block).
function payloadBase(slotCount: number): number { return 16 + 8 * slotCount; }

// ---------------------------------------------------------------------------
// Broadcaster — the writer side. One per stream. Single writer by contract.
// ---------------------------------------------------------------------------

export class WeftFanoutBroadcaster {
  /// The single SharedArrayBuffer. Post it (structured clone shares SABs) to
  /// the thread that owns the writer, or to reader threads; construct
  /// WeftFanoutReader directly from it there.
  sab: SharedArrayBuffer;
  /// BigInt64 view of the control block (latest, publishes, slot stamps).
  ctrl: BigInt64Array;
  /// Payload capacity per slot, in floats (immutable after init).
  payloadFloats: number;
  /// Ring depth (immutable after init). RFC 0004 recommends 4-8.
  slotCount: number;

  /// Writer-private frame counter (plain number — single writer by
  /// contract, the same discipline as the kernel's thread-private w_work).
  private wSeq: number;
  /// Cached per-slot payload views, allocated ONCE at construction so
  /// begin() never allocates (Law 2 — the same discipline as the kernel's
  /// wViews / wF32Views arrays).
  private slotViews: Float32Array[];

  /// Allocate a fan-out ring. `payloadFloats` = per-slot payload capacity in
  /// floats; `slotCount` = ring depth (default 4, RFC 0004 §Reference).
  constructor(payloadFloats: number, slotCount: number = 4) {
    if (!Number.isInteger(payloadFloats) || payloadFloats < 1) {
      throw new Error(`payloadFloats must be an integer >= 1 (got ${payloadFloats})`);
    }
    if (!Number.isInteger(slotCount) || slotCount < 2) {
      throw new Error(`slotCount must be an integer >= 2 (got ${slotCount})`);
    }
    this.payloadFloats = payloadFloats;
    this.slotCount = slotCount;
    const bytes = payloadBase(slotCount) + slotCount * payloadFloats * 4;
    this.sab = new SharedArrayBuffer(bytes);
    this.ctrl = new BigInt64Array(this.sab, 0, ctrlLength(slotCount));
    // SAB is zero-initialized, so: latestSeq = 0 (no frame yet), publishes =
    // 0, every slotSeq = 0 (nothing resident / all invalidated). Stated here
    // for review; the reader treats latestSeq <= 0 as "no frame".
    this.wSeq = 0;
    this.slotViews = [];
    const base = payloadBase(slotCount);
    for (let k = 0; k < slotCount; k++) {
      this.slotViews.push(
        new Float32Array(this.sab, base + k * payloadFloats * 4, payloadFloats)
      );
    }
  }

  /// Live write cursor for the NEXT frame: a cached Float32Array view over
  /// slot (wSeq mod M). The slot's stamp is invalidated BEFORE the view is
  /// returned, so any reader targeting an older frame in this slot detects
  /// the overwrite (see the protocol note in the file header). The view stays
  /// valid until the next begin(). Zero allocation per call.
  begin(): Float32Array {
    this.wSeq += 1;
    const k = (this.wSeq - 1) % this.slotCount;
    Atomics.store(this.ctrl, IDX_SLOTSEQ + k, 0n);
    return this.slotViews[k];
  }

  /// Publish the begun frame: stamp the slot, then flip latestSeq (both
  /// seqcst). The payload fill happened between begin() and here, bracketed
  /// by the two stamps. Returns the published frame seq, or 0 if no begin()
  /// preceded (nothing is published — a detectable no-op, not an error).
  publish(): number {
    if (this.wSeq === 0) return 0;
    const k = (this.wSeq - 1) % this.slotCount;
    Atomics.store(this.ctrl, IDX_SLOTSEQ + k, BigInt(this.wSeq));
    Atomics.store(this.ctrl, IDX_LATEST, BigInt(this.wSeq));
    Atomics.add(this.ctrl, IDX_PUBLISHES, 1n);
    return this.wSeq;
  }

  /// Create a reader bound to this ring (same process). Reader threads
  /// elsewhere construct WeftFanoutReader directly from `this.sab` after
  /// posting it — geometry travels with the constructor arguments, the same
  /// way the kernel's payloadMax does.
  createReader(): WeftFanoutReader {
    return new WeftFanoutReader(this.sab, this.payloadFloats, this.slotCount);
  }

  /// Advisory state snapshot (cold path — allocates; never call per frame).
  debugStats(): FanoutDebugStats {
    const stamps: bigint[] = [];
    for (let k = 0; k < this.slotCount; k++) {
      stamps.push(Atomics.load(this.ctrl, IDX_SLOTSEQ + k));
    }
    return {
      latestSeq: Atomics.load(this.ctrl, IDX_LATEST),
      publishes: Atomics.load(this.ctrl, IDX_PUBLISHES),
      slotCount: this.slotCount,
      payloadFloats: this.payloadFloats,
      slotStamps: stamps,
    };
  }
}

// ---------------------------------------------------------------------------
// Reader — the consumer side. N per ring, each fully independent.
// ---------------------------------------------------------------------------

export class WeftFanoutReader {
  sab: SharedArrayBuffer;
  /// BigInt64 view of the control block (shared with the broadcaster).
  ctrl: BigInt64Array;
  payloadFloats: number;
  slotCount: number;

  /// The reader's own pre-allocated copy buffer — "each reader polls its own
  /// pre-allocated buffer" (RFC 0004 §Guide). Stable identity for the
  /// consumer's lifetime; holds frame data only after a fresh claim.
  private target: Float32Array;
  /// Cached per-slot views, used to copy INTO target with zero allocation
  /// per claim (Law 2).
  private slotViews: Float32Array[];
  /// Last frame seq this reader has held consistent (0 = none yet).
  private lastSeq: number;
  /// Preallocated, identity-stable claim record (mutated per claim).
  private rec: FanoutClaim;

  // Reader-private statistics (advisory; exposed via stats()).
  private nReads: number;
  private nFresh: number;
  private nDrops: number;
  private nSkip: number;
  private nExhausted: number;

  /// Attach a reader to a fan-out ring's SharedArrayBuffer. Works in any
  /// thread that received the SAB; geometry (payloadFloats, slotCount) is
  /// validated against the buffer length, so a mismatched pair fails fast
  /// instead of tearing.
  constructor(sab: SharedArrayBuffer, payloadFloats: number, slotCount: number = 4) {
    if (!Number.isInteger(payloadFloats) || payloadFloats < 1) {
      throw new Error(`payloadFloats must be an integer >= 1 (got ${payloadFloats})`);
    }
    if (!Number.isInteger(slotCount) || slotCount < 2) {
      throw new Error(`slotCount must be an integer >= 2 (got ${slotCount})`);
    }
    const expectBytes = payloadBase(slotCount) + slotCount * payloadFloats * 4;
    if (sab.byteLength !== expectBytes) {
      throw new Error(
        `SAB geometry mismatch: expected ${expectBytes} bytes for ` +
        `${slotCount} slots x ${payloadFloats} floats, got ${sab.byteLength}`
      );
    }
    this.sab = sab;
    this.ctrl = new BigInt64Array(sab, 0, ctrlLength(slotCount));
    this.payloadFloats = payloadFloats;
    this.slotCount = slotCount;

    this.target = new Float32Array(payloadFloats);
    this.slotViews = [];
    const base = payloadBase(slotCount);
    for (let k = 0; k < slotCount; k++) {
      this.slotViews.push(new Float32Array(sab, base + k * payloadFloats * 4, payloadFloats));
    }
    this.lastSeq = 0;
    this.rec = { fresh: false, seq: 0, dropped: 0 };
    this.nReads = 0;
    this.nFresh = 0;
    this.nDrops = 0;
    this.nSkip = 0;
    this.nExhausted = 0;
  }

  /// Claim the freshest completed frame into this reader's buffer. Never
  /// blocks, never spins unboundedly, never fails: a tick on which no
  /// consistent newer frame is available returns fresh=false and the reader
  /// keeps its last consistent frame. Returns the reader-owned claim record
  /// (identity-stable, mutated in place — zero allocation per claim).
  ///
  /// `dropped` counts frames that completed without this reader ever
  /// observing them (RFC 0004 §Reference: per-reader drop accounting). The
  /// telescoping identity sum(dropped) = lastSeq - freshClaims holds exactly.
  claim(): FanoutClaim {
    const rec = this.rec;
    this.nReads++;
    let L = Atomics.load(this.ctrl, IDX_LATEST);
    if (L <= 0n || L === BigInt(this.lastSeq)) {
      rec.fresh = false;
      rec.seq = this.lastSeq;
      rec.dropped = 0;
      return rec;
    }
    for (let attempt = 0; attempt < MAX_CLAIM_ATTEMPTS; attempt++) {
      const k = Number((L - 1n) % BigInt(this.slotCount));
      const sB = Atomics.load(this.ctrl, IDX_SLOTSEQ + k);
      if (sB !== L) {
        // Slot mid-overwrite (stamp 0) or already re-stamped by a newer
        // frame. Re-read latestSeq: unchanged means the writer is mid-fill
        // on our slot — skip the tick; changed means a newer frame
        // completed — chase it.
        const L2 = Atomics.load(this.ctrl, IDX_LATEST);
        if (L2 === L) {
          this.nSkip++;
          rec.fresh = false;
          rec.seq = this.lastSeq;
          rec.dropped = 0;
          return rec;
        }
        L = L2;
        continue;
      }
      // Stamp matches frame L: copy, then re-validate the stamp.
      this.target.set(this.slotViews[k]);
      const sA = Atomics.load(this.ctrl, IDX_SLOTSEQ + k);
      if (sA === L) {
        // Consistent frame L. Slot stamps are strictly monotonic per slot
        // (each overwrite re-stamps with a higher frame seq), so an
        // unchanged stamp proves no overwrite began during the copy.
        const dropped = Number(L) - this.lastSeq - 1;
        this.nDrops += dropped;
        this.lastSeq = Number(L);
        this.nFresh++;
        rec.fresh = true;
        rec.seq = this.lastSeq;
        rec.dropped = dropped;
        return rec;
      }
      // Torn copy detected (an overwrite began mid-copy). Retry on the
      // newest completed frame.
      L = Atomics.load(this.ctrl, IDX_LATEST);
    }
    // Bounded retries exhausted: keep the last consistent frame. Counted,
    // never silent, never a spin (Law 1).
    this.nExhausted++;
    rec.fresh = false;
    rec.seq = this.lastSeq;
    rec.dropped = 0;
    return rec;
  }

  /// The reader's pre-allocated copy buffer (Float32Array, stable identity).
  /// Meaningful after a fresh claim(); overwritten by the next fresh claim —
  /// read it live in the Draw phase, the same discipline as rReadSlice (A3).
  view(): Float32Array {
    return this.target;
  }

  /// Advisory statistics snapshot (cold path — allocates; AXIOM T: advisory,
  /// never a correctness reference).
  stats(): FanoutReaderStats {
    return {
      reads: this.nReads,
      fresh: this.nFresh,
      drops: this.nDrops,
      skippedMidOverwrite: this.nSkip,
      tornExhausted: this.nExhausted,
    };
  }
}
