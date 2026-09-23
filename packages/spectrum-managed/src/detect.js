// src/detect.js — runtime hardware feature detection (Pillar 5, mandate A).
//
// One-shot INIT-time detection (allocation here is allowed): WASM SIMD128
// probe, SharedArrayBuffer, WebGPU adapter, WebGL2, worker count. Results are
// written into a caller-owned SHP1 flyweight (Law 1: the steady-state
// telemetry loop afterwards only READS primitives from that flyweight).
//
// E1 seam: when Engineer 1's weft_hw_profile_t probe is available it wins;
// this module only fills the E_PROBE_UNAVAILABLE fallback profile.

import {
  FEAT, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET, THERMAL_NOMINAL,
  CHARGING_UNKNOWN, BATTERY_UNKNOWN, VIS_UNKNOWN, RECORD_SIZE,
  makeProfileFlyweight,
} from './wire.js';

// Standard v128 smoke module:
//   (module (func (result v128) (v128.const i32x4 0 0 0 0)))
// Hand-encoded: header | type sec (v128 = 0x7b) | func sec | code sec with
// body [00 locals][fd 0c + 16B zero lane][0b end].
const WASM_SIMD_MODULE = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b,
  0x03, 0x02, 0x01, 0x00,
  0x0a, 0x16, 0x01, 0x14, 0x00,
  0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0b,
]);

export function detectWasmSimd128() {
  try { return WebAssembly.validate(WASM_SIMD_MODULE); } catch { return false; }
}

export function detectSharedArrayBuffer() {
  try {
    if (typeof SharedArrayBuffer === 'undefined') return false;
    new SharedArrayBuffer(8); // constructibility gate (Node 22+ requires flag-free path)
    return true;
  } catch { return false; }
}

export function detectWebGPU(navigatorLike = globalThis.navigator) {
  try {
    return typeof navigatorLike !== 'undefined' && !!navigatorLike.gpu;
  } catch { return false; }
}

// Async tier-1 confirmation: adapter request is the honest WebGPU signal.
// Never throws (Law 4): device-lost / absent adapter -> false.
export async function confirmWebGPUAdapter(navigatorLike = globalThis.navigator) {
  try {
    if (!detectWebGPU(navigatorLike)) return false;
    const adapter = await navigatorLike.gpu.requestAdapter();
    return !!adapter;
  } catch { return false; }
}

export function detectWebGL2(doc = globalThis.document) {
  try {
    if (!doc || !doc.createElement) return false;
    const canvas = doc.createElement('canvas');
    return !!canvas.getContext('webgl2');
  } catch { return false; }
}

export function detectWorkerCount(navigatorLike = globalThis.navigator) {
  try {
    if (navigatorLike && typeof navigatorLike.hardwareConcurrency === 'number' && navigatorLike.hardwareConcurrency > 0) {
      return navigatorLike.hardwareConcurrency;
    }
  } catch { /* fall through */ }
  return 1;
}

// SIMD width from detected ISA bits (mirrors the SHP1 feature vector)
export function simdWidthFromFeatures(flagsLo) {
  if (flagsLo & (1 << FEAT.AVX512)) return 512;
  if (flagsLo & (1 << FEAT.AVX2)) return 256;
  return 128; // NEON / SVE2 / RVV / SSE-class / WASM v128 baseline
}

// Writes a coherent fallback profile into `dst` (a makeProfileFlyweight()).
// `features` is an array of FEAT names (init path). This is the explicit
// E_PROBE_UNAVAILABLE path: the SDK still functions, honestly down-specced.
export function detectInto(dst, opts = {}) {
  const lo = opts.featureFlagsLo | 0;
  const setBit = (name) => { const b = FEAT[name]; if (b < 32) dst.featureFlagsLo |= (1 >>> 0) << b; };

  if (detectWasmSimd128()) setBit('WASM_SIMD128');
  if (detectSharedArrayBuffer()) setBit('SHARED_ARRAY_BUFFER');
  if (opts.webgpu === true || (opts.webgpu === undefined && detectWebGPU())) setBit('WEBGPU');
  if (opts.webgl2 === true || (opts.webgl2 === undefined && detectWebGL2())) setBit('WEBGL2');

  const workers = opts.workerCount || detectWorkerCount();
  const bigLittle = workers >= 6; // P+E topology heuristic (documented, coarse)
  if (bigLittle) setBit('BIG_LITTLE');
  if (opts.dlpack === true) setBit('DLPACK_EXPORT');

  // Tier heuristic: core count + memory. Honest, coarse, documented.
  const memBytes = opts.memoryTotalBytes || 0;
  const cores = workers;
  let tier = TIER_BUDGET;
  if (cores >= 10 && memBytes >= 32 * 1024 ** 3) tier = TIER_FLAGSHIP;
  else if (cores >= 4 && memBytes >= 8 * 1024 ** 3) tier = TIER_MID;

  dst.siliconTier = opts.siliconTier !== undefined ? opts.siliconTier : tier;
  dst.thermalState = opts.thermalState !== undefined ? opts.thermalState : THERMAL_NOMINAL;
  dst.perfCores = opts.perfCores !== undefined ? opts.perfCores : Math.max(1, cores >> 1);
  dst.effCores = opts.effCores !== undefined ? opts.effCores : Math.max(0, cores >> 1);
  dst.gpuFamily = opts.gpuFamily !== undefined ? opts.gpuFamily : 0;
  dst.cacheLineBytes = opts.cacheLineBytes || 64;
  dst.cpuMaxClockKhz = opts.cpuMaxClockKhz || 0;
  dst.memoryTotalBytes = memBytes;
  dst.memoryBudgetBytes = opts.memoryBudgetBytes || Math.floor((memBytes || 512 * 1024 * 1024) / 8);
  dst.simdWidthBits = simdWidthFromFeatures(dst.featureFlagsLo);
  dst.frameBudgetUs = dst.siliconTier === TIER_FLAGSHIP ? 4166
    : dst.siliconTier === TIER_MID ? 8333 : 16666;
  dst.maxFrameRateMilliHz = dst.siliconTier === TIER_FLAGSHIP ? 240000
    : dst.siliconTier === TIER_MID ? 120000 : 60000;
  dst.batteryPermille = opts.batteryPermille !== undefined ? opts.batteryPermille : BATTERY_UNKNOWN;
  dst.batteryCharging = opts.batteryCharging !== undefined ? opts.batteryCharging : CHARGING_UNKNOWN;
  dst.visibility = opts.visibility !== undefined ? opts.visibility : VIS_UNKNOWN;
  dst.dmaLaneCount = opts.dmaLaneCount !== undefined ? opts.dmaLaneCount : (bigLittle ? 2 : 1);
  dst.vendorId = opts.vendorId || 0;
  dst.deviceId = opts.deviceId || 0;
  return dst;
}

export function makeFallbackProfile(opts) {
  const fw = makeProfileFlyweight();
  detectInto(fw, opts);
  return fw;
}

export { RECORD_SIZE };
