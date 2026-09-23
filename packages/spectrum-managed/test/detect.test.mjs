// test/detect.test.mjs — runtime feature detection (Node lane) + fallback profile.
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  detectWasmSimd128, detectSharedArrayBuffer, detectWebGPU, detectWorkerCount,
  simdWidthFromFeatures, makeFallbackProfile, detectInto,
} from '../src/detect.js';
import { FEAT, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET, BATTERY_UNKNOWN, makeProfileFlyweight } from '../src/wire.js';

test('WASM SIMD128 is available on this runtime (Node 22/24 lane)', () => {
  assert.equal(detectWasmSimd128(), true);
});

test('SharedArrayBuffer detection returns a boolean and never throws', () => {
  const v = detectSharedArrayBuffer();
  assert.equal(typeof v, 'boolean');
});

test('WebGPU absent in bare Node -> false without throwing', () => {
  assert.equal(detectWebGPU(undefined), false);
  assert.equal(detectWebGPU({}), false);
  assert.equal(detectWebGPU({ gpu: {} }), true); // presence probe only
});

test('worker count is a positive integer (navigator.hardwareConcurrency in Node >=21)', () => {
  const n = detectWorkerCount();
  assert.equal(Number.isInteger(n), true);
  assert.ok(n >= 1);
});

test('simdWidthFromFeatures mirrors the SHP1 ISA mapping', () => {
  assert.equal(simdWidthFromFeatures(1 << FEAT.AVX512), 512);
  assert.equal(simdWidthFromFeatures(1 << FEAT.AVX2), 256);
  assert.equal(simdWidthFromFeatures(1 << FEAT.NEON), 128);
  assert.equal(simdWidthFromFeatures(0), 128);
});

test('fallback profile is coherent and tier-graded (E_PROBE_UNAVAILABLE path)', () => {
  const fw = makeFallbackProfile({ memoryTotalBytes: 64 * 1024 ** 3, workerCount: 12 });
  assert.equal(fw.siliconTier, TIER_FLAGSHIP);
  assert.equal(fw.maxFrameRateMilliHz, 240000);
  assert.equal(fw.frameBudgetUs, 4166);
  assert.equal(fw.batteryPermille, BATTERY_UNKNOWN);
  assert.equal(fw.simdWidthBits >= 128, true);
  assert.ok(fw.featureFlagsLo !== 0, 'at least WASM SIMD128 detected');

  const mid = makeFallbackProfile({ memoryTotalBytes: 16 * 1024 ** 3, workerCount: 6 });
  assert.equal(mid.siliconTier, TIER_MID);
  assert.equal(mid.maxFrameRateMilliHz, 120000);

  const budget = makeFallbackProfile({ memoryTotalBytes: 2 * 1024 ** 3, workerCount: 2 });
  assert.equal(budget.siliconTier, TIER_BUDGET);
  assert.equal(budget.maxFrameRateMilliHz, 60000);
  assert.equal(budget.memoryBudgetBytes, Math.floor((2 * 1024 ** 3) / 8));
});

test('detectInto reuses the caller flyweight (no new object)', () => {
  const fw = makeProfileFlyweight();
  const ret = detectInto(fw, { memoryTotalBytes: 8 * 1024 ** 3, workerCount: 4 });
  assert.equal(ret, fw);
  assert.equal(fw.dmaLaneCount, 1, 'no BIG_LITTLE at 4 workers');
  const fw2 = detectInto(fw, { memoryTotalBytes: 8 * 1024 ** 3, workerCount: 8 });
  assert.equal(fw2, fw);
  assert.equal(fw2.dmaLaneCount, 2, 'BIG_LITTLE heuristic at 8 workers');
});
