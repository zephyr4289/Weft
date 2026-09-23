// audit/static_audit.mjs — structural audit of the Swift package (sandbox has
// no swiftc; the XCTest battery is CI-gated on the Apple lane — this audit is
// the always-runnable local proof layer).
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const PKG = join(HERE, '..');
const CORE = join(PKG, '../heddle-core/src/layout.js');

let pass = 0, fail = 0;
const ok = (cond, label) => {
  if (cond) { pass++; console.log(`  ok  ${label}`); }
  else { fail++; console.log(`FAIL  ${label}`); }
};

const src = (p) => readFileSync(p, 'utf8');
const hotPlane = src(join(PKG, 'Sources/WeftSwiftUI/HotPlane.swift'));
const model = src(join(PKG, 'Sources/WeftSwiftUI/HotPlaneModel.swift'));
const metal = src(join(PKG, 'Sources/WeftSwiftUI/MetalHuddleView.swift'));
const js = src(CORE);

// 1. constants parity vs layout.js
const swiftConst = (name) => {
  const m = hotPlane.match(new RegExp(`public static let ${name}(?::\\s*\\w+)? = (0x[0-9A-Fa-f_]+|\\d+)`));
  return m ? Number(m[1].replace(/_/g, '')) : undefined;
};
const jsConst = (name) => {
  const m = js.match(new RegExp(`export const ${name} = (0x[0-9a-fA-F]+|\\d+);`)) ||
            js.match(new RegExp(`${name}:\\s*(0x[0-9a-fA-F]+|\\d+)`));
  return m ? Number(m[1].replace(/_/g, '')) : undefined;
};
const PAIRS = [
  ['HEADER_SIZE', 'headerSize'], ['LANE_CTRL_STRIDE', 'laneCtrlStride'],
  ['MAGIC_U32', 'magicU32'], ['VERSION', 'version'],
  ['FLAG_LE_REQUIRED', 'flagLeRequired'], ['LANE_FLAG_ACTIVE', 'laneFlagActive'],
  ['MAX_LANES', 'maxLanes'],
];
for (const [j, s] of PAIRS) {
  ok(jsConst(j) !== undefined && jsConst(j) === swiftConst(s), `parity ${j} == ${s} == ${jsConst(j)}`);
}
const hdrJs = js.match(/export const HDR = \{([\s\S]*?)\};/)[1];
const laneJs = js.match(/export const LANE = \{([\s\S]*?)\};/)[1];
const camel = (s) => s.toLowerCase().replace(/_([a-z0-9])/g, (_, c) => c.toUpperCase());
let hdrOk = true, laneOk = true, hdrCount = 0, laneCount = 0;
for (const [, name, hex] of hdrJs.matchAll(/([A-Z_0-9]+):\s*(0x[0-9a-fA-F]+)/g)) {
  hdrCount++;
  const key = 'hdr' + camel(name)[0].toUpperCase() + camel(name).slice(1);
  if (swiftConst(key) !== Number(hex)) { hdrOk = false; console.log(`      hdr drift ${name}: js=${hex} swift=${swiftConst(key)}`); }
}
for (const [, name, hex] of laneJs.matchAll(/([A-Z_0-9]+):\s*(0x[0-9a-fA-F]+)/g)) {
  laneCount++;
  const key = 'lane' + camel(name)[0].toUpperCase() + camel(name).slice(1);
  if (swiftConst(key) !== Number(hex)) { laneOk = false; console.log(`      lane drift ${name}: js=${hex} swift=${swiftConst(key)}`); }
}
ok(hdrOk && hdrCount >= 20, `header offsets parity (${hdrCount} fields)`);
ok(laneOk && laneCount >= 10, `lane offsets parity (${laneCount} fields)`);

// 2. explicit little-endian: every multi-byte load passes .littleEndian
const loads = [...hotPlane.matchAll(/loadUnaligned\(fromByteOffset:[^)]+as:\s*UInt(16|32|64)\.self\)(\.\w+)?/g)];
const nonLE = loads.filter((m) => m[2] !== '.littleEndian');
ok(loads.length >= 25 && nonLE.length === 0, `explicit .littleEndian on ${loads.length - nonLE.length}/${loads.length} loads`);

// 3. hot-path purity: draw(in:) allocates nothing
const drawBody = metal.match(/public func draw\(in view: MTKView\) \{([\s\S]*?)\n    \}/)[1];
ok(!/\[Double\]\(|\[UInt32\]\(|Array\(|\.map\(|\.filter\(|makeBuffer\(/.test(drawBody), 'draw(in:): zero allocation patterns');
ok(/readRecent\(lane, capacity, window\)/.test(drawBody), 'draw(in:) reads the plane window directly');
ok(/enc\.endEncoding\(\)[\s\S]*cmd\.present\(drawable\)[\s\S]*cmd\.commit\(\)/.test(drawBody), 'Metal ordering: endEncoding → present → commit');
ok(/MTKViewDelegate/.test(metal), 'HuddleRenderer is an MTKViewDelegate');
ok(/makeBuffer\(length: capacity \* MemoryLayout<Float>\.stride/.test(metal), 'vertex MTLBuffer preallocated once');

// 4. model: @Observable + pump purity + taxonomy
ok(/@Observable/.test(model), 'HotPlaneModel is @Observable');
const pumpBody = model.match(/public func pump\(\) \{([\s\S]*?)\n    \}/)[1];
ok(!/WeftLaneSnapshot\(\)|WeftHeaderSnapshot\(\)|Array\(repeating/.test(pumpBody), 'pump(): snapshots preallocated (no per-pump alloc)');
ok(/lastEvent == \.ok \? "OK"|epochChanged/.test(model) || /lastEvent/.test(model), 'Law-4 event surface present');
ok(/case ringUnderrun = 14/.test(hotPlane) && /case ok = 0/.test(hotPlane), '15-code taxonomy endpoints present');

// 5. package shape
const pkg = src(join(PKG, 'Package.swift'));
ok(/name: "WeftSwiftUI"/.test(pkg) && /\.macOS\(\.v14\)/.test(pkg), 'Package.swift platforms + target');
const tests = src(join(PKG, 'Tests/WeftSwiftUITests/HotPlaneTests.swift'));
ok(/XCTAssertEqual\(HPL1Code\.allCases\.count, 15\)/.test(tests), 'XCTest asserts taxonomy completeness');
ok(/func testTornSeqlockNeverReturnsValue/.test(tests), 'XCTest covers torn-seqlock Law-4 behavior');

console.log(`swift_audit: ${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
