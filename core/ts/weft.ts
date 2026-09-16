// weft.ts — Corrected Triad Protocol kernel (TypeScript reference)
//
// Normative: per 02-KERNEL.md and 03-ENVELOPE.md. Direct port of core/c/weft.{h,c}
// and core/rust/src/lib.rs. Same protocol, same envelope, same I6 handshake,
// same memory-ordering matrix (see litmus/catalog.yaml).
//
// Substrate: Node 24 with SharedArrayBuffer (verified — preflight), Atomics.exchange,
// worker_threads. Per 06-PITFALLS §4:
//   - All DataView accessors use littleEndian=true.
//   - u32 arithmetic: Math.imul for multiplies, >>> 0 after shifts/xors.
//   - Atomics.exchange operates on Int32Array (latest lives in the Int32 control block).
//   - One SharedArrayBuffer per Weft sized: controlBlock(64B) + 3 × buf_size.
//   - The kernel has NO worker_threads dependency — workers belong to the runner.

/// Magic "WEFT" little-endian: bytes 57 45 46 54 → u32 LE = 0x54464557.
export const WEFT_MAGIC = 0x54464557;
/// Triad protocol version 1.
export const WEFT_VERSION_1 = 1;

/// Publish result (02 §4). Plain const object (TS enums not supported in strip-only mode).
export const PubResult = { Ok: 0, DroppedRevoked: 1 } as const;
export type PubResult = (typeof PubResult)[keyof typeof PubResult];

/// Decode result (03-ENVELOPE §2). Plain const object.
export const DecodeResult = { Ok: 0, Short: 1, BadMagic: 2, BadHeader: 3 } as const;
export type DecodeResult = (typeof DecodeResult)[keyof typeof DecodeResult];

/// Per-buffer debug sample (port parity: weft_debug_buf_t, core/c/weft.h).
/// `owner`: 0=free, 1=writer, 2=reader, 3=in-exchange(latest).
export interface WeftDebugBuf {
  slotIdx: number;
  seq: number;
  version: number;
  headerSize: number;
  payloadLen: number;
  owner: number;
}

/// Advisory kernel state sample (port parity: weft_debug_view_t, core/c/weft.h).
/// Per AXIOM T (05-CONTRACTS): telemetry values are advisory, never a
/// correctness reference. wWork/rWork are thread-private; reading them from
/// another thread is advisory-only sampling. This is a set of
/// individually-consistent samples, NOT a consistent snapshot. Cold path —
/// allocates its result object; never call from the hot loop (Law 2).
/// Deviation from C, stated: C reports two live samples and never
/// dereferences the third (I6 freed-buffer hazard); TS has no free() — the
/// SAB is GC-managed — so all three slots are sampled.
export interface WeftDebugView {
  latest: number;
  wWork: number;
  rWork: number;
  revoked: boolean;
  epoch: number;
  tPublish: bigint;
  tClaim: bigint;
  tDrop: bigint;
  bufs: WeftDebugBuf[];
  midPublishSample: boolean;
}

/// Compute buf_size = align64(16 + payload_max + 8, 64).
function computeBufSize(payloadMax: number): number {
  const raw = 16 + payloadMax + 8;
  return (raw + 63) & ~63;
}

/// Buffer layout per buffer:
///   [0..16) envelope
///   [16..16+payload_max) payload
///   [buf_size-8..buf_size) canary (u64 LE, value = seq)
///
/// The Weft owns 3 buffers + the single shared atomic `latest` + writer-private
/// `w_work` + reader-private `r_work` + I6 state + telemetry.
///
/// Memory layout in the SharedArrayBuffer:
///   [0..64) control block (Int32Array of 16 — we use slots 0..6)
///     slot 0: latest (Int32)
///     slot 1: w_work (Int32, writer-private)
///     slot 2: r_work (Int32, reader-private)
///     slot 3: revoked (Int32 — 0/1 for false/true)
///     slot 4: epoch (Int32)
///     slot 5: t_publish (BigInt64? No — use two Int32 slots: lo, hi)
///     slots 5-6: t_publish (lo, hi — BigInt64 via DataView)
///     slots 7-8: t_claim
///     slots 9-10: t_drop
///     slots 11-12: t_wsteps
///     slots 13-14: t_rsteps
///   [64..64+3*buf_size) three buffers, each `buf_size` bytes
///
/// We use BigInt64Array views for the u64 counters via DataView on the SAB
/// directly (since BigInt64Array requires 8-byte alignment, which 64-byte
/// control block guarantees).
export class Weft {
  /// The single SharedArrayBuffer.
  sab: SharedArrayBuffer;
  /// Int32 view of the control block (slots 0..15 = bytes 0..63).
  ctrl: Int32Array;
  /// BigInt64 view of the control block (for u64 counters, slots 0..7 of u64s = bytes 0..63).
  ctrlU64: BigInt64Array;
  /// DataView over the SAB (for envelope encode/decode with littleEndian=true).
  dv: DataView;
  /// Byte offset of buffer 0 in the SAB (= 64).
  buf0Offset: number;
  /// Byte size of one buffer.
  bufSize: number;
  /// Payload capacity in bytes (immutable after init).
  payloadMax: number;

