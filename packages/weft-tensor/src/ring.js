// ring.js — WeftTensorRing: single-producer / multi-consumer seqlock tensor ring.
//
// Spec: docs/weft-tensor/LAYOUT-V1.md (NORMATIVE).
//
// Publish = two SeqCst 32-bit atomic stores (hi word first, LO WORD LAST) into
// the u64 producer_seq word. This avoids a BigInt allocation per commit
// (Law 1). Torn two-word reads can never corrupt a consumer: every acquire
// re-validates the slot's own 64-bit seq + magic + committed bit (seqlock),
// so a torn producer_seq read merely costs one bounded retry.
//
// Consumers are wait-free: acquire*() never blocks the producer and never
// allocates. Producers never wait on consumers (no back-pressure in v1 —
// overwrite semantics, frame loss is reported, never corruption).

import {
  RING_MAGIC, SLOT_MAGIC, LAYOUT_VERSION, RING_HEADER_SIZE, SLOT_HEADER_SIZE,
  OFF_MAGIC, OFF_LAYOUT_VERSION, OFF_HEADER_SIZE, OFF_SLOT_COUNT, OFF_SLOT_STRIDE,
  OFF_DTYPE_CODE, OFF_DTYPE_BITS, OFF_LANES, OFF_ELEM_SIZE, OFF_SHAPE, OFF_STRIDES,
  OFF_SCHEMA_ID, OFF_PRODUCER_SEQ, OFF_TICK_HZ, OFF_FLAGS, OFF_HEADER_CRC,
  RING_FLAG_LITTLE_ENDIAN, RING_FLAG_SHARED_MEMORY,
  SOFF_MAGIC, SOFF_PAYLOAD_LEN, SOFF_SEQ, SOFF_TIMESTAMP_NS, SOFF_DURATION_US, SOFF_SLOT_FLAGS,
  SOFF_FOURCC, SOFF_RANK, SOFF_PLANES, SOFF_PLANE_OFFSET, SOFF_PLANE_SIZE,
  SLOT_FLAG_COMMITTED, MAX_RANK, DLPackCode,
  validateRingHeader, readSlotHeader, ringHeaderCrc, LayoutError, fourccFromString,
} from './layout.js';
import { WeftTensorFrameView } from './frame-view.js';

const TWO32 = 4294967296;
const SEQ_HI_LIMIT = 0x200000; // producer_seq < 2^53 (managed Number path, exact)

// Host endianness probe — module-lifetime, zero steady-state cost.
const _leProbeU32 = new Uint32Array(1);
const _leProbeU8 = new Uint8Array(_leProbeU32.buffer);
_leProbeU32[0] = 1;
const HOST_LITTLE_ENDIAN = _leProbeU8[0] === 1;

function align64(n) { return (n + 63) & ~63; }

function isShared(buffer) {
  return typeof SharedArrayBuffer !== 'undefined' &&
    buffer[Symbol.toStringTag] === 'SharedArrayBuffer';
}

function splitU64(value) {
  // Accepts BigInt | safe Number | {lo, hi}. Returns {lo, hi} u32 pair.
  if (typeof value === 'bigint') {
    return { lo: Number(value & 0xffffffffn), hi: Number((value >> 32n) & 0xffffffffn) };
  }
  if (typeof value === 'number') {
    if (!Number.isInteger(value) || value < 0 || value >= SEQ_HI_LIMIT * TWO32) {
      throw new LayoutError('WTR1_BAD_U64', `u64 value ${value} not representable (use BigInt or {lo,hi})`);
    }
    return { lo: value % TWO32, hi: Math.floor(value / TWO32) };
  }
  if (value && typeof value === 'object') return { lo: value.lo >>> 0, hi: value.hi >>> 0 };
  throw new LayoutError('WTR1_BAD_U64', `cannot interpret u64 value ${value}`);
}

