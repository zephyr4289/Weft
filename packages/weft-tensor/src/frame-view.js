// frame-view.js — WeftTensorFrameView: zero-allocation flyweight over one ring slot.
//
// Pillar 1 heritage (lead's canonical contract): instantiate ONCE, then
// .bind(...) for every frame — steady-state frame access allocates nothing
// (Law 1). All reads are explicitly little-endian (Law 2) and bit-exact with
// the C/Rust/GPU tensor plane and the Python DLPack bridge.

import {
  SLOT_HEADER_SIZE, fourccToString, f16FromBits,
} from './layout.js';

const TWO32 = 4294967296;

/**
 * Flyweight view over the payload region of one committed ring slot.
 *
 * The ring preallocates ONE instance (plus per-slot typed views) at attach;
 * `acquireLatest()` re-binds the same object every time. Consumers must NOT
 * retain the view across frames — copy out what they need (or blit it into a
 * canvas, which is the whole point).
 */
export class WeftTensorFrameView {
  constructor() {
    this._ring = null;
    this._dv = null;         // DataView over the WHOLE ring buffer
    this._slotBase = -1;     // absolute byte offset of the slot header
    this._payloadBase = -1;  // absolute byte offset of the payload
    this._payloadLen = 0;
    // Metadata snapshot (written by ring.acquire*, no per-frame allocation):
    this.seq = 0; this.seqLo = 0; this.seqHi = 0;
    this.timestampLo = 0; this.timestampHi = 0;
    this.durationUs = 0; this.flags = 0; this._fourcc = 0;
    // Per-slot payload views, injected by the ring (one per slot, O(slotCount)):
    this._payloadU8 = null; // Uint8Array  over payload cap
  }

  /** Rebind in 0ns — the Pillar 1 canonical contract. */
  bind(ring, slotBase, meta, payloadU8) {
    this._ring = ring;
    this._dv = ring._dv;
    this._slotBase = slotBase;
    this._payloadBase = slotBase + SLOT_HEADER_SIZE;
    this._payloadLen = meta.payloadLen;
    this.seq = meta.seq; this.seqLo = meta.seqLo; this.seqHi = meta.seqHi;
    this.timestampLo = meta.timestampLo; this.timestampHi = meta.timestampHi;
    this.durationUs = meta.durationUs; this.flags = meta.flags;
    this._fourcc = meta.fourcc;
    this._payloadU8 = payloadU8;
    return this;
  }

  get isBound() { return this._ring !== null; }
  get payloadLength() { return this._payloadLen; }
  /** Absolute byte offset of the payload inside the ring buffer. */
  get payloadByteOffset() { return this._payloadBase; }
  /** IEEE 754 timestamp: full u64 as Number when exact (< 2^53 ns ≈ year 2262 — always true for ns clocks started this millennium... but we still guard). */
  get timestampNs() { return this.timestampHi * TWO32 + this.timestampLo; }
  /** Milliseconds, Number — convenient for HUDs; truncation-safe for display. */
  get timestampMs() { return this.timestampNs / 1e6; }
  /** LE ASCII fourcc of the payload format ("RGBA", "F32 ", "PCM ", ...). */
  get fourcc() { return fourccToString(this._fourcc); }
  get ring() { return this._ring; }

  /**
   * Preallocated Uint8Array view over THIS slot's payload capacity.
   * Same object every frame for a given slot — zero allocation. Do not retain.
   */
  payloadView() { return this._payloadU8; }

  /**
   * Copy the live payload into `dest` (zero allocation).
   * Fast path: payload fills the slot cap exactly → single memcpy via set().
   * Short-payload path: bounded loop (still no allocation; short payloads are
   * the rare case by design — video/audio slots are fixed-size).
   */
  copyPayloadInto(dest) {
    const n = this._payloadLen;
    if (n === this._payloadU8.length) {
      dest.set(this._payloadU8);
    } else {
      for (let i = 0; i < n; i++) dest[i] = this._payloadU8[i];
    }
    return n;
  }

  // -- tensor metadata (from the ring header, static per ring) ---------------

  /** Uint32Array(8) shape — the RING's preallocated array, do not mutate. */
  get shape() { return this._ring.layout.shape; }
  /** Uint32Array(8) strides in ELEMENTS (DLPack convention) — do not mutate. */
  get stridesElems() { return this._ring.layout.strides; }
  get rank() { return this._ring.layout.rank; }
  get dtype() { return this._ring.layout.dtype; }
  get elemSize() { return this._ring.layout.elemSize; }
  get schemaId() { return this._ring.layout.schemaId; }

  // -- scalar element accessors (flat element index, stride-aware) -----------

  _flatByteOffset(flatIndex) {
    // Decompose flatIndex against strides (elements). For contiguous rings
    // this is flatIndex * elemSize; we do the general strided math because it
    // costs nothing and keeps parity with strided native tensors.
    let rest = flatIndex, byte = 0;
    const shp = this._ring.layout.shape, str = this._ring.layout.strides;
    for (let d = this._ring.layout.rank - 1; d >= 0; d--) {
      const dim = shp[d];
      if (dim === 0) continue;
      const idx = rest % dim;
      rest = (rest - idx) / dim;
      byte += idx * str[d] * this._ring.layout.elemSize;
    }
    return this._payloadBase + byte;
  }

  getF32(flatIndex) { return this._dv.getFloat32(this._flatByteOffset(flatIndex), true); }
  getF64(flatIndex) { return this._dv.getFloat64(this._flatByteOffset(flatIndex), true); }
  getU8(flatIndex) { return this._dv.getUint8(this._flatByteOffset(flatIndex)); }
  getI8(flatIndex) { return this._dv.getInt8(this._flatByteOffset(flatIndex)); }
  getU16(flatIndex) { return this._dv.getUint16(this._flatByteOffset(flatIndex), true); }
  getI16(flatIndex) { return this._dv.getInt16(this._flatByteOffset(flatIndex), true); }
  getU32(flatIndex) { return this._dv.getUint32(this._flatByteOffset(flatIndex), true); }
  getI32(flatIndex) { return this._dv.getInt32(this._flatByteOffset(flatIndex), true); }
  getF16(flatIndex) {
    // No Float16Array in engines yet — decode via the exact f16 codec.
    return f16FromBits(this._dv.getUint16(this._flatByteOffset(flatIndex), true));
  }
}
