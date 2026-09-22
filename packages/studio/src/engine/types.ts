/**
 * Weft Studio — shared types, constants, error taxonomy, deterministic hashing.
 * Zero dependencies. STUDIO-SEAMS-V1 §7 Law-4 taxonomy.
 */

export const SLOT_SIZE = 32;
export const DEFAULT_RING_CAPACITY = 1_000_000;
export const CACHE_LINE = 64;
export const CACHE_LINE_ALT = 128;
export const FRAME_BUDGET_NS_240 = 4_166_666; // 240 Hz budget in ns
export const CADENCE_240 = 240;
export const CADENCE_120 = 120;

export const SREC_MAGIC = 0x53524543; // "SREC"
export const SREC_VERSION = 1;
export const SREC_RECORD_SIZE = 40;
export const SREC_HEADER_SIZE = 32;
export const SBURST_MAGIC = 0x53425553; // "SBUS" little-endian read "SBUS"... kept as spec'd "SBURST" tag constant
export const SBURST_TAG0 = 0x53; // 'S'
export const CHECKPOINT_INTERVAL = 4096;

/** Law-4 studio error taxonomy (STUDIO-SEAMS-V1 §7). */
export const E = {
  SCHEMA: 'E_SCHEMA',
  CAPACITY: 'E_CAPACITY',
  TORN: 'E_TORN',
  DROPPED: 'E_DROPPED',
  SEAM: 'E_SEAM',
  EXPORT: 'E_EXPORT',
} as const;
export type WeftErrorCode = (typeof E)[keyof typeof E];

export interface WeftError {
  code: WeftErrorCode;
  message: string;
}

export function weftError(code: WeftErrorCode, message: string): WeftError {
  return { code, message };
}

/** FNV-1a 32 — exact 32-bit arithmetic (mul partial products fit 2^53). */
export function fnv1a32(data: Uint8Array | string, seed = 0x811c9dc5): number {
  let h = seed >>> 0;
  const n = data.length;
  for (let i = 0; i < n; i++) {
    const b = (typeof data === 'string' ? data.charCodeAt(i) : data[i]) & 0xff;
    h = (h ^ b) >>> 0;
    // h *= 16777619 mod 2^32: split h into 16-bit lanes (products <= 2^40, exact in f64)
    const low = h & 0xffff;
    const high = h >>> 16;
    h = ((low * 16777619) + ((high * 16777619) % 65536) * 65536) >>> 0;
  }
  return h >>> 0;
}

/**
 * FNV-1a 64 — two 32-bit lanes (double-arithmetic pairing, no BigInt).
 * lane lo = FNV-1a-32(data); lane hi = FNV-1a-32(data re-folded through lo).
 * Deterministic across languages by construction; pinned by golden fixtures.
 */
export function fnv1a64(data: Uint8Array | string): string {
  const lo = fnv1a32(data);
  const hi = fnv1a32(data, lo ^ 0x9dc38393);
  return hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
}

const CRC32_TABLE = (() => {
  const t = new Int32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c | 0;
  }
  return t;
})();

export function crc32(bytes: Uint8Array, from = 0, to = bytes.length): number {
  let c = 0xffffffff | 0;
  for (let i = from; i < to; i++) c = CRC32_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export function hex8(v: number): string {
  return (v >>> 0).toString(16).padStart(8, '0');
}

/** Deterministic clamp. */
export function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}