/** Typed-array constructor for the ring's dtype (null for f16/bfloat — byte-level only). */
function typedCtorFor(code, bits) {
  switch (code) {
    case DLPackCode.INT:
      if (bits === 8) return Int8Array;
      if (bits === 16) return Int16Array;
      if (bits === 32) return Int32Array;
      if (bits === 64) return BigInt64Array;
      return null;
    case DLPackCode.UINT:
      if (bits === 8) return Uint8Array;
      if (bits === 16) return Uint16Array;
      if (bits === 32) return Uint32Array;
      if (bits === 64) return BigUint64Array;
      return null;
    case DLPackCode.FLOAT:
      if (bits === 16) return null; // no engine Float16Array — use f16 codec / byte paths
      if (bits === 32) return Float32Array;
      if (bits === 64) return Float64Array;
      return null;
    case DLPackCode.BOOL: return Uint8Array;
    default: return null;
  }
}

export class WeftTensorRing {
  // Private state — attached once, then immutable references.
  _buffer; _dv; _layout; _futex;      // Int32Array over producer_seq (lo, hi) — LE host
  _slot8 = [];                        // per-slot Uint8Array over payload cap
  _slotTyped = null;                  // per-slot typed view over payload (ring dtype) or null
  _meta = {};                         // reused slot-header scratch
  _view = new WeftTensorFrameView();  // reused flyweight
  stats = { commits: 0, acquireCalls: 0, acquireRetries: 0, tornReads: 0, overruns: 0 };

  constructor(buffer, layout) {
    if (!HOST_LITTLE_ENDIAN) {
      // Fail-closed: our fast publish path maps Int32 words onto the u64
      // producer_seq assuming LE hosts (all supported targets are LE).
      throw new LayoutError('WTR1_BE_HOST', 'big-endian hosts are unsupported in V1 (fail-closed)');
    }
    this._buffer = buffer;
    this._layout = layout;
    this._dv = new DataView(buffer);
    this._futex = new Int32Array(buffer, OFF_PRODUCER_SEQ, 2);
    const Ctor = typedCtorFor(layout.dtype.code, layout.dtype.bits);
    for (let s = 0; s < layout.slotCount; s++) {
      const payloadBase = layout.headerSize + s * layout.slotStride + SLOT_HEADER_SIZE;
      this._slot8.push(new Uint8Array(buffer, payloadBase, layout.payloadCap));
    }
    if (Ctor !== null && layout.payloadCap % layout.elemSize === 0) {
      this._slotTyped = { Ctor, views: [] };
      for (let s = 0; s < layout.slotCount; s++) {
        const payloadBase = layout.headerSize + s * layout.slotStride + SLOT_HEADER_SIZE;
        this._slotTyped.views.push(new Ctor(buffer, payloadBase, layout.payloadCap / layout.elemSize));
      }
    }
    Object.seal(this.stats);
  }

  // -- properties -------------------------------------------------------------

  get buffer() { return this._buffer; }
  get isShared() { return isShared(this._buffer); }
  get layout() { return this._layout; }
  get slotCount() { return this._layout.slotCount; }
  get slotStride() { return this._layout.slotStride; }
  get payloadCap() { return this._layout.payloadCap; }
  get byteLength() { return this._buffer.byteLength; }
  /** Latest committed sequence (0 = nothing committed yet). Exact Number < 2^53. */
  get producerSeq() {
    const lo = Atomics.load(this._futex, 0);
    const hi = Atomics.load(this._futex, 1);
    return hi * TWO32 + lo;
  }
  get producerSeqLoHi() {
    return { lo: Atomics.load(this._futex, 0), hi: Atomics.load(this._futex, 1) };
  }

  // -- construction ------------------------------------------------------------