  /// Control block slot indices (Int32 slots, 4 bytes each).
  static readonly SLOT_LATEST = 0;
  static readonly SLOT_W_WORK = 1;
  static readonly SLOT_R_WORK = 2;
  static readonly SLOT_REVOKED = 3;
  static readonly SLOT_EPOCH = 4;
  // u64 slots (BigInt64Array indices, 8 bytes each, starting at byte 40 → u64 index 5)
  static readonly SLOT64_T_PUBLISH = 5;  // bytes 40-47
  static readonly SLOT64_T_CLAIM = 6;    // bytes 48-55
  static readonly SLOT64_T_DROP = 7;     // bytes 56-63

  // Note: t_wsteps and t_rsteps are stored in the same u64 array further along
  // (would need slots 8-9 and 10-11). For Phase 0 we co-locate them in the
  // control block by extending it to 128 bytes if needed. For simplicity,
  // we put t_wsteps and t_rsteps as plain JS numbers (single-threaded access
  // by contract — wsteps by writer, rsteps by reader — and they're only
  // read by the litmus runner, not synchronized).
  t_wsteps: bigint = 0n;
  t_rsteps: bigint = 0n;

  /// Writer-cursor views, one per buffer slot, allocated ONCE at construction
  /// (Law 2: zero allocation per call — wBegin just indexes this array).
  /// Port parity with weft_w_begin's stable raw pointer (core/c/weft.h).
  private wViews: Uint8Array[];
  /// Typed writer-cursor views (Float32 over the payload region), one per
  /// slot, allocated ONCE. Byte offset is 4-byte aligned by construction
  /// (buf0Offset=64, bufSize is 64-aligned, +16 payload start).
  private wF32Views: Float32Array[];
  /// Reader-side payload views, one per buffer slot, allocated ONCE at
  /// construction — the mirror of wViews for the draw-phase read path
  /// (Law 2 applies to the reader too). rLive()/rLiveFloat32() index these
  /// arrays; rReadSlice(16, payloadMax) resolves to the same cached view.
  private rViews: Uint8Array[];
  /// Typed reader views (Float32 over the payload region), one per slot.
  private rF32Views: Float32Array[];

