// hotplane.js — the WHP2 hot-plane consumer engine for JavaScript.
//
// Zero-copy: the plane lives in a SharedArrayBuffer shared with C
// producers (via core/wasm/heddle_bridge.c compiled to WASM, or a
// native N-API/FFI producer). This class NEVER copies the plane, NEVER
// allocates on any hot path (Law 1), and reads through Atomics on a
// BigUint64Array view so every access is a data-race-free atomic —
// the JS equivalent of the engine's __atomic protocol (JS Atomics are
// sequentially consistent, which is STRONGER than the engine's
// acquire/release ordering, so the two-store bracket reads remain
// correct by construction).
//
// The two-store seqlock reader protocol (HOTPLANE-LAYOUT-V2 §4):
//   open : load commit_seq, load begin_seq — equal => frame opened
//   read : payload
//   close: load begin_seq — must still equal
//   ...bounded retries, then E_SEQ_TORN (never torn data, Law 4).

import * as L from './layout.js';

const U64 = BigUint64Array;

function hex(n) {
  return '0x' + n.toString(16);
}

export class HotPlane {
  // buffer: SharedArrayBuffer (or ArrayBuffer for read-only probes of
  // captured images) containing the plane at byte `offset` (must be a
  // multiple of 64).
  constructor(buffer, { offset = 0, role = L.ROLE_CONSUMER } = {}) {
    if (!(buffer instanceof SharedArrayBuffer) &&
        !(buffer instanceof ArrayBuffer)) {
      throw new Error(`HEDDLE_E_ARG: buffer must be a SharedArrayBuffer (${hex(offset)})`);
    }
    if (!Number.isInteger(offset) || offset < 0 || (offset & 63) !== 0) {
      throw new Error('HEDDLE_E_ARG: offset must be 64-byte aligned');
    }
    if (buffer.byteLength - offset < L.HEADER_SIZE) {
      throw new Error('HEDDLE_E_ARG: buffer smaller than the 128B header');
    }

    this.buffer = buffer;
    this.offset = offset;
    this.role = role;

    // Validation ladder (the same rungs as hplane_attach).
    const dv = new DataView(buffer, offset, L.HEADER_SIZE);
    const magic = dv.getUint32(L.OFF.magic, true);
    if (magic !== L.MAGIC) {
      throw new Error(`HEDDLE_E_MAGIC: ${hex(magic)}`);
    }
    const verMajor = dv.getUint16(L.OFF.verMajor, true);
    if (verMajor !== L.LAYOUT_MAJOR) {
      throw new Error(`HEDDLE_E_VERSION: major ${verMajor}`);
    }
    const headerSize = dv.getUint32(L.OFF.headerSize, true);
    const layoutRev = dv.getUint32(L.OFF.layoutRev, true);
    if (headerSize !== L.HEADER_SIZE || layoutRev !== L.LAYOUT_REV) {
      throw new Error('HEDDLE_E_LAYOUT: header geometry mismatch');
    }
    const canary = dv.getUint32(L.OFF.endianCanary, true);
    if (canary !== L.ENDIAN_CANARY) {
      throw new Error(`HEDDLE_E_ENDIAN: canary ${hex(canary)}`);
    }
    const bytes = new Uint8Array(buffer, offset, L.HEADER_SIZE);
    const crc = L.crc32(bytes, L.STATIC_CRC_COVER_OFF,
                        L.STATIC_CRC_COVER_LEN);
    const crcStored = dv.getUint32(L.OFF.staticCrc32, true);
    if (crc !== crcStored) {
      throw new Error(`HEDDLE_E_CFG_CRC: ${hex(crc)} != ${hex(crcStored)}`);
    }

    // Static config (immutable).
    this.mode = dv.getUint32(L.OFF.mode, true);
    this.laneCount = dv.getUint32(L.OFF.laneCount, true);
    this.laneStride = dv.getUint32(L.OFF.laneStride, true);
    this.sampleSize = dv.getUint32(L.OFF.sampleSize, true);
    this.slotCapacity = dv.getUint32(L.OFF.slotCapacity, true);
    this.statKind = dv.getUint32(L.OFF.statKind, true);
    this.flags = dv.getUint32(L.OFF.flags, true);
    this.planeSize = dv.getBigUint64(L.OFF.planeSize, true);
    this.createStampNs = dv.getBigUint64(L.OFF.createStampNs, true);

    if (buffer.byteLength - offset < Number(this.planeSize)) {
      throw new Error('HEDDLE_E_REGION_SIZE: buffer smaller than plane_size');
    }
    if (this.laneCount === 0 || this.laneCount > L.MAX_LANES) {
      throw new Error('HEDDLE_E_CAPACITY: lane_count out of range');
    }

    // The 8-byte register view over the WHOLE plane region (header
    // cacheline 1, lane descriptors, ring slot pairs — every normative
    // register is 8-byte aligned; cell payloads are read as bytes).
    const regionBytes = Number(this.planeSize);
    this.u64 = new U64(buffer, offset, regionBytes / 8);
    this.stride = this.mode === L.MODE_RING
      ? L.slotStride(this.sampleSize)
      : L.cellStride(this.sampleSize);
    this.laneDataBase = L.HEADER_SIZE + this.laneCount * L.LANE_DESC_SIZE;
    this.cellOf = (lane, cell) =>
      this.offset + this.laneDataBase + lane * this.laneStride +
      cell * this.stride;
  }

