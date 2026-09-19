/**
 * weft-wasm.ts — TypeScript bindings for the Weft C core compiled to
 * WebAssembly (issue #18-1).
 *
 * ZERO-COPY CONTRACT: the wasm linear memory IS the shared memory between C
 * and JS. Reads are typed-array VIEWS over that memory (payload(), view(),
 * ringBytes() — never copies). Writes: `frameBuffer(n)` hands JS a persistent
 * Uint32Array view inside wasm memory — fill it and publish() without any
 * copy crossing the boundary.
 *
 * u64 sequence numbers surface as BigInt (WASM BigInt — baseline in every
 * modern engine; the loader asserts it).
 *
 * GROWTH CAVEAT: with ALLOW_MEMORY_GROWTH, a wasm heap growth DETACHES all
 * previously handed-out views. The module starts at 16 MiB (comfortably
 * beyond any ring a canvas/telemetry app uses); if you allocate enough to
 * grow past it, re-fetch views (payload()/view()/ringBytesView()) after any
 * allocation that could grow the heap.
 *
 * Load: in node — `loadWeftWasm()` resolves the module relative to this
 * file; in browsers/workers — pass the URL of libweft.wasm
 * (`new WeftWasm(await initWeftWasm())` if you load the MODULARIZE glue
 * yourself). The default export handles both.
 */

export interface WeftExports {
  /* eslint-disable @typescript-eslint/naming-convention */
  _wweft_new(payload_max: number): number;
  _wweft_free(w: number): void;
  _wweft_publish(w: number, seq: number, src: number, len: number): number;
  _wweft_claim_seq(w: number): bigint;
  _wweft_payload_ptr(w: number): number;
  _wweft_payload_len(w: number): number;
  _wweft_epoch(w: number): number;
  _wweft_revoke(w: number): number;
  _wweft_reclaim(w: number, pre: number, ms: number): number;
  _wweft_t_publish(w: number): bigint;
  _wweft_t_claim(w: number): bigint;
  _wweft_t_drop(w: number): bigint;
  _wweft_t_wsteps(w: number): bigint;
  _wweft_t_rsteps(w: number): bigint;
  _wfan_new(payload_bytes: number, slot_count: number): number;
  _wfan_free(f: number): void;
  _wfan_publish(f: number, src: number, len: number): bigint;
  _wfan_publish_batch(f: number, srcs: number, lens: number, n: number): bigint;
  _wfan_ring(f: number): number;
  _wfan_ring_bytes(payload_bytes: number, slot_count: number): number;
  _wfr_new(ring: number, ring_bytes: number, pb: number, m: number): number;
  _wfr_free(r: number): void;
  _wfr_claim(r: number, seq_out: number, dropped_out: number): number;
  _wfr_view(r: number): number;
  _malloc(n: number): number;
  _free(p: number): void;
  /* eslint-enable @typescript-eslint/naming-convention */
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
}

export type WasmExports = WeftExports;

export interface WeftClaimResult {
  fresh: boolean;
  seq: bigint;
  dropped: bigint;
}

/** Load the module. `url` is the libweft.js glue (MODULARIZE build). */
export async function loadWeftWasm(url?: string): Promise<WeftWasm> {
  let init: (opts?: unknown) => Promise<WeftExports>;
  let glue: string;
  if (url) {
    glue = url;
  } else {
    // node / bundler default: sibling dist/libweft.js
    const path = await import("node:path");
    const { pathToFileURL } = await import("node:url");
    const here = typeof __dirname !== "undefined"
      ? __dirname
      : path.dirname(process.argv[1] ?? ".");
    glue = pathToFileURL(
      path.resolve(here, "..", "dist", "libweft.js")).href;
  }
  const imported = await import(/* webpackIgnore: true */ glue);
  const candidate = imported.default ?? imported.initWeftWasm ?? imported;
  if (typeof candidate !== "function") {
    throw new Error(`libweft glue at ${glue} did not export the MODULARIZE ` +
      "init function (build mismatch?)");
  }
  init = candidate as (opts?: unknown) => Promise<WeftExports>;
  const mod = (await init()) as unknown as WeftExports;
  if (typeof mod._wweft_new !== "function") {
    throw new Error("libweft.wasm loaded but exports missing (build mismatch?)");
  }
  return new WeftWasm(mod);
}