  /**
   * Create a new ring (allocates header + slots, validates itself).
   * @param {object} opts
   *   slotCount     >= 2 (default 4)
   *   payloadCap    payload bytes per slot (must be >= 1, 64-aligned NOT required — stride pads)
   *   dtype         {code, bits, lanes=1} — DLPack DLDataTypeCode on the wire
   *   shape         array of dims (rank <= 8)
   *   schemaId      BigInt | Number | {lo,hi}
   *   tickHz        producer rate hint (default 0)
   *   shared        allocate SharedArrayBuffer (default: false)
   *   fourcc        default payload format tag for commits (default 'RAW ')
   */
  static create(opts) {
    const {
      slotCount = 4,
      payloadCap,
      dtype = { code: DLPackCode.UINT, bits: 8, lanes: 1 },
      shape,
      schemaId = 0,
      tickHz = 0,
      shared = false,
      fourcc = 'RAW ',
    } = opts ?? {};
    if (!Number.isInteger(slotCount) || slotCount < 2) {
      throw new LayoutError('WTR1_BAD_SLOT_COUNT', `slotCount ${slotCount} must be an integer >= 2`);
    }
    if (!Number.isInteger(payloadCap) || payloadCap < 1) {
      throw new LayoutError('WTR1_BAD_PAYLOAD_CAP', `payloadCap ${payloadCap} must be an integer >= 1`);
    }
    const { code, bits, lanes = 1 } = dtype;
    if (lanes !== 1) throw new LayoutError('WTR1_BAD_DTYPE', 'lanes != 1 unsupported in V1');
    if (!Array.isArray(shape) || shape.length < 1 || shape.length > MAX_RANK) {
      throw new LayoutError('WTR1_BAD_RANK', `shape must be an array of 1..${MAX_RANK} dims`);
    }
    let elems = 1;
    for (const d of shape) {
      if (!Number.isInteger(d) || d < 1) throw new LayoutError('WTR1_BAD_SHAPE', `dim ${d} invalid`);
      elems *= d;
    }
    const elemSize = (bits >>> 3) * lanes;
    if (elems * elemSize > payloadCap) {
      throw new LayoutError('WTR1_TOO_SMALL', `shape needs ${elems * elemSize}B > payloadCap ${payloadCap}B`);
    }
    const slotStride = align64(SLOT_HEADER_SIZE + payloadCap);
    const total = RING_HEADER_SIZE + slotCount * slotStride;
    const buffer = shared ? new SharedArrayBuffer(total) : new ArrayBuffer(total);
    const dv = new DataView(buffer);

    // Static header config.
    dv.setUint8(OFF_MAGIC + 0, RING_MAGIC[0]);
    dv.setUint8(OFF_MAGIC + 1, RING_MAGIC[1]);
    dv.setUint8(OFF_MAGIC + 2, RING_MAGIC[2]);
    dv.setUint8(OFF_MAGIC + 3, RING_MAGIC[3]);
    dv.setUint16(OFF_LAYOUT_VERSION, LAYOUT_VERSION, true);
    dv.setUint16(OFF_HEADER_SIZE, RING_HEADER_SIZE, true);
    dv.setUint32(OFF_SLOT_COUNT, slotCount, true);
    dv.setUint32(OFF_SLOT_STRIDE, slotStride, true);
    dv.setUint8(OFF_DTYPE_CODE, code);
    dv.setUint8(OFF_DTYPE_BITS, bits);
    dv.setUint16(OFF_LANES, lanes, true);
    dv.setUint32(OFF_ELEM_SIZE, elemSize, true);
    for (let d = 0; d < MAX_RANK; d++) {
      dv.setUint32(OFF_SHAPE + 4 * d, d < shape.length ? shape[d] : 0, true);
      dv.setUint32(OFF_STRIDES + 4 * d, 0, true);
    }
    // Row-major element strides.
    let acc = 1;
    for (let d = shape.length - 1; d >= 0; d--) {
      dv.setUint32(OFF_STRIDES + 4 * d, acc, true);
      acc *= shape[d];
    }
    const sid = splitU64(schemaId);
    dv.setUint32(OFF_SCHEMA_ID, sid.lo, true);
    dv.setUint32(OFF_SCHEMA_ID + 4, sid.hi, true);
    dv.setUint32(OFF_TICK_HZ, tickHz >>> 0, true);
    dv.setUint32(OFF_FLAGS, RING_FLAG_LITTLE_ENDIAN | (shared ? RING_FLAG_SHARED_MEMORY : 0), true);
    dv.setUint32(OFF_HEADER_CRC, ringHeaderCrc(dv), true);
    // producer_seq stays 0; slot headers stay zeroed (magic-less = never used).

    const ring = new WeftTensorRing(buffer, validateRingHeader(buffer));
    ring._defaultFourcc = fourccFromString(fourcc);
    return ring;
  }

