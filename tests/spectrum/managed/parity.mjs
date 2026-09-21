// parity.mjs — cross-language parity harness (Pillar 5, mandate F).
//
// Asserts IDENTICAL hardware-profile decodes and IDENTICAL governor
// behavior across the managed runtimes:
//   TS (this file, packages/spectrum-managed)
//   Python (parity_helper.py, python/weft_spectrum) via subprocess
//   both compared against expected_profile.json + governor_vector.json
//   (Swift + Dart projections are pinned by their static-audit lanes and
//   XCTest/pure-Dart suites against the same frozen manifests.)
//
// Exit 0 only when every field and every tick matches. Fail-closed.

import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { decodeProfile, makeProfileFlyweight } from '../../../packages/spectrum-managed/src/wire.js';
import { createCadenceState, cadenceTick, tierTick } from '../../../packages/spectrum-managed/src/governor.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const FIX = join(HERE, 'fixtures');
const expected = JSON.parse(readFileSync(join(FIX, 'expected_profile.json'), 'utf8'));
const vec = JSON.parse(readFileSync(join(FIX, 'governor_vector.json'), 'utf8'));
const tierVec = JSON.parse(readFileSync(join(FIX, 'tier_vector.json'), 'utf8'));

const FIELDS = [
  'layoutVersion', 'recordSize', 'featureFlagsLo', 'featureFlagsHi', 'siliconTier',
  'thermalState', 'perfCores', 'effCores', 'gpuFamily', 'cacheLineBytes',
  'cpuMaxClockKhz', 'memoryTotalBytes', 'memoryBudgetBytes', 'simdWidthBits',
  'frameBudgetUs', 'maxFrameRateMilliHz', 'batteryPermille', 'batteryCharging',
  'visibility', 'dmaLaneCount', 'vendorId', 'deviceId', 'crc32',
];

let checks = 0, failures = 0;
const fail = (msg) => { failures++; console.error(`  FAIL ${msg}`); };
const ok = (msg) => { checks++; console.log(`  ok   ${msg}`); };

// ---------------------------------------------------------------------------
// 1. TS projection
// ---------------------------------------------------------------------------
const ts = { profiles: {}, governor: { caps: [], tierStages: [] } };
for (const name of ['flagship', 'mid', 'budget']) {
  const buf = readFileSync(join(FIX, `hw-profile-${name}.bin`));
  const { ok: good, code, view } = decodeProfile(buf);
  const fw = makeProfileFlyweight();
  view.snapshotInto(fw);
  ts.profiles[name] = {
    code: good ? 0 : code,
    fields: Object.fromEntries(FIELDS.map((f) => [f, f === 'crc32' ? view.crc32 : fw[f]])),
  };
}
{
  const torn = readFileSync(join(FIX, 'hw-profile-torn.bin'));
  ts.profiles.torn = { code: decodeProfile(torn).code, expected: 4 };
}
{
  const st = createCadenceState();
  const inp = { thermalState: 0, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0, tierMaxHz: vec.profileMaxHz };
  for (const t of vec.ticks) {
    inp.thermalState = t.thermal;
    inp.batteryPermille = t.batteryPermille;
    inp.batteryCharging = t.batteryCharging;
    inp.visibility = t.visibility;
    inp.heapPressure = t.heapPressure;
    ts.governor.caps.push(cadenceTick(st, inp));
  }
  const tst = createCadenceState();
  const tinp = { heapPressure: 0, profileBudgetBytes: 8589934592 };
  for (const t of tierVec.ticks) {
    tinp.heapPressure = t.heapPressure;
    tierTick(tst, tinp);
    ts.governor.tierStages.push(tst.tierStage);
  }
}

// ---------------------------------------------------------------------------
// 2. TS vs frozen manifests
// ---------------------------------------------------------------------------
for (const name of ['flagship', 'mid', 'budget']) {
  const exp = expected[name];
  const got = ts.profiles[name];
  if (got.code !== 0) fail(`${name}: decode code ${got.code}`);
  else {
    let bad = 0;
    for (const f of FIELDS) if (got.fields[f] !== exp[f]) { bad++; fail(`${name}.${f}: ${got.fields[f]} != ${exp[f]}`); }
    if (bad === 0) ok(`${name}: all ${FIELDS.length} fields == expected_profile.json`);
  }
}
if (ts.profiles.torn.code === ts.profiles.torn.expected) ok('torn record -> E_CRC_MISMATCH (TS)');
else fail(`torn: TS code ${ts.profiles.torn.code} != 4`);
{
  const transitions = [vec.expected[0].cap];
  for (let i = 1; i < vec.expected.length; i++) {
    if (vec.expected[i].cap !== vec.expected[i - 1].cap) transitions.push(vec.expected[i].cap);
  }
  const capsMatch = ts.governor.caps.every((c, i) => c === vec.expected[i].cap);
  const trMatch = JSON.stringify(transitions) === JSON.stringify(vec.expectedCapTransitions);
  if (capsMatch && trMatch) ok(`governor: ${ts.governor.caps.length} ticks identical to frozen vector (TS)`);
  else fail('governor: TS sequence diverges from frozen vector');
}

// ---------------------------------------------------------------------------
// 3. Python projection vs TS
// ---------------------------------------------------------------------------
const pyPath = join(HERE, 'parity_helper.py');
const py = spawnSync(process.env.PYTHON || 'python3', [pyPath], { encoding: 'utf8' });
if (py.status !== 0) {
  fail(`python helper exited ${py.status}: ${py.stderr.slice(0, 400)}`);
} else {
  const pj = JSON.parse(py.stdout);
  for (const name of ['flagship', 'mid', 'budget']) {
    const a = ts.profiles[name].fields, b = pj.profiles[name].fields;
    let bad = 0;
    for (const f of FIELDS) if (a[f] !== b[f]) { bad++; fail(`parity ${name}.${f}: TS ${a[f]} != Py ${b[f]}`); }
    if (bad === 0) ok(`${name}: TS == Python on all ${FIELDS.length} fields`);
  }
  if (pj.profiles.torn.code === 4) ok('torn record -> E_CRC_MISMATCH (Python)');
  else fail(`torn: Py code ${pj.profiles.torn.code} != 4`);

  const capsEq = JSON.stringify(ts.governor.caps) === JSON.stringify(pj.governor.caps);
  const stagesEq = JSON.stringify(ts.governor.tierStages) === JSON.stringify(pj.governor.tierStages);
  if (capsEq && stagesEq) {
    ok(`governor: TS == Python identical ${ts.governor.caps.length}-tick cap sequence + tier staging`);
  } else fail('governor: TS vs Python divergence (caps or tierStages)');
}

// ---------------------------------------------------------------------------
// 4. Swift + Dart audit-lane pinning (same frozen manifests)
// ---------------------------------------------------------------------------
const audits = [
  ['swift', join(HERE, '..', '..', '..', 'apple', 'WeftSpectrum', 'audit', 'static_audit.mjs')],
  ['dart', join(HERE, '..', '..', '..', 'android', 'weft_spectrum', 'audit', 'static_audit.mjs')],
];
for (const [name, path] of audits) {
  const r = spawnSync('node', [path], { encoding: 'utf8' });
  if (r.status === 0) ok(`${name} audit lane: PASS (pinning its projection to the same manifests)`);
  else fail(`${name} audit lane failed: ${r.stdout.split('\n').slice(-1)}`);
}

console.log(`\nparity: ${checks} ok, ${failures} failures`);
process.exit(failures === 0 ? 0 : 1);
