// test/static_audit.mjs — mechanical structural audit of the flutter-heddle
// package (sandbox has NO Dart SDK; the Dart harness is CI-gated on the
// flutter lane — this audit is the local, always-runnable proof layer).
//
// Checks: constants byte-parity vs heddle-core layout.js, Endian.little on
// EVERY multi-byte accessor in every .dart file, hot-path purity (paint /
// _onTick / pump: no allocations, no setState, shouldRepaint => false),
// fixed-storage notifier, barrel/example wiring, pubspec contract.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const HERE = dirname(fileURLToPath(import.meta.url));
const PKG = join(HERE, '..');
const CORE = join(PKG, '../heddle-core/src/layout.js');

let pass = 0, fail = 0;
const ok = (cond, label) => {
  if (cond) { pass++; console.log(`  ok  ${label}`); }
  else { fail++; console.log(`FAIL  ${label}`); }
};

function listDart(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...listDart(p));
    else if (name.endsWith('.dart')) out.push(p);
  }
  return out;
}

// ---- 1. constants parity vs layout.js ----
const js = readFileSync(CORE, 'utf8');
const dartLayout = readFileSync(join(PKG, 'lib/src/hpl1_layout.dart'), 'utf8');

function jsConst(name) {
  const m = js.match(new RegExp(`export const ${name} = (0x[0-9a-fA-F]+|\\d+);`)) ||
            js.match(new RegExp(`${name}:\\s*(0x[0-9a-fA-F]+|\\d+)`));
  return m ? Number(m[1]) : undefined;
}
function dartConst(name) {
  const m = dartLayout.match(new RegExp(`static const int ${name} = (0x[0-9a-fA-F]+|\\d+);`));
  return m ? Number(m[1]) : undefined;
}

const PARITY = [
  ['HEADER_SIZE', 'headerSize'], ['LANE_CTRL_STRIDE', 'laneCtrlStride'],
  ['MAGIC_U32', 'magicU32'], ['VERSION', 'version'],
  ['FLAG_LE_REQUIRED', 'flagLeRequired'], ['FLAG_EPOCH_STABLE', 'flagEpochStable'],
  ['LANE_FLAG_ACTIVE', 'laneFlagActive'], ['LANE_FLAG_MANUAL', 'laneFlagManual'],
  ['MAX_LANES', 'maxLanes'],
];
for (const [j, d] of PARITY) {
  ok(jsConst(j) !== undefined && jsConst(j) === dartConst(d), `parity ${j} == ${d} == ${jsConst(j)}`);
}
// header + lane offset tables (all numeric members of HDR/LANE in both files)
const hdrJs = js.match(/export const HDR = \{([\s\S]*?)\};/)[1];
const laneJs = js.match(/export const LANE = \{([\s\S]*?)\};/)[1];
const hdrPairs = [...hdrJs.matchAll(/([A-Z_0-9]+):\s*(0x[0-9a-fA-F]+)/g)];
const lanePairs = [...laneJs.matchAll(/([A-Z_0-9]+):\s*(0x[0-9a-fA-F]+)/g)];
ok(hdrPairs.length >= 20, `HDR table has ${hdrPairs.length} offsets in layout.js`);
// SNAKE_CASE → camelCase: PUBLISH_SEQ → publishSeq, RESERVED0 → reserved0
const camel = (s) => s.toLowerCase().replace(/_([a-z0-9])/g, (_, c) => c.toUpperCase());
const dartKey = (prefix, name) => prefix + camel(name)[0].toUpperCase() + camel(name).slice(1);
let hdrParityOk = true, laneParityOk = true;
for (const [, name, hex] of hdrPairs) {
  const dv = dartConst(dartKey('hdr', name));
  if (dv === undefined || dv !== Number(hex)) { hdrParityOk = false; console.log(`      hdr drift: ${name} js=${hex} dart=${dv}`); }
}
for (const [, name, hex] of lanePairs) {
  const dv = dartConst(dartKey('lane', name));
  if (dv === undefined || dv !== Number(hex)) { laneParityOk = false; console.log(`      lane drift: ${name} js=${hex} dart=${dv}`); }
}
ok(hdrParityOk, `header offsets parity (${hdrPairs.length} fields)`);
ok(laneParityOk, `lane offsets parity (${lanePairs.length} fields)`);
ok(dartConst('hdrGlobalMin') === 0x40 && dartConst('laneHead') === 0x30, 'spot-check hdrGlobalMin/laneHead');