export class WeftWasm {
  private readonly m: WeftExports;
  constructor(m: WeftExports) { this.m = m; }

  /** The Triad kernel (1 writer, 1 reader, 3 buffers, one atomic). */
  kernel(payloadMax: number): WasmKernel {
    const w = this.m._wweft_new(payloadMax);
    if (!w) throw new Error(`wweft_new(${payloadMax}) failed`);
    return new WasmKernel(this.m, w);
  }

  /** The RFC-0004 fan-out ring (1 writer, N readers, M slots). */
  fanout(payloadBytes: number, slotCount = 4): WasmFanout {
    const f = this.m._wfan_new(payloadBytes, slotCount);
    if (!f) throw new Error(`wfan_new(${payloadBytes}, ${slotCount}) failed`);
    return new WasmFanout(this.m, f, payloadBytes, slotCount);
  }

  /** Allocate a persistent buffer inside wasm memory (zero-copy writes). */
  frameBuffer(bytes: number): Uint8Array {
    const p = this.m._malloc(bytes);
    if (!p) throw new Error(`malloc(${bytes}) in wasm memory failed`);
    return new Uint8Array(this.m.HEAPU8.buffer, p, bytes);
  }

  /** Free a frameBuffer's underlying allocation (the view must not be
   * used afterwards). */
  freeBuffer(view: Uint8Array): void {
    this.m._free(view.byteOffset);
  }
}

export class WasmKernel {
  private readonly m: WeftExports;
  private readonly w: number;
  constructor(m: WeftExports, w: number) { this.m = m; this.w = w; }

  publish(seq: number, payload: Uint8Array | Uint32Array): boolean {
    const u8 = payload instanceof Uint8Array
      ? payload : new Uint8Array(payload.buffer, payload.byteOffset, payload.byteLength);
    // Fast path: payload already lives in wasm memory.
    const inWasm = this.inWasmMemory(u8);
    const src = inWasm ? u8.byteOffset : this.copyIn(u8);
    const rc = this.m._wweft_publish(this.w, seq, src, u8.length);
    if (!inWasm) this.m._free(src);
    return rc === 0;
  }

  claim(): bigint {
    return this.m._wweft_claim_seq(this.w);
  }

  /** Zero-copy view of the claimed frame's payload (valid until the next
   * two publishes — the Triad guarantee). */
  payload(): Uint8Array {
    const ptr = this.m._wweft_payload_ptr(this.w);
    const len = this.m._wweft_payload_len(this.w);
    return new Uint8Array(this.m.HEAPU8.buffer, ptr, len);
  }

  revoke(): number {
    return this.m._wweft_revoke(this.w);
  }

  reclaim(preRevokeEpoch: number, timeoutMs = 1000): boolean {
    return this.m._wweft_reclaim(this.w, preRevokeEpoch, timeoutMs) === 0;
  }

  telemetry(): Record<string, bigint> {
    const m = this.m;
    return {
      publish: m._wweft_t_publish(this.w),
      claim: m._wweft_t_claim(this.w),
      drop: m._wweft_t_drop(this.w),
      wsteps: m._wweft_t_wsteps(this.w),
      rsteps: m._wweft_t_rsteps(this.w),
    };
  }

  destroy(): void {
    this.m._wweft_free(this.w);
  }

  private inWasmMemory(u8: Uint8Array): boolean {
    const buf = this.m.HEAPU8.buffer as ArrayBuffer;
    return u8.buffer === buf;
  }

  private copyIn(src: Uint8Array): number {
    const p = this.m._malloc(src.length);
    this.m.HEAPU8.set(src, p);
    return p;
  }
}

export class WasmFanout {
  private readonly m: WeftExports;
  private readonly f: number;
  private readonly ringPtr: number;
  readonly ringBytes: number;
  readonly payloadBytes: number;
  readonly slotCount: number;

  constructor(m: WeftExports, f: number,
              payloadBytes: number, slotCount: number) {
    this.m = m; this.f = f;
    this.payloadBytes = payloadBytes;
    this.slotCount = slotCount;
    this.ringPtr = m._wfan_ring(f);
    this.ringBytes = m._wfan_ring_bytes(payloadBytes, slotCount);
  }

