// verified.ts — RFC 0005: VerifiedWeft authenticated frames (driver layer).
//
// WHY EXISTS: RFC 0005 proposes optional HMAC-SHA256 authentication for
// frame records crossing untrusted boundaries (WebSocket bridge, multi-
// tenant IPC, cross-origin SAB streams). This module implements the RFC's
// reference format byte-compatibly with core/c/verified.c and
// core/rust/src/verified.rs: same wire layout, same key schedule, same
// tags (V-series conformance, shared fixture in
// fixtures/xlang-verifiedweft/hmac-vectors.json).
//
// ZERO DEPENDENCIES, ZERO HOT-PATH ALLOCATION: SHA-256 state, key pads,
// and the message schedule are preallocated. Law 2 applies to the steady
// state: sign()/verify() through VerifiedWeftSigner allocate nothing.
//
// WIRE FORMAT (RFC 0005 "extended record"):
//   [0..16)                     frame envelope v1
//   [16..16+payload_len)        payload
//   [16+payload_len..+32)       HMAC-SHA256 tag
// KEY SCHEDULE (domain separation):
//   auth_key = HMAC-SHA256(secret, "Weft-VerifiedWeft-v1:key")
//   tag      = HMAC-SHA256(auth_key, envelope[0..16] || payload)
//
// GUARANTEE BOUNDARY (Law 4): integrity + authenticity of frame contents.
// Does not prevent DoS; a frame that fails verification MUST be dropped
// and counted, never consumed.

export const SHA256_DIGEST_LEN = 32;
export const SHA256_BLOCK_LEN = 64;
export const HMAC_TAG_LEN = 32;
export const VW_KEY_LEN = 32;
export const VW_ENVELOPE_LEN = 16;

/** Verify result codes — numeric parity with weft_vw_result_t (core/c). */
export const VW_OK = 0;
export const VW_ERR_SHORT = 1;
export const VW_ERR_BAD_MAGIC = 2;
export const VW_ERR_TAG = 3;

const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
  0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
  0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
  0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
  0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
  0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
  0xc67178f2,
]);

const H0 = new Uint32Array([
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
  0x5be0cd19,
]);

function rotr(x: number, n: number): number {
  return ((x >>> n) | (x << (32 - n))) >>> 0;
}

/** Streaming SHA-256 (FIPS 180-4). Preallocated; reuse via init(). */
export class Sha256 {
  private h = new Uint32Array(8);
  private totalLen = 0;
  private block = new Uint8Array(SHA256_BLOCK_LEN);
  private fill = 0;
  private w = new Uint32Array(64);

  constructor() {
    this.h.set(H0);
  }

  /** Reset to the initial state (no allocation). */
  init(): this {
    this.h.set(H0);
    this.totalLen = 0;
    this.fill = 0;
    return this;
  }

  /** Snapshot-copy this state into `dst` (both preallocated). */
  copyInto(dst: Sha256): void {
    dst.h.set(this.h);
    dst.totalLen = this.totalLen;
    dst.block.set(this.block);
    dst.fill = this.fill;
  }

  /** Feed message bytes. */
  update(data: Uint8Array, start = 0, end = data.length): this {
    let len = end - start;
    this.totalLen += len;
    if (this.fill > 0) {
      const take = Math.min(SHA256_BLOCK_LEN - this.fill, len);
      this.block.set(data.subarray(start, start + take), this.fill);
      this.fill += take;
      start += take;
      len -= take;
      if (this.fill === SHA256_BLOCK_LEN) {
        this.compress(this.block);
        this.fill = 0;
      }
    }
    while (len >= SHA256_BLOCK_LEN) {
      this.compress(data.subarray(start, start + SHA256_BLOCK_LEN));
      start += SHA256_BLOCK_LEN;
      len -= SHA256_BLOCK_LEN;
    }
    if (len > 0) {
      this.block.set(data.subarray(start, start + len), this.fill);
      this.fill = len;
    }
    return this;
  }