  // ---- volatile register getters ----------------------------------------
  get beginSeq()  { return this.u64[L.OFF.beginSeq / 8]; }
  get commitSeq() { return this.u64[L.OFF.commitSeq / 8]; }
  get dirtyMask() { return this.u64[L.OFF.dirtyMask / 8]; }
  get renderFrameId() { return this.u64[L.OFF.renderFrameId / 8]; }
  get heartbeatNs() { return this.u64[L.OFF.heartbeatNs / 8]; }
  get dirtyTransitions() { return this.u64[L.OFF.dirtyTransitions / 8]; }

  // Epoch: multi-producer planes maintain the register; single-producer
  // planes derive it from commit_seq (values coincide — §3.1).
  get epoch() {
    return (this.flags & L.F_MULTI_PRODUCER)
      ? this.u64[L.OFF.epoch / 8]
      : this.u64[L.OFF.commitSeq / 8];
  }

  // ---- dirty plane / frame protocol --------------------------------------
  // Harvest: exchange the active mask with 0 and return the previous
  // mask. One call per frame, render thread only.
  harvestMask() {
    this.#requireRole(L.ROLE_CONSUMER, 'harvestMask');
    return Atomics.exchange(this.u64, L.OFF.dirtyMask / 8, 0n);
  }

  // Re-mark lanes (defer a torn lane to the next frame — lossless).
  remark(bits) {
    this.#requireRole(L.ROLE_CONSUMER, 'remark');
    if (typeof bits !== 'bigint') {
      throw new Error('HEDDLE_E_ARG: remark(bits) takes a BigInt mask');
    }
    const valid = this.laneCount >= 64
      ? 0xffffffffffffffffn
      : (1n << BigInt(this.laneCount)) - 1n;
    if ((bits & ~valid) !== 0n) {
      throw new Error('HEDDLE_E_LANE_OVERFLOW: bits beyond lane_count');
    }
    return Atomics.or(this.u64, L.OFF.dirtyMask / 8, bits);
  }

  // Publish a completed frame; returns the new frame id.
  frameCommit() {
    this.#requireRole(L.ROLE_CONSUMER, 'frameCommit');
    return Atomics.add(this.u64, L.OFF.renderFrameId / 8, 1n) + 1n;
  }

