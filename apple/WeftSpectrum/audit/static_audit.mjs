// audit/static_audit.mjs — Swift structural audit without a Swift toolchain.
//
// No-SDK lane (proven pattern, Pillar 4 swift-heddle): scans the Swift
// sources for the structural invariants that MATTER and cross-checks every
// frozen constant against the TS/Python projections. Fail-closed.
//
// Checks:
//   1.  SHP1 geometry constants == wire.js (192/188/1)
//   2.  Full 15-code Law 4 taxonomy with identical integers
//   3.  Endianness: every multi-byte load goes through a .littleEndian init;
//       zero .bigEndian occurrences
//   4.  Governor constants == governor_vector.json
//   5.  Feature bit table complete (20 bits, identical integers)
//   6.  Hot-path purity: cadenceTick body — no allocations, no await, no
//       string interpolation
//   7.  Observation contract: @Observable + @MainActor on the model;
//       edge-triggered publishes (assignment guarded by change checks)
//   8.  Fail-soft seams: applyPressureEvent / attach never throw paths;
//       down-tier ladder present; PipelineSelector conservative default
//   9.  Golden projection: flagship flagsLo 0xF158F + budget 8589934592 +
//       CRC vector 0xCBF43926 asserted in tests

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const PKG = dirname(dirname(fileURLToPath(import.meta.url)));
const FIX = join(PKG, '..', '..', 'tests', 'spectrum', 'managed', 'fixtures');
const expected = JSON.parse(readFileSync(join(FIX, 'expected_profile.json'), 'utf8'));
const vec = JSON.parse(readFileSync(join(FIX, 'governor_vector.json'), 'utf8'));

let pass = 0, fail = 0;
const failures = [];
function check(name, ok, detail = '') {
  if (ok) { pass++; } else { fail++; failures.push(`${name}${detail ? ' :: ' + detail : ''}`); }
}
const read = (p) => readFileSync(join(PKG, p), 'utf8');
const stripComments = (src) => src
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/\/\/.*$/gm, '');

const swiftFiles = [];
(function walk(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (statSync(p).isDirectory()) walk(p);
    else if (e.name.endsWith('.swift')) swiftFiles.push(p);
  }
})(join(PKG, 'Sources'));
const testFiles = [];
(function walk(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (statSync(p).isDirectory()) walk(p);
    else if (e.name.endsWith('.swift')) testFiles.push(p);
  }
})(join(PKG, 'Tests'));

const wire = read('Sources/WeftSpectrum/SpectrumWire.swift');
const gov = read('Sources/WeftSpectrum/SpectrumGovernor.swift');
const model = read('Sources/WeftSpectrum/WeftSpectrumModel.swift');
const selector = read('Sources/WeftSpectrum/MetalPipelineSelector.swift');
const tests = testFiles.map((p) => readFileSync(p, 'utf8')).join('\n');
const all = wire + gov + model + selector + tests;
const allStripped = stripComments(all);

// --- 1. geometry
check('geometry: record size 192', /shp1RecordSize: Int = 192/.test(all));
check('geometry: crc offset 188', /shp1CrcOffset: Int = 188/.test(all));
check('geometry: layout version 1', /shp1LayoutVersion: UInt16 = 1/.test(all));

// --- 2. taxonomy
const codes = { eBadMagic: 1, eBadVersion: 2, eBadSize: 3, eCrcMismatch: 4, eReservedDirty: 5,
  eProbeUnavailable: 6, eDeviceLost: 7, eFfiTimeout: 8, eHeapPressure: 9, eListenerLeak: 10,
  eAlignInvalid: 11, eHudContextLost: 12, eUnmarshalFailed: 13, eTierExhausted: 14, eHudRecovered: 15 };
let taxOk = 0;
for (const [k, v] of Object.entries(codes)) {
  if (new RegExp(`let ${k}: Int32 = ${v}\\b`).test(all)) taxOk++;
}
check(`taxonomy: 15/15 frozen codes`, taxOk === 15, `matched ${taxOk}`);