  /** Finalize into out[outOff..outOff+32); state resets for reuse. */
  finalize(out: Uint8Array, outOff = 0): void {
    const bitLen = this.totalLen * 8;
    // Append 0x80, zeros to fill==56 (compressing across the boundary if
    // needed), then the 8-byte big-endian bit length. Length bytes are not
    // counted into totalLen (already captured above).
    if (this.fill > 55) {
      this.block[this.fill] = 0x80;
      this.block.fill(0, this.fill + 1, SHA256_BLOCK_LEN);
      this.compress(this.block);
      this.block.fill(0, 0, 56);
    } else {
      this.block[this.fill] = 0x80;
      this.block.fill(0, this.fill + 1, 56);
    }
    this.fill = 56;
    const hi = Math.floor(bitLen / 0x100000000);
    const lo = bitLen >>> 0;
    this.block[56] = (hi >>> 24) & 0xff;
    this.block[57] = (hi >>> 16) & 0xff;
    this.block[58] = (hi >>> 8) & 0xff;
    this.block[59] = hi & 0xff;
    this.block[60] = (lo >>> 24) & 0xff;
    this.block[61] = (lo >>> 16) & 0xff;
    this.block[62] = (lo >>> 8) & 0xff;
    this.block[63] = lo & 0xff;
    this.compress(this.block);
    this.fill = 0;

    for (let i = 0; i < 8; i++) {
      out[outOff + i * 4] = (this.h[i] >>> 24) & 0xff;
      out[outOff + i * 4 + 1] = (this.h[i] >>> 16) & 0xff;
      out[outOff + i * 4 + 2] = (this.h[i] >>> 8) & 0xff;
      out[outOff + i * 4 + 3] = this.h[i] & 0xff;
    }
    this.init();
  }

  private compress(block: Uint8Array): void {
    const w = this.w;
    for (let i = 0; i < 16; i++) {
      const j = i * 4;
      w[i] = ((block[j] << 24) | (block[j + 1] << 16) | (block[j + 2] << 8) | block[j + 3]) >>> 0;
    }
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15];
      const b = w[i - 2];
      const s0 = (rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3)) >>> 0;
      const s1 = (rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10)) >>> 0;
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let a = this.h[0], b = this.h[1], c = this.h[2], d = this.h[3];
    let e = this.h[4], f = this.h[5], g = this.h[6], h = this.h[7];
    for (let i = 0; i < 64; i++) {
      const S1 = (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) >>> 0;
      const ch = ((e & f) ^ (~e & g)) >>> 0;
      const t1 = (h + S1 + ch + K[i] + w[i]) >>> 0;
      const S0 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) >>> 0;
      const maj = ((a & b) ^ (a & c) ^ (b & c)) >>> 0;
      const t2 = (S0 + maj) >>> 0;
      h = g; g = f; f = e;
      e = (d + t1) >>> 0;
      d = c; c = b; b = a;
      a = (t1 + t2) >>> 0;
    }
    this.h[0] = (this.h[0] + a) >>> 0;
    this.h[1] = (this.h[1] + b) >>> 0;
    this.h[2] = (this.h[2] + c) >>> 0;
    this.h[3] = (this.h[3] + d) >>> 0;
    this.h[4] = (this.h[4] + e) >>> 0;
    this.h[5] = (this.h[5] + f) >>> 0;
    this.h[6] = (this.h[6] + g) >>> 0;
    this.h[7] = (this.h[7] + h) >>> 0;
  }
}

/** One-shot SHA-256 (allocates its output — caller's choice, not hot path). */
export function sha256(data: Uint8Array): Uint8Array {
  const out = new Uint8Array(SHA256_DIGEST_LEN);
  new Sha256().update(data).finalize(out);
  return out;
}