  // ---- bounded two-store bracket read over a lane pair -------------------
  #laneOpen(lane) {
    const base = L.HEADER_SIZE + lane * L.LANE_DESC_SIZE;
    const c = Atomics.load(this.u64, (base + L.LANE_OFF.commitSeq) / 8);
    const b = Atomics.load(this.u64, (base + L.LANE_OFF.beginSeq) / 8);
    return (c === b) ? c : null;
  }
  #laneClose(lane, open) {
    const base = L.HEADER_SIZE + lane * L.LANE_DESC_SIZE;
    return Atomics.load(this.u64, (base + L.LANE_OFF.beginSeq) / 8) === open;
  }

  #requireRole(role, op) {
    if (!(this.role & role)) {
      throw new Error(`HEDDLE_E_STATE: ${op} requires role ${role}`);
    }
  }

  // ---- reads (state mode) -------------------------------------------------
  // Reads one cell into `out` (Uint8Array of sampleSize). Returns
  // HEDDLE_OK or HEDDLE_E_SEQ_TORN after bounded retries.
  readCell(lane, cell, out, maxRetries = 64) {
    if (this.mode !== L.MODE_STATE) {
      throw new Error('HEDDLE_E_MODE: readCell is a state-plane op');
    }
    if (lane >= this.laneCount) {
      throw new Error('HEDDLE_E_LANE_OVERFLOW');
    }
    if (cell >= this.slotCapacity) {
      throw new Error('HEDDLE_E_CAPACITY');
    }
    if (!out || out.byteLength < this.sampleSize) {
      throw new Error('HEDDLE_E_PAYLOAD: out too small for sample_size');
    }
    const abs = this.cellOf(lane, cell);
    const view = new Uint8Array(this.buffer, abs, this.sampleSize);
    for (let r = 0; r <= maxRetries; r++) {
      const open = this.#laneOpen(lane);
      if (open === null) {
        continue;
      }
      out.set(view);
      if (this.#laneClose(lane, open)) {
        return L.OK;
      }
    }
    return L.E_SEQ_TORN;
  }

  // ---- reads (ring mode) ----------------------------------------------------
  // Reads sample ordinal `seq` (1-based, per lane). Refusals mirror the
  // engine exactly: E_OVERRUN / E_NOT_PUBLISHED / E_STATE / E_SEQ_TORN.
  readSlot(lane, seq, out, maxRetries = 64) {
    if (this.mode !== L.MODE_RING) {
      throw new Error('HEDDLE_E_MODE: readSlot is a ring-plane op');
    }
    if (lane >= this.laneCount) {
      throw new Error('HEDDLE_E_LANE_OVERFLOW');
    }
    if (!out || out.byteLength < this.sampleSize) {
      throw new Error('HEDDLE_E_PAYLOAD');
    }
    const slotIdx = Number((seq - 1n) % BigInt(this.slotCapacity));
    const abs = this.cellOf(lane, slotIdx);
    const commitIdx = (abs - this.offset + L.SLOT_HEADER_SIZE - 8) / 8;
    const beginIdx = (abs - this.offset) / 8;
    const view = new Uint8Array(this.buffer,
                                abs + L.SLOT_HEADER_SIZE,
                                this.sampleSize);
    for (let r = 0; r <= maxRetries; r++) {
      const cm = Atomics.load(this.u64, commitIdx);
      const bn = Atomics.load(this.u64, beginIdx);
      if (cm !== bn) {
        continue;                      // slot being (re)written
      }
      if (cm === 0n) {
        return L.E_STATE;              // never written
      }
      if (cm !== seq) {
        return cm > seq ? L.E_OVERRUN : L.E_NOT_PUBLISHED;
      }
      out.set(view);
      if (Atomics.load(this.u64, beginIdx) === cm) {
        return L.OK;
      }
    }
    return L.E_SEQ_TORN;
  }

  ringHead(lane) {
    if (this.mode !== L.MODE_RING) {
      throw new Error('HEDDLE_E_MODE: ringHead is a ring-plane op');
    }
    if (lane >= this.laneCount) {
      throw new Error('HEDDLE_E_LANE_OVERFLOW');
    }
    const base = L.HEADER_SIZE + lane * L.LANE_DESC_SIZE;
    return Atomics.load(this.u64, (base + L.LANE_OFF.current) / 8);
  }

  // ---- lane stats + bounding box ------------------------------------------
  laneStats(lane, maxRetries = 64) {
    if (lane >= this.laneCount) {
      throw new Error('HEDDLE_E_LANE_OVERFLOW');
    }
    const base = L.HEADER_SIZE + lane * L.LANE_DESC_SIZE;
    for (let r = 0; r <= maxRetries; r++) {
      const open = this.#laneOpen(lane);
      if (open === null) {
        continue;
      }
      const st = {
        minRaw: Atomics.load(this.u64, (base + L.LANE_OFF.min) / 8),
        maxRaw: Atomics.load(this.u64, (base + L.LANE_OFF.max) / 8),
        currentRaw: Atomics.load(this.u64, (base + L.LANE_OFF.current) / 8),
        commitCount: Atomics.load(this.u64, (base + L.LANE_OFF.commitCnt) / 8),
      };
      if (this.#laneClose(lane, open)) {
        return st;
      }
    }
    throw new Error('HEDDLE_E_SEQ_TORN: lane stats torn after retries');
  }

  // Bounding-box harvest: bracketed read of the producer-owned register
  // (mutations since the producer last observed this consumer's frame
  // advance). EMPTY only for lanes never written.
  bboxHarvest(lane, maxRetries = 64) {
    if (lane >= this.laneCount) {
      throw new Error('HEDDLE_E_LANE_OVERFLOW');
    }
    const base = L.HEADER_SIZE + lane * L.LANE_DESC_SIZE;
    for (let r = 0; r <= maxRetries; r++) {
      const open = this.#laneOpen(lane);
      if (open === null) {
        continue;
      }
      const bbox = Atomics.load(this.u64, (base + L.LANE_OFF.bbox) / 8);
      if (this.#laneClose(lane, open)) {
        return {
          min: Number(bbox >> 32n),
          max: Number(bbox & 0xffffffffn),
          packed: bbox,
        };
      }
    }
    throw new Error('HEDDLE_E_SEQ_TORN: bbox torn after retries');
  }

  // ---- descriptor -----------------------------------------------------------
  describe() {
    return {
      magic: L.MAGIC,
      verMajor: L.LAYOUT_MAJOR,
      verMinor: L.LAYOUT_MINOR,
      headerSize: L.HEADER_SIZE,
      layoutRev: L.LAYOUT_REV,
      mode: this.mode,
      laneCount: this.laneCount,
      laneStride: this.laneStride,
      sampleSize: this.sampleSize,
      slotCapacity: this.slotCapacity,
      statKind: this.statKind,
      flags: this.flags,
      planeSize: this.planeSize,
      createStampNs: this.createStampNs,
      multiProducer: !!(this.flags & L.F_MULTI_PRODUCER),
    };
  }
}