  publish(payload: Uint8Array | Uint32Array): bigint {
    const u8 = payload instanceof Uint8Array
      ? payload : new Uint8Array(payload.buffer, payload.byteOffset, payload.byteLength);
    const inWasm = u8.buffer === (this.m.HEAPU8.buffer as ArrayBuffer);
    const src = inWasm ? u8.byteOffset : this.m._malloc(u8.length);
    if (!inWasm) this.m.HEAPU8.set(u8, src);
    const seq = this.m._wfan_publish(this.f, src, u8.length);
    if (!inWasm) this.m._free(src);
    return seq;
  }

  /** Single-flip batch publish (issue #17-3). Frames are copied into a
   * scratch region of wasm memory once (they must live somewhere stable
   * through the batch), then published as one unit. */
  publishBatch(frames: Array<Uint8Array | Uint32Array>): bigint {
    const n = frames.length;
    if (n === 0) return 0n;
    const m = this.m;
    const words = frames.map((fr) => fr instanceof Uint8Array
      ? new Uint32Array(fr.buffer, fr.byteOffset, fr.byteLength >>> 2)
      : fr);
    // one contiguous block: [srcs[n]][lens[n]][payload bytes...]
    let totalBytes = 0;
    for (const w of words) totalBytes += w.byteLength;
    const srcsPtr = m._malloc(n * 4);
    const lensPtr = m._malloc(n * 8);
    const bodyPtr = m._malloc(totalBytes || 4);
    let off = bodyPtr;
    const HEAPU32 = new Uint32Array(m.HEAPU8.buffer, srcsPtr, n);
    const HEAPU64 = new BigUint64Array(m.HEAPU8.buffer, lensPtr, n);
    for (let i = 0; i < n; i++) {
      const w = words[i];
      new Uint8Array(m.HEAPU8.buffer, off, w.byteLength).set(
        new Uint8Array(w.buffer, w.byteOffset, w.byteLength));
      HEAPU32[i] = off;
      HEAPU64[i] = BigInt(w.byteLength);
      off += w.byteLength;
    }
    const last = m._wfan_publish_batch(this.f, srcsPtr, lensPtr, n);
    m._free(srcsPtr);
    m._free(lensPtr);
    m._free(bodyPtr);
    return last;
  }

  /** Zero-copy view of the WHOLE ring (the byte-layout contract: latestSeq
   * u64 @0, publishes u64 @8, slotSeq[k] u64 @16+8k, then M slots). */
  ringBytesView(): Uint8Array {
    return new Uint8Array(this.m.HEAPU8.buffer, this.ringPtr, this.ringBytes);
  }

  /** BigUint64 view of the ring's control block (stamps, cross-port checks). */
  ctrlView(): BigUint64Array {
    const ctrlWords = (16 + 8 * this.slotCount) / 8;
    return new BigUint64Array(this.m.HEAPU8.buffer, this.ringPtr, ctrlWords);
  }

  reader(): WasmFanoutReader {
    const r = this.m._wfr_new(this.ringPtr, this.ringBytes,
                              this.payloadBytes, this.slotCount);
    if (!r) throw new Error("wfr_new failed");
    return new WasmFanoutReader(this.m, r, this.payloadBytes);
  }

  destroy(): void {
    this.m._wfan_free(this.f);
  }
}

export class WasmFanoutReader {
  private readonly m: WeftExports;
  private readonly r: number;
  readonly payloadBytes: number;
  private readonly seqSlot: number;
  private readonly droppedSlot: number;

  constructor(m: WeftExports, r: number, payloadBytes: number) {
    this.m = m; this.r = r; this.payloadBytes = payloadBytes;
    this.seqSlot = m._malloc(8);
    this.droppedSlot = m._malloc(8);
  }

  claim(): WeftClaimResult {
    const fresh = this.m._wfr_claim(this.r, this.seqSlot, this.droppedSlot) === 1;
    const seq = new BigUint64Array(this.m.HEAPU8.buffer, this.seqSlot, 1)[0];
    const dropped = new BigUint64Array(this.m.HEAPU8.buffer, this.droppedSlot, 1)[0];
    return { fresh, seq, dropped };
  }

  /** Zero-copy view of the reader's copy buffer (payload_bytes wide). */
  view(): Uint32Array {
    const p = this.m._wfr_view(this.r);
    return new Uint32Array(this.m.HEAPU8.buffer, p, this.payloadBytes >>> 2);
  }

  destroy(): void {
    this.m._free(this.seqSlot);
    this.m._free(this.droppedSlot);
    this.m._wfr_free(this.r);
  }
}
