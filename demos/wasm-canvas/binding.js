/**
 * binding.js — plain-JS twin of core/wasm/src/weft-wasm.ts for the
 * collaborative-canvas demo (browsers don't execute TypeScript; the full
 * typed binding lives in core/wasm and is what node/bundler consumers use).
 * Same zero-copy contract: views over wasm linear memory, no copies.
 */
export async function loadWeftWasm(glueUrl) {
  const mod = await import(glueUrl);
  const init = mod.default ?? mod.initWeftWasm ?? mod;
  if (typeof init !== "function") throw new Error("init function missing");
  const distUrl = new URL("./", new URL(glueUrl, location.href));
  const m = await init({
    locateFile: (file) => new URL(file, distUrl).href,
  });
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
    const s = this.m._wfan_publish(this.f, payload.byteOffset, payload.length);
    return typeof s === "bigint" ? s : BigInt(s || 0);
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
    const dv = new DataView(this.m.HEAPU8.buffer);
    const seq = dv.getBigUint64(this.seqSlot, true);
    const dropped = dv.getBigUint64(this.dropSlot, true);
    return { fresh, seq, dropped };
  }
  view() {
    const p = this.m._wfr_view(this.r);
    return new Uint32Array(this.m.HEAPU8.buffer, p, this.payloadBytes >>> 2);
  }
}