  /**
   * Adopt EXISTING memory (fixture files, mmap, SharedArrayBuffer from a
   * worker, Python-written ring). Full Law-4 validation runs here.
   */
  static attach(buffer) {
    const layout = validateRingHeader(buffer);
    const ring = new WeftTensorRing(buffer, layout);
    ring._defaultFourcc = fourccFromString('RAW ');
    return ring;
  }

  // -- producer ----------------------------------------------------------------

  /**
   * Zero-copy producer fast path: returns the preallocated Uint8Array view for
   * the NEXT slot (seq = producerSeq + 1). Write your payload straight into it
   * (e.g. VideoFrame.copyTo(view, {format:'RGBA'}) — GPU -> ring, zero
   * intermediate), then call finishCommit().
   */
  beginCommit() {
    const seq = this.producerSeq + 1;
    const slot = (seq - 1) % this._layout.slotCount;
    return {
      seq, slot,
      payloadU8: this._slot8[slot],
      payloadTyped: this._slotTyped !== null ? this._slotTyped.views[slot] : null,
    };
  }

  /**
   * Publish a frame previously targeted by beginCommit().
   * @param {number} byteLen  live payload bytes (<= payloadCap)
   * @param {object} [meta]   { ts?: BigInt, tsLo?, tsHi?, timestampNs?: Number,
   *                            durationUs?, fourcc?: string|u32, rank?, planes? }
   */
  finishCommit(handle, byteLen, meta) {
    if (byteLen < 0 || byteLen > this._layout.payloadCap) {
      throw new LayoutError('WTR1_COMMIT_RANGE', `payload ${byteLen}B > cap ${this._layout.payloadCap}B`);
    }
    const { seq, slot } = handle;
    const base = this._layout.headerSize + slot * this._layout.slotStride;
    const dv = this._dv;

    let tsLo = 0, tsHi = 0;
    if (meta) {
      if (meta.ts !== undefined) { const t = splitU64(meta.ts); tsLo = t.lo; tsHi = t.hi; }
      else if (meta.tsLo !== undefined || meta.tsHi !== undefined) { tsLo = meta.tsLo >>> 0; tsHi = meta.tsHi >>> 0; }
      else if (meta.timestampNs !== undefined) { const t = splitU64(meta.timestampNs); tsLo = t.lo; tsHi = t.hi; }
    }
    let fourccU32 = this._defaultFourcc;
    if (meta && meta.fourcc !== undefined) {
      fourccU32 = typeof meta.fourcc === 'string' ? fourccFromString(meta.fourcc) : meta.fourcc >>> 0;
    }
    const rank = meta && meta.rank !== undefined ? meta.rank : this._layout.rank;
    const planes = meta && meta.planes !== undefined ? meta.planes : 1;

    // Slot header with COMMITTED bit CLEAR (torn marker) — then all fields —
    // then the COMMITTED bit — then the publish stores. Readers see a fully
    // written header or nothing.
    dv.setUint8(base + SOFF_MAGIC + 0, SLOT_MAGIC[0]);
    dv.setUint8(base + SOFF_MAGIC + 1, SLOT_MAGIC[1]);
    dv.setUint8(base + SOFF_MAGIC + 2, SLOT_MAGIC[2]);
    dv.setUint8(base + SOFF_MAGIC + 3, SLOT_MAGIC[3]);
    dv.setUint32(base + SOFF_SLOT_FLAGS, 0, true);
    dv.setUint32(base + SOFF_PAYLOAD_LEN, byteLen, true);
    dv.setUint32(base + SOFF_SEQ, (seq % TWO32) | 0, true);
    dv.setUint32(base + SOFF_SEQ + 4, Math.floor(seq / TWO32) | 0, true);
    dv.setUint32(base + SOFF_TIMESTAMP_NS, tsLo, true);
    dv.setUint32(base + SOFF_TIMESTAMP_NS + 4, tsHi, true);
    dv.setUint32(base + SOFF_DURATION_US, (meta && meta.durationUs) || 0, true);
    dv.setUint32(base + SOFF_FOURCC, fourccU32, true);
    dv.setUint8(base + SOFF_RANK, rank);
    dv.setUint8(base + SOFF_PLANES, planes);
    dv.setUint32(base + SOFF_PLANE_OFFSET, 0, true);
    dv.setUint32(base + SOFF_PLANE_OFFSET + 4, 0, true);
    dv.setUint32(base + SOFF_PLANE_OFFSET + 8, 0, true);
    dv.setUint32(base + SOFF_PLANE_SIZE, byteLen, true);
    dv.setUint32(base + SOFF_PLANE_SIZE + 4, 0, true);
    dv.setUint32(base + SOFF_PLANE_SIZE + 8, 0, true);
    dv.setUint32(base + SOFF_SLOT_FLAGS, SLOT_FLAG_COMMITTED, true); // commit marker

    // Publish: hi word first, LO WORD LAST (SeqCst). Zero BigInt allocation.
    const seqLo = (seq % TWO32) | 0, seqHi = Math.floor(seq / TWO32) | 0;
    Atomics.store(this._futex, 1, seqHi);
    Atomics.store(this._futex, 0, seqLo);
    this.stats.commits++;
    return seq;
  }