  /// Allocate a Weft with the given payload_max.
  constructor(payloadMax: number) {
    this.payloadMax = payloadMax;
    this.bufSize = computeBufSize(payloadMax);
    const ctrlSize = 64;  // 64-byte control block
    const totalSize = ctrlSize + 3 * this.bufSize;
    this.sab = new SharedArrayBuffer(totalSize);
    this.ctrl = new Int32Array(this.sab, 0, 16);  // 16 Int32 slots = 64 bytes
    this.ctrlU64 = new BigInt64Array(this.sab, 0, 8);  // 8 BigInt64 slots = 64 bytes
    this.dv = new DataView(this.sab);
    this.buf0Offset = ctrlSize;

    // Zero everything (SAB is zero-initialized, but be explicit).
    this.ctrl.fill(0);
    this.ctrlU64.fill(0n);

    // Initialize indices per 02 §1: latest=0, w_work=1, r_work=2.
    Atomics.store(this.ctrl, Weft.SLOT_LATEST, 0);
    Atomics.store(this.ctrl, Weft.SLOT_W_WORK, 1);
    Atomics.store(this.ctrl, Weft.SLOT_R_WORK, 2);
    Atomics.store(this.ctrl, Weft.SLOT_REVOKED, 0);  // false
    Atomics.store(this.ctrl, Weft.SLOT_EPOCH, 0);

    // Initialize all 3 buffers with null envelopes (seq=0) and pat(0, i) payload.
    // Per 04-LITMUS §0.6 (v1.1, normative null-frame fixture invariant): the null
    // frame is a valid initial state, not a sentinel that breaks verification.
    // A kernel that zero-fills the null frame makes L6 false-red.
    for (let i = 0; i < 3; i++) {
      const bufOffset = this.buf0Offset + i * this.bufSize;
      this.envelopeEncodeV1(bufOffset, 0, payloadMax);
      for (let j = 0; j < payloadMax; j++) {
        this.dv.setUint8(bufOffset + 16 + j, pat(0, j));
      }
      // Canary at buf_size-8 = 0 (matches seq=0). SAB is already zeroed.
    }

    // Writer-cursor views: created once, here, so that wBegin()/wBeginFloat32()
    // never allocate (Law 2 — zero is a contract on the writer path too).
    // The litmus fillPayload() path is untouched and remains normative.
    this.wViews = [];
    this.wF32Views = [];
    this.rViews = [];
    this.rF32Views = [];
    for (let i = 0; i < 3; i++) {
      const payloadOff = this.buf0Offset + i * this.bufSize + 16;
      this.wViews.push(new Uint8Array(this.sab, payloadOff, payloadMax));
      this.wF32Views.push(new Float32Array(this.sab, payloadOff, Math.floor(payloadMax / 4)));
      // Reader views cover the same payload region (the region after claim()
      // is exclusively the reader's until its next claim — RFC-0001 §4.3).
      this.rViews.push(new Uint8Array(this.sab, payloadOff, payloadMax));
      this.rF32Views.push(new Float32Array(this.sab, payloadOff, Math.floor(payloadMax / 4)));
    }
  }

  // ---------------------------------------------------------------------------
  // Buffer access helpers
  // ---------------------------------------------------------------------------

  /// Byte offset of buffer `i` in the SAB.
  bufOffset(i: number): number { return this.buf0Offset + i * this.bufSize; }

  // ---------------------------------------------------------------------------
  // Writer
  // ---------------------------------------------------------------------------

  /// Fill the writer's working buffer with pat(seq, i) payload.
  fillPayload(seq: number, payloadLen: number): void {
    const w = Atomics.load(this.ctrl, Weft.SLOT_W_WORK);
    const off = this.bufOffset(w) + 16;
    for (let i = 0; i < payloadLen; i++) {
      this.dv.setUint8(off + i, pat(seq, i));
    }
  }

  // ---------------------------------------------------------------------------
  // Writer cursor (port parity: weft_w_begin / weft_w_write_payload,
  // core/c/weft.h §Writer). The TS port originally omitted these; a real
  // writer had no public way to place user payload into the working buffer
  // and was forced to reach into kernel internals (ctrl/bufOffset) — a
  // Law 3 encapsulation breach demonstrated by demos/web ModeCRunner.
  // ---------------------------------------------------------------------------

  /// Live write cursor: a Uint8Array VIEW over the writer's working payload
  /// region buf[w_work][16..16+payload_max). Writes here are visible to the
  /// reader after the next publish(). NOT a snapshot — the live buffer
  /// (A3, same rule as the reader side). Zero allocation per call: the view
  /// is cached per buffer slot at construction. The view stays valid until
  /// the next publish() rotates w_work.
  wBegin(): Uint8Array {
    const w = Atomics.load(this.ctrl, Weft.SLOT_W_WORK);
    return this.wViews[w];
  }

  /// Typed write cursor: Float32Array view over the same payload region.
  /// Element count = floor(payload_max / 4). Same liveness and zero-alloc
  /// contract as wBegin(). Ergonomic typed access without per-frame view
  /// allocation (Law 2) — the TS analog of casting weft_w_begin's raw pointer.
  wBeginFloat32(): Float32Array {
    const w = Atomics.load(this.ctrl, Weft.SLOT_W_WORK);
    return this.wF32Views[w];
  }

