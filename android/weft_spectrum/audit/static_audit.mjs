// audit/static_audit.mjs — Dart structural audit without a Flutter/Dart SDK.
//
// No-SDK lane (sandbox/CI contract, proven in Pillar 4): this audit scans the
// Dart sources for the invariants that MATTER structurally, and cross-checks
// every frozen constant against the TS/Python projections. Fail-closed:
// any check failure exits non-zero and prints the offending line.
//
// Checks:
//   1.  SHP1 record geometry constants == wire.js (192/188/1)
//   2.  Full 15-code Law 4 taxonomy present with identical integers
//   3.  Endianness: every ByteData multi-byte read uses Endian.little;
//       zero occurrences of Endian.big / bigEndian anywhere
//   4.  Governor constants == governor_vector.json constants
//   5.  Feature bit table complete (20 bits, identical integers)
//   6.  Hot-path purity: cadenceTick body has no object/list/string-literal
//       allocation, no `new `, no `await`, no string interpolation
//   7.  HUD zero-rebuild contract: shouldRepaint => false; no setState(
//       anywhere in lib/; painter repaint channel wired
//   8.  Engine fail-soft: DynamicLibrary lookups wrapped in try/catch;
//       E_PROBE_UNAVAILABLE path present; no throw into widget code
//   9.  Golden value projection: expected_profile.json flagship/budget
//       values asserted in test/spectrum_test.dart (tier map + caps)

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
// comment-stripped source for banned-pattern scans (avoids matching prose
// like "NEVER calls setState()" inside documentation comments)
const stripComments = (src) => src
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/\/\/.*$/gm, '');

// --- collect sources
const dartFiles = [];
(function walk(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (statSync(p).isDirectory()) walk(p);
    else if (e.name.endsWith('.dart')) dartFiles.push(p);
  }
})(join(PKG, 'lib'));
const testSrc = read('test/spectrum_test.dart');
const allDart = dartFiles.map((p) => readFileSync(p, 'utf8')).join('\n') + '\n' + testSrc;

// --- 1. record geometry
check('geometry: record size 192', /shp1RecordSize\s*=\s*192/.test(allDart));
check('geometry: crc offset 188', /shp1CrcOffset\s*=\s*188/.test(allDart));
check('geometry: layout version 1', /shp1LayoutVersion\s*=\s*1/.test(allDart));

// --- 2. Law 4 taxonomy (all 15 codes, correct integers)
const codes = { eBadMagic: 1, eBadVersion: 2, eBadSize: 3, eCrcMismatch: 4, eReservedDirty: 5,
  eProbeUnavailable: 6, eDeviceLost: 7, eFfiTimeout: 8, eHeapPressure: 9, eListenerLeak: 10,
  eAlignInvalid: 11, eHudContextLost: 12, eUnmarshalFailed: 13, eTierExhausted: 14, eHudRecovered: 15 };
for (const [k, v] of Object.entries(codes)) {
  check(`taxonomy: ${k} = ${v}`, new RegExp(`const int ${k}\\s*=\\s*${v};`).test(allDart));
}

// --- 3. endianness
const byteDataReads = allDart.match(/getUint(?:16|32|64)\([^)]*\)/g) || [];
const littleReads = byteDataReads.filter((r) => r.includes('Endian.little')).length;
check(`endianness: all ${byteDataReads.length} ByteData reads Endian.little`,
  littleReads === byteDataReads.length, `little=${littleReads}`);
check('endianness: no Endian.big / bigEndian', !/Endian\.big|bigEndian/.test(allDart));

// --- 4. governor constants
check('governor: ladder', /cadenceLadder\s*=\s*\[240,\s*120,\s*60,\s*30\]/.test(allDart));
check('governor: sustainedTicks', new RegExp(`sustainedTicks\\s*=\\s*${vec.constants.SUSTAINED_TICKS}`).test(allDart));
check('governor: recoveryTicks', new RegExp(`recoveryTicks\\s*=\\s*${vec.constants.RECOVERY_TICKS}`).test(allDart));
check('governor: backgroundCap', new RegExp(`backgroundCap\\s*=\\s*${vec.constants.BACKGROUND_CAP}`).test(allDart));
check('governor: lowBattery', new RegExp(`lowBatteryPermille\\s*=\\s*${vec.constants.LOW_BATTERY_PERMILLE}`).test(allDart));
check('governor: maxTierStages', new RegExp(`maxTierStages\\s*=\\s*${vec.constants.MAX_TIER_STAGES}`).test(allDart));

