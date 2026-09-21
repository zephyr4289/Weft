// test/wire.test.mjs — SHP1 decode parity vs golden fixtures + Law 4 matrix.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ProfileView, makeProfileFlyweight, decodeProfile, crc32, ERROR_NAMES,
  E_BAD_MAGIC, E_BAD_VERSION, E_BAD_SIZE, E_CRC_MISMATCH, E_RESERVED_DIRTY,
  RECORD_SIZE, CRC_OFFSET, FEAT, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET,
} from '../src/wire.js';

const FIX = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'tests', 'spectrum', 'managed', 'fixtures');
const expected = JSON.parse(readFileSync(join(FIX, 'expected_profile.json'), 'utf8'));

const NUMERIC_FIELDS = [
  'layoutVersion', 'recordSize', 'featureFlagsLo', 'featureFlagsHi', 'siliconTier',
  'thermalState', 'perfCores', 'effCores', 'gpuFamily', 'cacheLineBytes',
  'cpuMaxClockKhz', 'memoryTotalBytes', 'memoryBudgetBytes', 'simdWidthBits',
  'frameBudgetUs', 'maxFrameRateMilliHz', 'batteryPermille', 'batteryCharging',
  'visibility', 'dmaLaneCount', 'vendorId', 'deviceId', 'crc32',
];

test('golden flagship decodes byte-parity vs expected_profile.json', () => {
  const { ok, code, view } = decodeProfile(readFileSync(join(FIX, 'hw-profile-flagship.bin')));
  assert.equal(ok, true, `validate code ${code} (${ERROR_NAMES.get(code)})`);
  const exp = expected.flagship;
  for (const f of NUMERIC_FIELDS) assert.equal(view[f], exp[f], `field ${f}`);
  for (const name of exp.features) {
    const bit = FEAT[name];
    assert.equal(view.hasFeatureBit(bit), true, `feature ${name}`);
  }
  assert.equal(view.siliconTier, TIER_FLAGSHIP);
});

test('golden mid decodes byte-parity (u64 fields via lo/hi pairs)', () => {
  const { ok, view } = decodeProfile(readFileSync(join(FIX, 'hw-profile-mid.bin')));
  assert.equal(ok, true);
  const exp = expected.mid;
  for (const f of NUMERIC_FIELDS) assert.equal(view[f], exp[f], `field ${f}`);
  assert.equal(view.cpuMaxClockKhz, view.u64('cpuMaxClockKhz'));
  assert.equal(view.memoryBudgetBytes, view.u64('memoryBudgetBytes'));
  assert.equal(view.maxFrameRateMilliHz, view.u64('maxFrameRateMilliHz'));
  assert.equal(view.siliconTier, TIER_MID);
});

test('golden budget decodes byte-parity', () => {
  const { ok, view } = decodeProfile(readFileSync(join(FIX, 'hw-profile-budget.bin')));
  assert.equal(ok, true);
  const exp = expected.budget;
  for (const f of NUMERIC_FIELDS) assert.equal(view[f], exp[f], `field ${f}`);
  assert.equal(view.siliconTier, TIER_BUDGET);
});

test('torn fixture -> E_CRC_MISMATCH (fail-closed, no garbage decode)', () => {
  const res = decodeProfile(readFileSync(join(FIX, 'hw-profile-torn.bin')));
  assert.equal(res.ok, false);
  assert.equal(res.code, E_CRC_MISMATCH);
  assert.equal(res.name, 'E_CRC_MISMATCH');
  assert.equal(expected.torn.error, 'E_CRC_MISMATCH');
});

test('Law 4 matrix: bad magic / bad version / bad size / reserved dirty', () => {
  const good = readFileSync(join(FIX, 'hw-profile-flagship.bin'));
  const bytes = new Uint8Array(good);

  const badMagic = new Uint8Array(bytes); badMagic[0] = 0x58;
  assert.equal(new ProfileView(badMagic).validate(), E_BAD_MAGIC);

  const badVer = new Uint8Array(bytes); badVer[4] = 9;
  assert.equal(new ProfileView(badVer).validate(), E_BAD_VERSION);

  const badSize = new Uint8Array(bytes); badSize[6] = 128;
  assert.equal(new ProfileView(badSize).validate(), E_BAD_SIZE);

  const truncated = bytes.slice(0, RECORD_SIZE - 1);
  assert.equal(new ProfileView(truncated).validate(), E_BAD_SIZE);

  const dirty = new Uint8Array(bytes); dirty[120] = 1;
  assert.equal(new ProfileView(dirty).validate(), E_RESERVED_DIRTY);

  // payload mutation outside the reserved region is caught by CRC (frozen:
  // integrity net is the CRC; reserved scan covers [104,188) only)
  const dirtyFlags = new Uint8Array(bytes); dirtyFlags[12] = 1;
  assert.equal(new ProfileView(dirtyFlags).validate(), E_CRC_MISMATCH);
});

test('zero-copy proof: view sees in-place mutations of the underlying buffer', () => {
  const bytes = new Uint8Array(readFileSync(join(FIX, 'hw-profile-budget.bin')));
  const { ok, view } = decodeProfile(bytes.buffer);
  assert.equal(ok, true);
  const before = view.thermalState;
  // mutate thermal field (offset 20) + refresh CRC through the same arithmetic
  bytes[20] = (before + 1) & 0xFF;
  const dv = new DataView(bytes.buffer);
  dv.setUint32(CRC_OFFSET, crc32(bytes, CRC_OFFSET), true);
  assert.equal(view.thermalState, (before + 1) & 0xFF, 'no snapshot: same memory');
});

test('snapshotInto copies primitives into a caller-owned flyweight', () => {
  const { ok, view } = decodeProfile(readFileSync(join(FIX, 'hw-profile-flagship.bin')));
  assert.equal(ok, true);
  const fw = makeProfileFlyweight();
  const ret = view.snapshotInto(fw);
  assert.equal(ret, fw, 'returns the same object (no allocation)');
  for (const f of NUMERIC_FIELDS) {
    if (f === 'crc32') continue; // crc32 is a view-only getter (not in flyweight)
    assert.equal(fw[f], expected.flagship[f], `flyweight ${f}`);
  }
});

test('crc32 reference vectors (IEEE 802.3)', () => {
  assert.equal(crc32(new Uint8Array(0), 0), 0x00000000);
  assert.equal(crc32(new Uint8Array([0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39]), 9), 0xCBF43926);
  // "123456789" is the canonical check vector; empty input = 0
});

test('ERROR_NAMES covers the full 15-code taxonomy', () => {
  assert.equal(ERROR_NAMES.size, 15);
  assert.equal(ERROR_NAMES.get(4), 'E_CRC_MISMATCH');
  assert.equal(ERROR_NAMES.get(15), 'E_HUD_RECOVERED');
});
