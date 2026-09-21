// src/wire.js — SHP1 zero-copy decoder for weft_hw_profile_t (Pillar 5).
//
// Normative layout: docs/spectrum/SPECTRUM-WIRE-V1.md (byte-frozen).
// Law 1: ProfileView is constructed ONCE over an existing buffer; every
// getter returns a primitive read in place — the steady-state telemetry loop
// allocates NOTHING. snapshotInto() copies primitives into a caller-owned
// flyweight (also allocation-free).
// Law 2: all multi-byte reads are explicit little-endian DataView ops at
// frozen offsets. Law 4: integrity violations surface as integer codes
// (§6 taxonomy) — a torn read can NEVER silently decode garbage.

export const RECORD_SIZE = 192;
export const CRC_OFFSET = 188;
export const MAGIC0 = 0x53; // 'S'
export const MAGIC1 = 0x48; // 'H'
export const MAGIC2 = 0x50; // 'P'
export const MAGIC3 = 0x31; // '1'
export const LAYOUT_VERSION = 1;

// §6 Law 4 error taxonomy (frozen codes; shared across all managed runtimes)
export const E_BAD_MAGIC = 1;
export const E_BAD_VERSION = 2;
export const E_BAD_SIZE = 3;
export const E_CRC_MISMATCH = 4;
export const E_RESERVED_DIRTY = 5;
export const E_PROBE_UNAVAILABLE = 6;
export const E_DEVICE_LOST = 7;
export const E_FFI_TIMEOUT = 8;
export const E_HEAP_PRESSURE = 9;
export const E_LISTENER_LEAK = 10;
export const E_ALIGN_INVALID = 11;
export const E_HUD_CONTEXT_LOST = 12;
export const E_UNMARSHAL_FAILED = 13;
export const E_TIER_EXHAUSTED = 14;
export const E_HUD_RECOVERED = 15;

export const ERROR_NAMES = new Map([
  [E_BAD_MAGIC, 'E_BAD_MAGIC'], [E_BAD_VERSION, 'E_BAD_VERSION'],
  [E_BAD_SIZE, 'E_BAD_SIZE'], [E_CRC_MISMATCH, 'E_CRC_MISMATCH'],
  [E_RESERVED_DIRTY, 'E_RESERVED_DIRTY'], [E_PROBE_UNAVAILABLE, 'E_PROBE_UNAVAILABLE'],
  [E_DEVICE_LOST, 'E_DEVICE_LOST'], [E_FFI_TIMEOUT, 'E_FFI_TIMEOUT'],
  [E_HEAP_PRESSURE, 'E_HEAP_PRESSURE'], [E_LISTENER_LEAK, 'E_LISTENER_LEAK'],
  [E_ALIGN_INVALID, 'E_ALIGN_INVALID'], [E_HUD_CONTEXT_LOST, 'E_HUD_CONTEXT_LOST'],
  [E_UNMARSHAL_FAILED, 'E_UNMARSHAL_FAILED'], [E_TIER_EXHAUSTED, 'E_TIER_EXHAUSTED'],
  [E_HUD_RECOVERED, 'E_HUD_RECOVERED'],
]);

// Enum domains (§2)
export const TIER_UNKNOWN = 0, TIER_FLAGSHIP = 1, TIER_MID = 2, TIER_BUDGET = 3;
export const THERMAL_NOMINAL = 0, THERMAL_LIGHT = 1, THERMAL_MODERATE = 2,
  THERMAL_SEVERE = 3, THERMAL_CRITICAL = 4;
export const VIS_VISIBLE = 0, VIS_HIDDEN = 1, VIS_UNKNOWN = 2;
export const CHARGING_NO = 0, CHARGING_YES = 1, CHARGING_UNKNOWN = 2;
export const BATTERY_UNKNOWN = 0xffff;

