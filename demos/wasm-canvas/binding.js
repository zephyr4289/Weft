/**
 * binding.js — plain-JS twin of core/wasm/src/weft-wasm.ts for the
 * collaborative-canvas demo (browsers don't execute TypeScript; the full
 * typed binding lives in core/wasm and is what node/bundler consumers use).
 * Same zero-copy contract: views over wasm linear memory, no copies.
 */
export async function loadWeftWasm(glueUrl) {
  const mod = await import(glueUrl);
  const init = mod.default ?? mod.initWeftWasm ?? mod;
  const m = await init();
  if (typeof m._wfan_new !== "function") throw new Error("libweft exports missing");
  return {
    fanout(payloadBytes, slotCount) { return new Fanout(m, payloadBytes, slotCount); },
    frameBuffer(bytes) {
      const p = m._malloc(bytes);
      return new Uint8Array(m.HEAPU8.buffer, p, bytes);
    },
    freeBuffer(view) { m._free(view.byteOffset); },
  };
}

class Fanout {
  constructor(m, payloadBytes, slotCount) {
    this.m = m; this.payloadBytes = payloadBytes; this.slotCount = slotCount;
    this.f = m._wfan_new(payloadBytes, slotCount);
    if (!this.f) throw new Error("wfan_new failed");
    this.ringPtr = m._wfan_ring(this.f);
    this.ringBytes = m._wfan_ring_bytes(payloadBytes, slotCount);
  }
  publish(payload) {   // payload: Uint8Array view inside wasm memory
    return this.m._wfan_publish(this.f, payload.byteOffset, payload.length);
  }
  reader() { return new Reader(this.m, this, this.payloadBytes); }
}

class Reader {
  constructor(m, fan, payloadBytes) {
    this.m = m; this.payloadBytes = payloadBytes;
    this.r = m._wfr_new(fan.ringPtr, fan.ringBytes, payloadBytes, fan.slotCount);
    if (!this.r) throw new Error("wfr_new failed");
    this.seqSlot = m._malloc(8);
    this.dropSlot = m._malloc(8);
  }
  claim() {
    const fresh = this.m._wfr_claim(this.r, this.seqSlot, this.dropSlot) === 1;
    const seq = new BigUint64Array(this.m.HEAPU8.buffer, this.seqSlot, 1)[0];
    const dropped = new BigUint64Array(this.m.HEAPU8.buffer, this.dropSlot, 1)[0];
    return { fresh, seq, dropped };
  }
  view() {
    const p = this.m._wfr_view(this.r);
    return new Uint32Array(this.m.HEAPU8.buffer, p, this.payloadBytes >>> 2);
  }
}