  /// Write `src.length` bytes of payload into the writer's working buffer at
  /// offset 16. Returns 0 on success, -1 if src.length > payload_max (nothing
  /// written). Mirrors weft_w_write_payload's contract exactly. This is a
  /// convenience bulk copy; the cursor (wBegin) exists for zero-copy fills.
  wWritePayload(src: Uint8Array): number {
    if (src.length > this.payloadMax) return -1;
    this.wBegin().set(src);
    return 0;
  }

  /// Publish the writer's working buffer with the given seq and payload_len.
  /// Per 02 §2 + §6:
  ///   1. if revoked.load(Relaxed): epoch.fetch_add(1, AcqRel); t_drop++;
  ///      return DROPPED_REVOKED  (checked FIRST, before any byte write)
  ///   2. write envelope (v1, seq, payload_len) into buf[w_work]
  ///   3. write canary = seq at buf[w_work].tail
  ///   4. old = latest.exchange(w_work, AcqRel)   // THE atomic
  ///   5. w_work = old
  ///   6. t_publish++; t_wsteps++; return Ok
  publish(seq: number, payloadLen: number): PubResult {
    // §6 step 1: revoked checked FIRST.
    // Note: TS Atomics are sequentially consistent (the only choice). The catalog
    // ordering matrix says "Relaxed" for the writer's revoked load; in TS this
    // is implemented as Atomics.load (seqcst), which is stronger — safe but
    // noisier than C/Rust's relaxed. Documented per 06 §4.
    if (Atomics.load(this.ctrl, Weft.SLOT_REVOKED) !== 0) {
      // ACK: epoch.fetch_add(1, AcqRel). In TS, Atomics.add is seqcst (stronger).
      Atomics.add(this.ctrl, Weft.SLOT_EPOCH, 1);
      Atomics.add(this.ctrlU64, Weft.SLOT64_T_DROP, 1n);
      return PubResult.DroppedRevoked;
    }

    const w = Atomics.load(this.ctrl, Weft.SLOT_W_WORK);
    const bufOff = this.bufOffset(w);

    // Write envelope (v1, seq, payload_len) into buf[w_work].
    this.envelopeEncodeV1(bufOff, seq, payloadLen);

    // Write canary = seq at buf[w_work].tail (u64 LE at buf_size-8).
    this.dv.setBigUint64(bufOff + this.bufSize - 8, BigInt(seq), true);

    // THE atomic: publish + take old latest.
    // Atomics.exchange is seqcst in TS (the only choice). The catalog says AcqRel;
    // seqcst is stronger — safe but noisier.
    const old = Atomics.exchange(this.ctrl, Weft.SLOT_LATEST, w);

    // w_work = old (writer-private; Relaxed in C/Rust, plain store in TS is fine
    // because TS Atomics.store is seqcst, but the field is thread-private by contract).
    Atomics.store(this.ctrl, Weft.SLOT_W_WORK, old);

    // Telemetry.
    Atomics.add(this.ctrlU64, Weft.SLOT64_T_PUBLISH, 1n);
    this.t_wsteps += 1n;

    return PubResult.Ok;
  }

  // ---------------------------------------------------------------------------
  // Reader
  // ---------------------------------------------------------------------------

  /// Claim the freshest published buffer. NEVER fails.
  claim(): number {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    const mine = Atomics.exchange(this.ctrl, Weft.SLOT_LATEST, r);
    Atomics.store(this.ctrl, Weft.SLOT_R_WORK, mine);
    Atomics.add(this.ctrlU64, Weft.SLOT64_T_CLAIM, 1n);
    this.t_rsteps += 1n;
    return mine;
  }

  /// Read envelope seq of the reader's held buffer (live).
  rSeq(): number {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    return this.dv.getUint32(this.bufOffset(r) + 8, true);
  }

  /// Read envelope magic of the reader's held buffer (live).
  rMagic(): number {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    return this.dv.getUint32(this.bufOffset(r), true);
  }

  /// Read envelope payload_len of the reader's held buffer (live).
  rPayloadLen(): number {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    return this.dv.getUint32(this.bufOffset(r) + 12, true);
  }

  /// Read envelope header_size of the reader's held buffer (live).
  /// Port parity: weft_r_header_size (core/c/weft.h §Reader).
  rHeaderSize(): number {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    return this.dv.getUint16(this.bufOffset(r) + 6, true);
  }