// §3 feature bits (u64 as lo/hi u32 pair)
export const FEAT = {
  WASM_SIMD128: 0, SHARED_ARRAY_BUFFER: 1, WEBGPU: 2, WEBGL2: 3,
  AVX512: 4, AVX2: 5, SSE42: 6, NEON: 7, SVE2: 8, RVV: 9,
  METAL_3: 10, CUDA: 11, APPLE_MPS: 12, OPENVINO: 13,
  FASTRPC_DSP: 14, NEUROPILOT: 15, MULTILANE_DMA: 16, BIG_LITTLE: 17,
  THERMAL_SENSOR: 18, DLPACK_EXPORT: 19,
};
const FEAT_BY_NAME = new Map(Object.entries(FEAT).map(([k, v]) => [v, k]));

// CRC-32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF) — identical arithmetic
// to fixtures/generate.mjs, python/weft_spectrum/wire.py and the native audits.
const CRC_TABLE = new Int32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = (c & 1) !== 0 ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
  CRC_TABLE[n] = c;
}

export function crc32(bytes, len) {
  let c = -1;
  for (let i = 0; i < len; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

// ---------------------------------------------------------------------------
// ProfileView — zero-copy window over ONE 192-byte record.
// ---------------------------------------------------------------------------
const U64_FIELDS = [[40, 'cpuMaxClockKhz'], [48, 'memoryTotalBytes'], [56, 'memoryBudgetBytes'], [72, 'maxFrameRateMilliHz']];

export class ProfileView {
  constructor(buffer, byteOffset = 0) {
    if (buffer instanceof DataView) {
      this._dv = buffer;
    } else if (ArrayBuffer.isView(buffer)) {
      // TypedArray/Buffer input (Buffer is a view over a pooled ArrayBuffer)
      this._dv = new DataView(buffer.buffer, buffer.byteOffset + byteOffset, buffer.byteLength);
    } else {
      this._dv = new DataView(buffer, byteOffset, RECORD_SIZE);
    }
    this._bytes = new Uint8Array(this._dv.buffer, this._dv.byteOffset, this._dv.byteLength);
  }

  // Law 4 gate: returns 0 when the record is intact, else the §6 code.
  // Checks size -> magic -> version -> reserved -> CRC (cheapest first).
  validate() {
    if (this._dv.byteLength < RECORD_SIZE) return E_BAD_SIZE;
    if (this._bytes[0] !== MAGIC0 || this._bytes[1] !== MAGIC1 ||
      this._bytes[2] !== MAGIC2 || this._bytes[3] !== MAGIC3) return E_BAD_MAGIC;
    if (this._dv.getUint16(4, true) !== LAYOUT_VERSION) return E_BAD_VERSION;
    if (this._dv.getUint16(6, true) !== RECORD_SIZE) return E_BAD_SIZE;
    for (let i = 104; i < CRC_OFFSET; i++) if (this._bytes[i] !== 0) return E_RESERVED_DIRTY;
    if (crc32(this._bytes, CRC_OFFSET) !== this._dv.getUint32(CRC_OFFSET, true)) return E_CRC_MISMATCH;
    return 0;
  }

  get layoutVersion() { return this._dv.getUint16(4, true); }
  get recordSize() { return this._dv.getUint16(6, true); }
  get featureFlagsLo() { return this._dv.getUint32(8, true); }
  get featureFlagsHi() { return this._dv.getUint32(12, true); }
  get siliconTier() { return this._dv.getUint32(16, true); }
  get thermalState() { return this._dv.getUint32(20, true); }
  get perfCores() { return this._dv.getUint32(24, true); }
  get effCores() { return this._dv.getUint32(28, true); }
  get gpuFamily() { return this._dv.getUint32(32, true); }
  get cacheLineBytes() { return this._dv.getUint32(36, true); }
  get simdWidthBits() { return this._dv.getUint32(64, true); }
  get frameBudgetUs() { return this._dv.getUint32(68, true); }
  get batteryPermille() { return this._dv.getUint32(80, true); }
  get batteryCharging() { return this._dv.getUint32(84, true); }
  get visibility() { return this._dv.getUint32(88, true); }
  get dmaLaneCount() { return this._dv.getUint32(92, true); }
  get vendorId() { return this._dv.getUint32(96, true); }
  get deviceId() { return this._dv.getUint32(100, true); }
  get crc32() { return this._dv.getUint32(CRC_OFFSET, true); }
  get cpuMaxClockKhz() { return this._dv.getUint32(40, true) + this._dv.getUint32(44, true) * 0x100000000; }
  get memoryTotalBytes() { return this._dv.getUint32(48, true) + this._dv.getUint32(52, true) * 0x100000000; }
  get memoryBudgetBytes() { return this._dv.getUint32(56, true) + this._dv.getUint32(60, true) * 0x100000000; }
  get maxFrameRateMilliHz() { return this._dv.getUint32(72, true) + this._dv.getUint32(76, true) * 0x100000000; }

  // 64-bit reads use lo/hi u32 pairs (no BigInt allocs on the hot path)
  u64(field) {
    for (let i = 0; i < U64_FIELDS.length; i++) {
      if (U64_FIELDS[i][1] === field) {
        const off = U64_FIELDS[i][0];
        return this._dv.getUint32(off, true) + this._dv.getUint32(off + 4, true) * 0x100000000;
      }
    }
    return 0;
  }

  hasFeatureBit(bit) {
    return bit < 32
      ? (this.featureFlagsLo & ((1 >>> 0) << bit)) !== 0
      : (this.featureFlagsHi & ((1 >>> 0) << (bit - 32))) !== 0;
  }
  featureName(bit) { return FEAT_BY_NAME.get(bit) || null; }

  // Copies all fields into a caller-owned flyweight (Law 1: no allocation).
  snapshotInto(dst) {
    dst.layoutVersion = this.layoutVersion;
    dst.recordSize = this.recordSize;
    dst.featureFlagsLo = this.featureFlagsLo;
    dst.featureFlagsHi = this.featureFlagsHi;
    dst.siliconTier = this.siliconTier;
    dst.thermalState = this.thermalState;
    dst.perfCores = this.perfCores;
    dst.effCores = this.effCores;
    dst.gpuFamily = this.gpuFamily;
    dst.cacheLineBytes = this.cacheLineBytes;
    dst.cpuMaxClockKhz = this.cpuMaxClockKhz;
    dst.memoryTotalBytes = this.memoryTotalBytes;
    dst.memoryBudgetBytes = this.memoryBudgetBytes;
    dst.simdWidthBits = this.simdWidthBits;
    dst.frameBudgetUs = this.frameBudgetUs;
    dst.maxFrameRateMilliHz = this.maxFrameRateMilliHz;
    dst.batteryPermille = this.batteryPermille;
    dst.batteryCharging = this.batteryCharging;
    dst.visibility = this.visibility;
    dst.dmaLaneCount = this.dmaLaneCount;
    dst.vendorId = this.vendorId;
    dst.deviceId = this.deviceId;
    return dst;
  }
}

export function makeProfileFlyweight() {
  return {
    layoutVersion: 0, recordSize: 0, featureFlagsLo: 0, featureFlagsHi: 0,
    siliconTier: 0, thermalState: 0, perfCores: 0, effCores: 0, gpuFamily: 0,
    cacheLineBytes: 64, cpuMaxClockKhz: 0, memoryTotalBytes: 0,
    memoryBudgetBytes: 0, simdWidthBits: 0, frameBudgetUs: 0,
    maxFrameRateMilliHz: 0, batteryPermille: BATTERY_UNKNOWN,
    batteryCharging: CHARGING_UNKNOWN, visibility: VIS_UNKNOWN,
    dmaLaneCount: 0, vendorId: 0, deviceId: 0,
  };
}

// Convenience: validate + snapshot in one call. Init path (may allocate the
// result object); steady-state polling should use ProfileView directly.
export function decodeProfile(buffer, byteOffset = 0) {
  const view = new ProfileView(buffer, byteOffset);
  const code = view.validate();
  if (code !== 0) return { ok: false, code, name: ERROR_NAMES.get(code), view };
  return { ok: true, code: 0, name: null, view };
}