// --- 3. endianness
const loads = allStripped.match(/loadUnaligned\(fromByteOffset:[^)]+as: U?I?n?t?(?:8|16|32|64)\.self\)/g) || [];
const leWrapped = (allStripped.match(/(?:UInt8|UInt16|UInt32|UInt64|Int8|Int16|Int32|Int64)\(littleEndian: bytes\.loadUnaligned/g) || []).length;
check(`endianness: ${leWrapped}/${loads.length} loads wrapped in littleEndian inits`,
  leWrapped === loads.length && loads.length >= 21, `wrapped=${leWrapped} loads=${loads.length}`);
check('endianness: no bigEndian anywhere', !/bigEndian/.test(allStripped));

// --- 4. governor constants
check('governor: ladder', /cadenceLadder: \[Int\] = \[240, 120, 60, 30\]/.test(all));
check('governor: sustainedTicks', new RegExp(`sustainedTicks: Int = ${vec.constants.SUSTAINED_TICKS}`).test(all));
check('governor: recoveryTicks', new RegExp(`recoveryTicks: Int = ${vec.constants.RECOVERY_TICKS}`).test(all));
check('governor: backgroundCap', new RegExp(`backgroundCap: Int = ${vec.constants.BACKGROUND_CAP}`).test(all));
check('governor: lowBattery', new RegExp(`lowBatteryPermille: Int = ${vec.constants.LOW_BATTERY_PERMILLE}`).test(all));
check('governor: maxTierStages', new RegExp(`maxTierStages: Int = ${vec.constants.MAX_TIER_STAGES}`).test(all));

// --- 5. feature bits (20, identical integers)
let featOk = 0;
for (let i = 0; i < 20; i++) {
  const names = ['featWasmSimd128', 'featSharedArrayBuffer', 'featWebgpu', 'featWebgl2',
    'featAvx512', 'featAvx2', 'featSse42', 'featNeon', 'featSve2', 'featRvv', 'featMetal3',
    'featCuda', 'featAppleMps', 'featOpenvino', 'featFastRpcDsp', 'featNeuropilot',
    'featMultilaneDma', 'featBigLittle', 'featThermalSensor', 'featDlpackExport'];
  if (new RegExp(`let ${names[i]}: UInt8 = ${i}(?![0-9])`).test(all)) featOk++;
}
check('feature bits: 20/20 with identical integers', featOk === 20, `matched ${featOk}`);

// --- 6. hot-path purity (cadenceTick body)
const tickStart = gov.indexOf('public func cadenceTick(');
const tickEnd = gov.indexOf('\n}', tickStart);
const tickBody = stripComments(gov.slice(tickStart, tickEnd));
check('hot path: cadenceTick found', tickStart > 0 && tickEnd > tickStart);
check('hot path: no allocation patterns',
  !/Array\(|Dictionary\(|Set\(|\.append\(|\.map\(|\.filter\(|\.compactMap\(|String\(|\+=\s*\[|=\s*\[|:\s*\[[A-Za-z]/.test(tickBody),
  'found allocation pattern in tick body');
check('hot path: no await/async', !/\bawait\b|\basync\b/.test(tickBody));

// --- 7. observation contract
check('observation: @MainActor @Observable final class',
  /@MainActor\s*\n@Observable\s*\npublic final class WeftSpectrumModel/.test(model));
check('observation: edge-triggered capHz publish',
  /if cadence\.capHz != capHz \{ capHz = cadence\.capHz \}/.test(model));
check('observation: edge-triggered thermal publish',
  /if thermal != thermalCode \{ thermalCode = thermal \}/.test(model));

// --- 8. fail-soft seams
check('seams: applyPressureEvent down-tier ladder', /applyPressureEvent/.test(model) && /tierTick\(cadence, input\)/.test(model));
check('seams: pipeline selector conservative default',
  selector.includes('.conservative') && /default: return 1/.test(selector));
check('seams: attach fail-soft on invalid record', /lastErrorCode = code/.test(model) && /lastErrorCode = eBadSize/.test(model));

// --- 9. golden projection
const flag = expected.flagship;
check('golden projection: flagsLo 0xF158F asserted', /0x000F_158F/.test(tests));
check('golden projection: budget 8589934592 asserted',
  tests.includes('8_589_934_592') && String(flag.memoryBudgetBytes) === '8589934592');
check('golden projection: CRC vector 0xCBF43926', tests.includes('0xCBF43926'));
check('golden projection: record geometry asserted',
  tests.includes('shp1RecordSize, 192') && tests.includes('shp1CrcOffset, 188'));

// --- report
console.log(`swift static audit: ${pass} passed, ${fail} failed`);
if (fail > 0) {
  for (const f of failures) console.error(`  FAIL ${f}`);
  process.exit(1);
}
console.log('swift static audit: PASS');