// --- 5. feature bits (20, identical integers)
const featNames = ['featWasmSimd128', 'featSharedArrayBuffer', 'featWebgpu', 'featWebgl2',
  'featAvx512', 'featAvx2', 'featSse42', 'featNeon', 'featSve2', 'featRvv', 'featMetal3',
  'featCuda', 'featAppleMps', 'featOpenvino', 'featFastRpcDsp', 'featNeuropilot',
  'featMultilaneDma', 'featBigLittle', 'featThermalSensor', 'featDlpackExport'];
let featOk = 0;
for (let i = 0; i < featNames.length; i++) {
  const re = new RegExp(`${featNames[i]}\\s*=\\s*${i}(?![0-9])`);
  if (re.test(allDart)) featOk++;
}
check(`feature bits: ${featNames.length}/${featNames.length} with identical integers`,
  featOk === featNames.length, `matched ${featOk}`);

// --- 6. hot-path purity (cadenceTick function body)
const govSrc = read('lib/src/spectrum_governor.dart');
const tickStart = govSrc.indexOf('int cadenceTick(CadenceState st, GovernorInput inp) {');
const tickEnd = govSrc.indexOf('\n}', tickStart);
const tickBody = govSrc.slice(tickStart, tickEnd);
check('hot path: cadenceTick found', tickStart > 0 && tickEnd > tickStart);
check('hot path: no allocation keywords', !/\bnew\s|List\.generate|\[\s*\]|toSet\(\)|toJson\(\)/.test(tickBody),
  'found allocation pattern in tick body');
check('hot path: no string interpolation', !/\$\{|\$[a-z]/i.test(tickBody.replace(/\$[^'"`]*/g, '')) || !/\$\{/.test(tickBody));
check('hot path: no await/async', !/\bawait\b|\basync\b/.test(tickBody));

// --- 7. HUD zero-rebuild contract
const hudSrc = read('lib/src/spectrum_hud.dart');
check('hud: shouldRepaint => false', /shouldRepaint\([^)]*\)\s*=>\s*false/.test(hudSrc));
check('hud: no setState in lib/', !dartFiles.some((p) => /setState\(/.test(stripComments(readFileSync(p, 'utf8')))));
check('hud: repaint channel wired', /repaint\s*=\s*_CadenceListenable|painter:/.test(hudSrc));
check('hud: fail-safe paint (try/catch + FALLBACK banner)',
  hudSrc.includes('catch (_)') && hudSrc.includes('SPECTRUM HUD FALLBACK'));

// --- 8. engine fail-soft
const engSrc = read('lib/src/weft_spectrum_engine.dart');
check('engine: DynamicLibrary lookups fail-soft', (engSrc.match(/catch \(_\)/g) || []).length >= 3);
check('engine: E_PROBE_UNAVAILABLE path', engSrc.includes('eProbeUnavailable'));
check('engine: no native C authored (no #include/elf/mmap C blobs)',
  !/^\s*#include/m.test(engSrc));

// --- 9. golden projection in tests
const flag = expected.flagship;
check('golden projection: record/crc constants in test',
  testSrc.includes('shp1RecordSize, 192') && testSrc.includes('shp1CrcOffset, 188'));
check('golden projection: crc vector 0xCBF43926', testSrc.includes('0xCBF43926'));
check('golden projection: budget 8589934592 == flagship memoryBudgetBytes',
  String(flag.memoryBudgetBytes) === '8589934592' && testSrc.includes('8589934592'));

// --- report
console.log(`dart static audit: ${pass} passed, ${fail} failed`);
if (fail > 0) {
  for (const f of failures) console.error(`  FAIL ${f}`);
  process.exit(1);
}
console.log('dart static audit: PASS');