  /// Read `len` bytes from the held buffer at `offset`. Returns a Uint8Array
  /// VIEW (not a copy) — A3: the reader must observe the LIVE buffer.
  /// The view is valid until the next claim().
  ///
  /// CONTRACT (C parity, normative — weft_r_read_slice): the window is
  /// bounded by buf_size - offset and MAY include envelope/canary bytes
  /// (offset is an absolute buffer offset, 0 = envelope start). Allocates a
  /// fresh view object per call (never a payload copy).
  ///
  /// HOT PATH (Law 2): draw-phase bindings must call rLive() / rLiveFloat32()
  /// instead — those return the payload views cached at construction:
  /// zero allocation per frame. rReadSlice stays C-parity for tools and
  /// verify-in-place code (L1) that needs the envelope in the window.
  rReadSlice(offset: number, len: number): Uint8Array {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    const bufOff = this.bufOffset(r);
    if (offset >= this.bufSize) return new Uint8Array(0);
    const n = Math.min(len, this.bufSize - offset);
    return new Uint8Array(this.sab, bufOff + offset, n);
  }

  /// Live payload view of the READER-HELD buffer (r_work). Zero-copy, in
  /// place, zero allocation (cached per slot at construction). This is the
  /// API draw-phase bindings MUST use after claim() — the TS analog of
  /// C weft_r_live_ptr / Swift rLivePtr / Kotlin rLiveBuf. The view is
  /// exclusively the reader's until its next claim() (RFC-0001 §4.3).
  /// Port-parity note: like the Kotlin slice, absolute index 0 = payload
  /// start (the envelope is NOT included; use rSeq()/rMagic() for it).
  rLive(): Uint8Array {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    return this.rViews[r];
  }

  /// Typed live payload view (Float32Array) of the reader-held buffer.
  /// Element count = floor(payload_max / 4). Same contract as rLive().
  rLiveFloat32(): Float32Array {
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    return this.rF32Views[r];
  }