  /**
   * Copy-in convenience: begin -> memcpy -> finish. `src` may be a TypedArray
   * of the ring's dtype (typed memcpy, exact length) or a Uint8Array (byte
   * copy). Short payloads use a bounded loop — no allocation on any path.
   * @returns {number} seq
   */
  commit(src, meta) {
    const h = this.beginCommit();
    let byteLen;
    if (src instanceof Uint8Array || src instanceof Int8Array) {
      byteLen = src.length;
      const dst = h.payloadU8;
      if (byteLen === dst.length) dst.set(src);
      else for (let i = 0; i < byteLen; i++) dst[i] = src[i];
    } else if (this._slotTyped !== null && src.constructor === this._slotTyped.Ctor) {
      byteLen = src.length * this._layout.elemSize;
      if (src.length > h.payloadTyped.length) {
        throw new LayoutError('WTR1_COMMIT_RANGE',
          `payload ${byteLen}B > cap ${this._layout.payloadCap}B`);
      }
      h.payloadTyped.set(src);
    } else {
      throw new LayoutError('WTR1_COMMIT_DTYPE',
        `src ${src.constructor.name} does not match ring dtype — use beginCommit() for byte-level control`);
    }
    return this.finishCommit(h, byteLen, meta);
  }

  /** Producer rate in Hz hint from the header. */
  get tickHz() { return this._layout.tickHz; }

  // -- consumer ----------------------------------------------------------------