// ---- 2. Endian.little on every multi-byte accessor in every .dart file ----
const dartFiles = listDart(join(PKG, 'lib')).concat(listDart(join(PKG, 'test')).filter((p) => p.endsWith('.dart')));
const ACCESSOR = /\.(get|set)(Uint16|Uint32|Int32|Int64|Uint64|Float32|Float64)\(([^;]*)\)/g;
let total = 0, explicit = 0;
for (const f of dartFiles) {
  const src = readFileSync(f, 'utf8');
  for (const m of src.matchAll(ACCESSOR)) {
    total++;
    if (m[3].includes('Endian.little')) explicit++;
    else console.log(`      non-explicit LE at ${f}:${src.slice(0, m.index).split('\n').length}`);
  }
}
ok(total > 0 && total === explicit, `Endian.little explicit on ${explicit}/${total} multi-byte accesses`);

// ---- 3. hot-path purity ----
const widgetSrc = readFileSync(join(PKG, 'lib/src/weft_canvas_widget.dart'), 'utf8');
const paintBody = widgetSrc.match(/void paint\(Canvas canvas, Size size\) \{([\s\S]*?)\n  \}/);
ok(paintBody !== null, 'paint() body extracted');
ok(!/setState|List\.generate|new List|\.map\(|new Float|new Uint/.test(paintBody[1]), 'paint(): no setState / no allocation patterns');
ok(/bool shouldRepaint\([^)]*\)\s*=>\s*false/.test(widgetSrc), 'shouldRepaint => false (repaints via repaint: notifier)');
ok(/super\(repaint: notifier\)/.test(widgetSrc), 'painter wired to repaint: notifier');
const tickBody = widgetSrc.match(/void _onTick\(Duration elapsed\) \{([\s\S]*?)\n  \}/);
ok(tickBody !== null && !_onTickAllocs(tickBody[1]), '_onTick: no allocation patterns');
function _onTickAllocs(body) { return /List\.generate|new List|\.map\(|new Float|new Uint|\{\}/.test(body.replace(/\{\s*\}/g, '{}')); }

const notifierSrc = readFileSync(join(PKG, 'lib/src/weft_hot_plane_notifier.dart'), 'utf8');
const pumpBody = notifierSrc.match(/void pump\(\) \{([\s\S]*?)\n  \}/);
ok(pumpBody !== null && !/new |List\.generate|\.map\(|Float64List\(|Uint32List\(|WeftLaneSnapshot\(/.test(pumpBody[1]), 'pump(): zero allocation (scratch preallocated)');
ok(/notifyListeners\(\)/.test(pumpBody[1]) && /advanced \|\| _epochFlag/.test(pumpBody[1]), 'notifyListeners conditional on seq advance');
ok(/class WeftHotPlaneNotifier extends ChangeNotifier/.test(notifierSrc), 'notifier extends ChangeNotifier (Listenable)');

// ---- 4. layout law scans ----
const layoutSrc = readFileSync(join(PKG, 'lib/src/hpl1_layout.dart'), 'utf8');
ok(!/Random\(|DateTime\.now|Math\.random/.test(layoutSrc + readFileSync(join(PKG, 'lib/src/hot_plane.dart'), 'utf8')), 'no nondeterminism in layout/plane core');
ok(/class Hpl1Exception implements Exception/.test(layoutSrc), 'typed Hpl1Exception present');
ok((layoutSrc.match(/static const int (ok|badMagic|badVersion|notLittleEndian|capacityMismatch|laneOutOfRange|tornSeqlock|epochChanged|planeDetached|contextLost|tabHidden|workerCrash|badRenderEngine|invalidSample|ringUnderrun) = /g) || []).length === 15,
  '15-code Law-4 taxonomy present in Dart');

// ---- 5. test purity + barrel + example ----
const dartTestSrc = readFileSync(join(PKG, 'test/ring_logic_test.dart'), 'utf8');
ok(!/package:flutter|dart:ffi/.test(dartTestSrc), 'ring_logic_test.dart: no flutter/ffi imports (runs on any Dart SDK)');
ok(/exit\(_failed == 0 \? 0 : 1\)/.test(dartTestSrc), 'harness exits nonzero on failure (no silent green)');
const barrel = readFileSync(join(PKG, 'lib/weft_flutter.dart'), 'utf8');
for (const part of ['hpl1_layout', 'hot_plane', 'weft_hot_plane_notifier', 'weft_canvas_widget']) {
  ok(barrel.includes(`src/${part}.dart`), `barrel exports ${part}`);
}
const example = readFileSync(join(PKG, 'example/lib/main.dart'), 'utf8');
ok(/WeftCanvasWidget\(plane: plane/.test(example), 'example wires WeftCanvasWidget');
ok(/import 'package:weft_flutter\/weft_flutter.dart'/.test(example), 'example imports the barrel');
const pubspec = readFileSync(join(PKG, 'pubspec.yaml'), 'utf8');
ok(/name: weft_flutter/.test(pubspec), 'pubspec name');
ok(/sdk: ">=3\.4\.0 <4\.0\.0"/.test(pubspec), 'pubspec sdk constraint');

console.log(`static_audit: ${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