/** Constant-time equality — timing independent of mismatch position. */
export function ctEq(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

const VW_DOMAIN = new TextEncoder().encode('Weft-VerifiedWeft-v1:key');

/** One-shot HMAC-SHA256 (allocates output; conformance and setup use only). */
export function hmacSha256(key: Uint8Array, data: Uint8Array): Uint8Array {
  const tag = new Uint8Array(HMAC_TAG_LEN);
  const s = new VerifiedWeftSigner(key);
  s.update(data);
  s.finalize(tag);
  return tag;
}

/** One-time auth-key derivation with domain separation. */
export function deriveKey(secret: Uint8Array): Uint8Array {
  return hmacSha256(secret, VW_DOMAIN);
}

/**
 * Reusable HMAC-SHA256 signer: ipad pre-fed into `inner`, opad pre-fed into
 * `outer`. Each message costs two compressions beyond payload blocks and
 * ZERO allocations (all state preallocated; templates restore the pads).
 * Serves both the per-frame signer (RFC 0005 hot path) and one-shot HMAC.
 */
export class VerifiedWeftSigner {
  private inner = new Sha256();
  private innerTemplate = new Sha256();
  private outer = new Sha256();
  private opad = new Uint8Array(SHA256_BLOCK_LEN);
  private innerDigest = new Uint8Array(SHA256_DIGEST_LEN);

  constructor(key: Uint8Array) {
    const keyBlock = new Uint8Array(SHA256_BLOCK_LEN);
    if (key.length > SHA256_BLOCK_LEN) {
      keyBlock.set(sha256(key));
    } else {
      keyBlock.set(key);
    }
    const ipad = new Uint8Array(SHA256_BLOCK_LEN);
    for (let i = 0; i < SHA256_BLOCK_LEN; i++) {
      ipad[i] = keyBlock[i] ^ 0x36;
      this.opad[i] = keyBlock[i] ^ 0x5c;
    }
    this.inner.update(ipad);
    this.inner.copyInto(this.innerTemplate);
    this.outer.update(this.opad);
  }

  /** Feed message bytes. */
  update(data: Uint8Array, start = 0, end = data.length): this {
    this.inner.update(data, start, end);
    return this;
  }

  /** Finalize into out[0..32); ipad/opad states restore for the next message. */
  finalize(out: Uint8Array): void {
    this.inner.finalize(this.innerDigest);
    this.outer.update(this.innerDigest);
    this.outer.finalize(out);
    this.innerTemplate.copyInto(this.inner); // restore ipad pre-feed
    this.outer.update(this.opad); // outer finalized -> re-feed opad
  }
}

/** Verify one frame record's fields. Constant-time tag compare. */
export function verifiedWeftVerify(
  authKey: Uint8Array,
  envelope: Uint8Array,
  payload: Uint8Array,
  tag: Uint8Array,
): number {
  const s = new VerifiedWeftSigner(authKey);
  const expect = new Uint8Array(HMAC_TAG_LEN);
  s.update(envelope, 0, VW_ENVELOPE_LEN);
  s.update(payload);
  s.finalize(expect);
  if (!ctEq(expect, tag)) return VW_ERR_TAG;
  return VW_OK;
}

/** Encode envelope v1 (magic "WEFT", version 1, header_size 16, seq, payload_len — all LE). */
export function weftEnvelopeEncodeV1(dst: Uint8Array, seq: number, payloadLen: number): void {
  const dv = new DataView(dst.buffer, dst.byteOffset, dst.byteLength);
  dst[0] = 0x57; dst[1] = 0x45; dst[2] = 0x46; dst[3] = 0x54; // "WEFT"
  dv.setUint16(4, 1, true);  // version
  dv.setUint16(6, 16, true); // header_size
  dv.setUint32(8, seq >>> 0, true);
  dv.setUint32(12, payloadLen >>> 0, true);
}

/** Encode a full auth record into dst. Returns bytes written, or 0 if too small. */
export function verifiedWeftRecordEncode(
  envelope: Uint8Array,
  payload: Uint8Array,
  tag: Uint8Array,
  dst: Uint8Array,
): number {
  const total = VW_ENVELOPE_LEN + payload.length + HMAC_TAG_LEN;
  if (dst.length < total || envelope.length < VW_ENVELOPE_LEN) return 0;
  dst.set(envelope.subarray(0, VW_ENVELOPE_LEN), 0);
  dst.set(payload, VW_ENVELOPE_LEN);
  dst.set(tag, VW_ENVELOPE_LEN + payload.length);
  return total;
}

/** Decode + verify a record. Returns zero-copy views into src on VW_OK. */
export function verifiedWeftRecordDecodeVerify(
  authKey: Uint8Array,
  src: Uint8Array,
): { envelope: Uint8Array; payload: Uint8Array; code: number } {
  const fail = (code: number) => ({
    envelope: new Uint8Array(0),
    payload: new Uint8Array(0),
    code,
  });
  if (src.length < VW_ENVELOPE_LEN + HMAC_TAG_LEN) return fail(VW_ERR_SHORT);
  if (!(src[0] === 0x57 && src[1] === 0x45 && src[2] === 0x46 && src[3] === 0x54)) {
    return fail(VW_ERR_BAD_MAGIC);
  }
  const dv = new DataView(src.buffer, src.byteOffset, src.byteLength);
  const headerSize = dv.getUint16(6, true);
  const plen = dv.getUint32(12, true);
  if (headerSize < VW_ENVELOPE_LEN) return fail(VW_ERR_BAD_MAGIC);
  const body = headerSize + plen;
  if (body > src.length - HMAC_TAG_LEN) return fail(VW_ERR_SHORT);

  const code = verifiedWeftVerify(
    authKey,
    src.subarray(0, VW_ENVELOPE_LEN),
    src.subarray(headerSize, body),
    src.subarray(body, body + HMAC_TAG_LEN),
  );
  if (code !== VW_OK) return fail(code);
  return {
    envelope: src.subarray(0, VW_ENVELOPE_LEN),
    payload: src.subarray(headerSize, body),
    code: VW_OK,
  };
}

// ---------------------------------------------------------------------------
// Series 6: pre-keyed stream verifier + batch decode-verify + optional
// native accelerator. Mirrors core/c/verified.{h,c} and
// core/rust/src/verified.rs — same codes, same stop semantics, same Law 4
// drop-and-count contract (a bad record is dropped and counted, never
// consumed).
// ---------------------------------------------------------------------------

/** Pre-keyed verifier: one HMAC key schedule amortized across a stream. */
export class VerifiedWeftVerifier {
  private signer: VerifiedWeftSigner;
  private expect = new Uint8Array(HMAC_TAG_LEN);

  constructor(authKey: Uint8Array) {
    this.signer = new VerifiedWeftSigner(authKey);
  }

  /** Verify one frame. Same codes as verifiedWeftVerify; ctEq compare. */
  verify(envelope: Uint8Array, payload: Uint8Array, tag: Uint8Array): number {
    this.signer.update(envelope, 0, VW_ENVELOPE_LEN);
    this.signer.update(payload);
    this.signer.finalize(this.expect);
    if (!ctEq(this.expect, tag)) return VW_ERR_TAG;
    return VW_OK;
  }
}

/** Zero-copy view of one verified record inside a batch buffer. */
export interface VerifiedWeftRecordView {
  /** 16-byte envelope prefix (view into the batch buffer). */
  envelope: Uint8Array;
  /** Payload bytes (view into the batch buffer). */
  payload: Uint8Array;
  /** Envelope seq field, decoded little-endian. */
  seq: number;
}

/** Stream-consumer batch result — mirrors the C/Rust batch APIs. */
export interface VerifiedWeftBatchResult {
  /** VW_OK, or the code of the FIRST bad record. */
  code: number;
  /** Count of verified records (the good prefix). */
  verified: number;
  /** End offset of the last VERIFIED record (resync point). */
  bytesConsumed: number;
  /** Zero-copy views of verified records (bounded by maxViews). */
  records: VerifiedWeftRecordView[];
}

/**
 * Walk a buffer of concatenated auth records, verifying each in order.
 * Stops at the first bad record (code + good-prefix count + its offset);
 * trailing bytes shorter than a record are ignored — the truncation policy
 * belongs to the caller, same contract as C/Rust. maxViews bounds the
 * returned views (verification itself always covers the whole prefix).
 */
export function verifiedWeftBatchDecodeVerify(
  authKey: Uint8Array,
  src: Uint8Array,
  maxViews = Number.POSITIVE_INFINITY,
): VerifiedWeftBatchResult {
  const verifier = new VerifiedWeftVerifier(authKey);
  const records: VerifiedWeftRecordView[] = [];
  let verified = 0;
  let off = 0;

  while (off + VW_ENVELOPE_LEN + HMAC_TAG_LEN <= src.length) {
    if (!(src[off] === 0x57 && src[off + 1] === 0x45 && src[off + 2] === 0x46 && src[off + 3] === 0x54)) {
      return { code: VW_ERR_BAD_MAGIC, verified, bytesConsumed: off, records };
    }
    const headerSize = src[off + 6] | (src[off + 7] << 8);
    const plen =
      (src[off + 12] | (src[off + 13] << 8) | (src[off + 14] << 16) | (src[off + 15] << 24)) >>> 0;
    if (headerSize < VW_ENVELOPE_LEN) {
      return { code: VW_ERR_BAD_MAGIC, verified, bytesConsumed: off, records };
    }
    const body = headerSize + plen;
    if (body > src.length - off - HMAC_TAG_LEN) {
      return { code: VW_ERR_SHORT, verified, bytesConsumed: off, records };
    }

    const code = verifier.verify(
      src.subarray(off, off + VW_ENVELOPE_LEN),
      src.subarray(off + headerSize, off + body),
      src.subarray(off + body, off + body + HMAC_TAG_LEN),
    );
    if (code !== VW_OK) {
      return { code, verified, bytesConsumed: off, records };
    }

    if (verified < maxViews) {
      records.push({
        envelope: src.subarray(off, off + VW_ENVELOPE_LEN),
        payload: src.subarray(off + headerSize, off + body),
        seq: (src[off + 8] | (src[off + 9] << 8) | (src[off + 10] << 16) | (src[off + 11] << 24)) >>> 0,
      });
    }
    verified++;
    off += body + HMAC_TAG_LEN;
  }

  return { code: VW_OK, verified, bytesConsumed: off, records };
}

/**
 * OPTIONAL native accelerator for bulk stream verification (Series 6).
 *
 * WHEN: browsers, Node >= 19, Deno, workers — anywhere globalThis.crypto
 * exposes subtle. The platform's HMAC runs on hardware SHA (SHA-NI / ARMv8
 * CE) — the same physical acceleration the C port dispatches to via
 * intrinsics; JS has no intrinsics, so the platform is the honest road.
 *
 * WHAT IT IS NOT: not a replacement for the sync pure-TS reference above.
 * The sync path stays normative; this path must agree with it bit-exactly
 * (the test suite runs both when WebCrypto is available and compares).
 * Returns null when WebCrypto is unavailable — callers fall back to
 * verifiedWeftBatchDecodeVerify, never the other way around.
 *
 * Stop semantics: records are verified in parallel chunks, then the FIRST
 * failure is reported in the same result shape (code/verified/
 * bytesConsumed) — an accelerator for bulk ingestion, not a streaming API.
 */
export async function verifiedWeftBatchVerifyNative(
  authKey: Uint8Array,
  src: Uint8Array,
): Promise<VerifiedWeftBatchResult | null> {
  const subtle = (globalThis as { crypto?: { subtle?: SubtleCrypto } }).crypto?.subtle;
  if (!subtle) return null;

  // Geometry walk first (sync, cheap): record boundaries + contiguous
  // envelope||payload views (header_size 16 is the v1 standard; other sizes
  // get a one-off concat buffer).
  interface Pending {
    off: number;
    body: number;
    data: Uint8Array;
    tag: Uint8Array;
    headerSize: number;
    plen: number;
  }
  const pending: Pending[] = [];
  let off = 0;
  while (off + VW_ENVELOPE_LEN + HMAC_TAG_LEN <= src.length) {
    if (!(src[off] === 0x57 && src[off + 1] === 0x45 && src[off + 2] === 0x46 && src[off + 3] === 0x54)) {
      return { code: VW_ERR_BAD_MAGIC, verified: 0, bytesConsumed: 0, records: [] };
    }
    const headerSize = src[off + 6] | (src[off + 7] << 8);
    const plen =
      (src[off + 12] | (src[off + 13] << 8) | (src[off + 14] << 16) | (src[off + 15] << 24)) >>> 0;
    if (headerSize < VW_ENVELOPE_LEN) {
      return { code: VW_ERR_BAD_MAGIC, verified: 0, bytesConsumed: 0, records: [] };
    }
    const body = headerSize + plen;
    if (body > src.length - off - HMAC_TAG_LEN) {
      return { code: VW_ERR_SHORT, verified: 0, bytesConsumed: 0, records: [] };
    }
    let data: Uint8Array;
    if (headerSize === VW_ENVELOPE_LEN) {
      data = src.subarray(off, off + body); // contiguous: zero-copy
    } else {
      data = new Uint8Array(body);
      data.set(src.subarray(off, off + VW_ENVELOPE_LEN), 0);
      data.set(src.subarray(off + headerSize, off + body), VW_ENVELOPE_LEN);
    }
    pending.push({
      off,
      body,
      data,
      tag: src.subarray(off + body, off + body + HMAC_TAG_LEN),
      headerSize,
      plen,
    });
    off += body + HMAC_TAG_LEN;
  }

  try {
    const key = await subtle.importKey(
      'raw',
      authKey as unknown as BufferSource,
      { name: 'HMAC', hash: 'SHA-256' },
      false,
      ['verify'],
    );
    const CHUNK = 256;
    for (let i = 0; i < pending.length; i += CHUNK) {
      const chunk = pending.slice(i, i + CHUNK);
      const oks = await Promise.all(
        chunk.map((p) => subtle.verify({ name: 'HMAC' }, key, p.tag as unknown as BufferSource,
                                        p.data as unknown as BufferSource)),
      );
      for (let j = 0; j < chunk.length; j++) {
        if (!oks[j]) {
          const firstBad = chunk[j];
          return {
            code: VW_ERR_TAG,
            verified: i + j,
            bytesConsumed: firstBad.off,
            records: [],
          };
        }
      }
    }
  } catch {
    return null; // platform refused (e.g. non-extractable key material path)
  }

  return {
    code: VW_OK,
    verified: pending.length,
    bytesConsumed: off,
    records: [], // accelerator: no views; the sync path is the view API
  };
}