  /**
   * Acquire the latest committed frame into the REUSED flyweight view.
   * Wait-free, allocation-free. Torn reads are detected via the slot seqlock
   * and retried (bounded by slotCount); returns null when nothing is
   * available yet / nothing new since `lastSeq` (if given) / overrun window.
   *
   * @param {number} [lastSeq] when provided, returns null unless a NEWER frame exists
   */
  acquireLatest(lastSeq) {
    this.stats.acquireCalls++;
    const L = this._layout;
    for (let attempt = 0; attempt < L.slotCount; attempt++) {
      const lo = Atomics.load(this._futex, 0);
      const hi = Atomics.load(this._futex, 1);
      const s = hi * TWO32 + lo;
      if (s === 0) return null;                              // nothing ever committed
      if (lastSeq !== undefined && s <= lastSeq) return null; // nothing new
      const seq = s;                                         // latest committed frame NUMBER (1-based)
      const slot = (seq - 1) % L.slotCount;
      const base = L.headerSize + slot * L.slotStride;
      if (readSlotHeader(this._dv, base, this._meta) && this._meta.seq === seq) {
        return this._view.bind(this, base, this._meta, this._slot8[slot]);
      }
      // Torn read or producer lapped us — bounded retry.
      this.stats.acquireRetries++;
      this.stats.tornReads++;
    }
    this.stats.overruns++;
    return null;
  }

  /**
   * Acquire the frame with an EXACT sequence number, or null when it already
   * fell out of the ring (consumer too slow) or is not yet committed.
   */
  acquireFrame(seq) {
    if (!Number.isInteger(seq) || seq < 1) return null;
    this.stats.acquireCalls++;
    const L = this._layout;
    const latest = this.producerSeq;
    if (seq > latest) return null; // not yet
    if (latest - seq >= L.slotCount) { this.stats.overruns++; return null; } // gone
    const slot = (seq - 1) % L.slotCount;
    const base = L.headerSize + slot * L.slotStride;
    if (readSlotHeader(this._dv, base, this._meta) && this._meta.seq === seq) {
      return this._view.bind(this, base, this._meta, this._slot8[slot]);
    }
    return null;
  }

  /**
   * Wait until producerSeq advances past `lastSeq`.
   * Uses Atomics.wait when allowed (workers / Node / Deno on shared memory),
   * else 1ms polling via the injected scheduler (browser main thread).
   *
   * @returns {Promise<number|null>} new producerSeq, or null on timeout
   */
  async waitForNewFrame(lastSeq, timeoutMs = 1000, scheduler = _defaultScheduler()) {
    const deadline = _now() + timeoutMs;
    for (;;) {
      const cur = this.producerSeq;
      if (cur > lastSeq) return cur; // advanced past the caller's watermark
      const remaining = deadline - _now();
      if (remaining <= 0) return null;
      const waited = await scheduler.wait(this, lastSeq, Math.min(remaining, 50));
      if (waited === false) continue; // scheduler wants a re-poll
    }
  }

  /** Human-readable one-line identity for logs/HUDs. */
  describe() {
    const L = this._layout;
    const shape = [];
    for (let d = 0; d < L.rank; d++) shape.push(L.shape[d]);
    return `WeftTensorRing(v${L.version}, slots=${L.slotCount}, stride=${L.slotStride}, ` +
      `dtype=${L.dtype.code}/${L.dtype.bits}, shape=[${shape.join('x')}], ` +
      `schema=0x${L.schemaId.toString(16)}, shared=${this.isShared})`;
  }
}

// -- scheduler plumbing (injectable for tests; zero dep on node: in src) -------

function _now() {
  return (typeof performance !== 'undefined' && typeof performance.now === 'function')
    ? performance.now()
    : Date.now();
}

function _defaultScheduler() {
  return {
    async wait(ring, lastSeq, capMs) {
      if (ring.isShared && typeof Atomics !== 'undefined' && Atomics.wait) {
        // Allowed on workers/Node/Deno; browsers may throw on main thread.
        try {
          const { lo, hi } = ring.producerSeqLoHi;
          if (hi * TWO32 + lo !== lastSeq) return true; // changed while we looked
          Atomics.wait(ring._futex, 0, lo, capMs);
          return true;
        } catch { /* main thread — fall through to polling */ }
      }
      await new Promise((res) => {
        if (typeof setTimeout === 'function') setTimeout(res, 1);
        else res(undefined);
      });
      return true;
    },
  };
}