  /// Verify the held buffer in-place (A3 — read live, not a snapshot).
  verifyHeld(expectedSeq: number, payloadLen: number): boolean {
    if (this.rMagic() !== WEFT_MAGIC) return false;
    if (this.rSeq() !== expectedSeq) return false;
    const payload = this.rReadSlice(16, payloadLen);
    for (let i = 0; i < payloadLen; i++) {
      if (payload[i] !== pat(expectedSeq, i)) return false;
    }
    const r = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);
    const cv = this.dv.getBigUint64(this.bufOffset(r) + this.bufSize - 8, true);
    if (cv !== BigInt(expectedSeq)) return false;
    return true;
  }

  // ---------------------------------------------------------------------------
  // I6 — writer revocation handshake (02 §6, A1)
  // ---------------------------------------------------------------------------

  /// Step 1: revoke the writer. Sets revoked.store(true, Release).
  /// In TS, Atomics.store is seqcst (stronger than Release — safe but noisier).
  revoke(): void {
    Atomics.store(this.ctrl, Weft.SLOT_REVOKED, 1);
  }

  /// Steps 2-3: poll epoch until it advances past `preRevokeEpoch`, bounded by `timeoutMs`.
  /// Returns true on ACK received, false on timeout.
  reclaim(preRevokeEpoch: number, timeoutMs: number): boolean {
    const start = Date.now();
    while (true) {
      const e = Atomics.load(this.ctrl, Weft.SLOT_EPOCH);
      if (e !== preRevokeEpoch) return true;
      if (Date.now() - start >= timeoutMs) return false;
      // Yield the core where the platform allows: Atomics.wait parks the
      // thread (workers) — it THROWS on the main thread, where we fall back
      // to a bounded busy-loop. 1 ms parks approximate the C kernel's
      // 100 µs nanosleep pacing closely enough for a teardown path.
      try {
        Atomics.wait(this.ctrl, Weft.SLOT_EPOCH, e, 1);
      } catch {
        // Main thread: no park available; bounded busy-loop.
      }
    }
  }

  /// Poison all 3 buffers with 0xDE (for L7).
  poisonAll(): void {
    const poison = new Uint8Array([0xDE]);
    for (let i = 0; i < 3; i++) {
      const off = this.bufOffset(i);
      for (let j = 0; j < this.bufSize; j++) {
        this.dv.setUint8(off + j, 0xDE);
      }
    }
  }

  /// Scan all 3 buffers byte-wise; return true if all bytes are 0xDE.
  scanPoison(): boolean {
    for (let i = 0; i < 3; i++) {
      const off = this.bufOffset(i);
      for (let j = 0; j < this.bufSize; j++) {
        if (this.dv.getUint8(off + j) !== 0xDE) return false;
      }
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // Telemetry
  // ---------------------------------------------------------------------------

  tPublish(): bigint { return Atomics.load(this.ctrlU64, Weft.SLOT64_T_PUBLISH); }
  tClaim(): bigint { return Atomics.load(this.ctrlU64, Weft.SLOT64_T_CLAIM); }
  tDrop(): bigint { return Atomics.load(this.ctrlU64, Weft.SLOT64_T_DROP); }
  epoch(): number { return Atomics.load(this.ctrl, Weft.SLOT_EPOCH); }

  // ---------------------------------------------------------------------------
  // Debug view (port parity: weft_debug_view, core/c/weft.h — the sanctioned
  // read-only, wait-free inspection accessor). Advisory per AXIOM T; cold
  // path (allocates its result). Never a correctness reference.
  // ---------------------------------------------------------------------------

  /// Read-only advisory snapshot of kernel state. In TS the (latest, w_work,
  /// r_work) triple is always a permutation of (0,1,2) — every slot has an
  /// owner — and the SAB is GC-managed (no freed-buffer dereference hazard,
  /// unlike C's post-reclaim state), so all three slots are sampled.
  /// midPublishSample=true when a header looks sampled mid-write (seq != 0
  /// with a wrong magic) — same detection rule as the C kernel.
  debugView(): WeftDebugView {
    const latest = Atomics.load(this.ctrl, Weft.SLOT_LATEST);      // advisory (Relaxed in C)
    const wWork = Atomics.load(this.ctrl, Weft.SLOT_W_WORK);      // thread-private; advisory
    const rWork = Atomics.load(this.ctrl, Weft.SLOT_R_WORK);      // thread-private; advisory
    const revoked = Atomics.load(this.ctrl, Weft.SLOT_REVOKED) !== 0;
    const epoch = Atomics.load(this.ctrl, Weft.SLOT_EPOCH);       // Acquire in C

    const bufs: WeftDebugBuf[] = [];
    let midPublishSample = false;
    for (let i = 0; i < 3; i++) {
      const off = this.bufOffset(i);
      const seq = this.dv.getUint32(off + 8, true);
      const magic = this.dv.getUint32(off, true);
      if (seq !== 0 && magic !== WEFT_MAGIC) midPublishSample = true;
      bufs.push({
        slotIdx: i,
        seq,
        version: this.dv.getUint16(off + 4, true),
        headerSize: this.dv.getUint16(off + 6, true),
        payloadLen: this.dv.getUint32(off + 12, true),
        owner: i === wWork ? 1 : (i === rWork ? 2 : 3),
      });
    }

    return {
      latest,
      wWork,
      rWork,
      revoked,
      epoch,
      tPublish: this.tPublish(),
      tClaim: this.tClaim(),
      tDrop: this.tDrop(),
      bufs,
      midPublishSample,
    };
  }

  // ---------------------------------------------------------------------------
  // Envelope instance methods (03-ENVELOPE) — thin wrappers over the pure
  // functions below, so callers (and the constructor) never need DataView
  // plumbing. Formerly grafted on via prototype patching + module
  // augmentation; now ordinary methods (one source of truth, and the
  // packages/core mirror of this file is verbatim-identical again).
  // ---------------------------------------------------------------------------

  /// Encode an envelope at absolute SAB offset `dstOff` (constructor + tools).
  envelopeEncodeV1(dstOff: number, seq: number, payloadLen: number): void {
    envelopeEncodeV1(this.dv, dstOff, seq, payloadLen);
  }

  /// Encode a custom-version envelope at absolute SAB offset `dstOff`.
  envelopeEncode(dstOff: number, version: number, headerSize: number, seq: number, payloadLen: number): void {
    envelopeEncode(this.dv, dstOff, version, headerSize, seq, payloadLen);
  }
}

// ---------------------------------------------------------------------------
// Envelope pure functions (03-ENVELOPE §1, §2)
// ---------------------------------------------------------------------------

/// Encode a triad-1 envelope at `dst` byte offset in `dv`.
export function envelopeEncodeV1(dv: DataView, dstOff: number, seq: number, payloadLen: number): void {
  envelopeEncode(dv, dstOff, WEFT_VERSION_1, 16, seq, payloadLen);
}

/// Encode a custom-version envelope.
export function envelopeEncode(dv: DataView, dstOff: number, version: number, headerSize: number, seq: number, payloadLen: number): void {
  // Per 03-ENVELOPE §1: all fields little-endian.
  dv.setUint32(dstOff, WEFT_MAGIC, true);
  dv.setUint16(dstOff + 4, version, true);
  dv.setUint16(dstOff + 6, headerSize, true);
  dv.setUint32(dstOff + 8, seq, true);
  dv.setUint32(dstOff + 12, payloadLen, true);
  // Unknown trailing fields filled with 0xAA (L8b convention).
  for (let i = 16; i < headerSize; i++) {
    dv.setUint8(dstOff + i, 0xAA);
  }
}

/// Decode an envelope. Per 03-ENVELOPE §2.
export function envelopeDecode(dv: DataView, srcOff: number, avail: number): { ok: boolean; version: number; headerSize: number; seq: number; payloadLen: number; result: DecodeResult } {
  if (avail < 16) return { ok: false, version: 0, headerSize: 0, seq: 0, payloadLen: 0, result: DecodeResult.Short };
  const magic = dv.getUint32(srcOff, true);
  if (magic !== WEFT_MAGIC) return { ok: false, version: 0, headerSize: 0, seq: 0, payloadLen: 0, result: DecodeResult.BadMagic };
  const version = dv.getUint16(srcOff + 4, true);
  const headerSize = dv.getUint16(srcOff + 6, true);
  if (headerSize < 16 || headerSize > avail) return { ok: false, version: 0, headerSize: 0, seq: 0, payloadLen: 0, result: DecodeResult.BadHeader };
  const seq = dv.getUint32(srcOff + 8, true);
  const payloadLen = dv.getUint32(srcOff + 12, true);
  if (payloadLen > avail - headerSize) return { ok: false, version: 0, headerSize: 0, seq: 0, payloadLen: 0, result: DecodeResult.Short };
  return { ok: true, version, headerSize, seq, payloadLen, result: DecodeResult.Ok };
}

/// Bind-time version negotiation. Per 03-ENVELOPE §3.
export function negotiate(writerVersion: number, readerVersions: number[]): number {
  let chosen = 0;
  for (const rv of readerVersions) {
    if (rv <= writerVersion && rv > chosen) chosen = rv;
  }
  return chosen;  // 0 = BIND_INCOMPATIBLE
}

// ---------------------------------------------------------------------------
// Shared payload pattern (04-LITMUS §0.1)
// ---------------------------------------------------------------------------

/// mix32(x) — per 04-LITMUS §0.1. u32 arithmetic (use Math.imul and >>> 0).
export function mix32(x: number): number {
  x = x ^ (x >>> 16);
  x = Math.imul(x, 0x7FEB352D);
  x = x ^ (x >>> 15);
  x = Math.imul(x, 0x846CA68B);
  x = x ^ (x >>> 16);
  return x >>> 0;
}

/// pat(seq, i) — per 04-LITMUS §0.1. Deterministic payload byte.
/// Identical to C and Rust (A5).
export function pat(seq: number, i: number): number {
  const x = (Math.imul(seq >>> 0, 2654435761) + Math.imul(i >>> 0, 2246822519)) >>> 0;
  return (mix32(x) & 0xFF) >>> 0;
}

/// xorshift32 step — per 04-LITMUS §0.2. Marsaglia 13/17/5.
/// State 0 is invalid (reseed 0x9E3779B9).
export function xorshift32(state: { v: number }): number {
  if (state.v === 0) state.v = 0x9E3779B9;
  let x = state.v;
  x = (x ^ (x << 13)) >>> 0;
  x = (x ^ (x >>> 17)) >>> 0;
  x = (x ^ (x << 5)) >>> 0;
  state.v = x;
  return x;
}
// ---------------------------------------------------------------------------
// Fan-out driver layer (RFC 0004) — 1 writer, N readers, userland-only.
// Accepted as a driver-layer pattern (round-6-adjudication §4); zero kernel
// surface. See src/fanout.ts for the protocol and its boundary of the claim.
// ---------------------------------------------------------------------------

export * from './fanout';

