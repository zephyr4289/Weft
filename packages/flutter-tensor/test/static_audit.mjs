#!/usr/bin/env node
// static_audit.mjs — sandbox-mirrored structural self-check for
// packages/flutter-tensor (this sandbox has NO Dart SDK — the same approach
// as Pillar 1's flutter_weft structural mirror).
//
// Run from the package dir:  node test/static_audit.mjs
// Exit 0 only if EVERY check is green; one line per check.
//
// What is enforced, mechanically:
//   1. file inventory (all deliverables exist, non-empty)
//   2. pubspec contract (name / description / sdk / ffi)
//   3. ring_header.dart purity (no flutter, no dart:ffi — runs on plain VM)
//   4. Law 2 ENDIAN DISCIPLINE: every multi-byte ByteData access in every
//      .dart file passes Endian.little explicitly (repo CI-failing crime)
//   5. constants parity vs packages/weft-tensor/src/layout.js (NORMATIVE)
//   6. CRC known-answers re-derived with node:zlib and diffed against the
//      values embedded in test/ring_logic_test.dart (no guessed vectors)
//   7. CRC implementation shape (poly 0xEDB88320, region [0,96)+[104,112))
//   8-10. Law 1 hot-path scans: paint() / _onTick / frame() free of
//      List.generate, new List(, .map(, allocations; no setState in tick
//   11. shouldRepaint => false (notifier drives repaints)
//   12. ticker lifecycle (Single Ticker, stopped + disposed)
//   13. seqlock acquire protocol (bounded retry, validation, counters,
//       zero-alloc bodies)
//   14. producer publish order (COMMITTED clear -> fields -> set ->
//       seq hi -> seq lo LAST)
//   15. notifier zero-alloc storage (fixed fill + swap-remove)
//   16. export barrel completeness
//   17. test purity (ring_logic_test.dart has no flutter/ffi imports)
//   18. example wiring (explicit LE, attachByteData, OverlayScratch)

import { readFileSync, existsSync, statSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import zlib from 'node:zlib';
import * as layout from '../../weft-tensor/src/layout.js';

const PKG = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WEFT_TENSOR_SRC = path.resolve(PKG, '..', 'weft-tensor', 'src');

let pass = 0;
let fail = 0;
const failures = [];

function check(ok, label) {
  if (ok) {
    pass++;
    console.log(`PASS  ${label}`);
  } else {
    fail++;
    failures.push(label);
    console.log(`FAIL  ${label}`);
  }
}

function read(rel) {
  const p = path.join(PKG, rel);
  return existsSync(p) ? readFileSync(p, 'utf8') : null;
}

/** Strip // and /* *\/ comments (Dart) so scans see code only. */
function stripComments(src) {
  return src
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/\/\/[^\n]*/g, '');
}

/** Extract the body of function/method `name(...)` via brace matching. */
function extractFn(src, name) {
  const sig = src.indexOf(name);
  if (sig < 0) return null;
  const open = src.indexOf('{', src.indexOf(')', sig));
  if (open < 0) return null;
  let depth = 0;
  for (let i = open; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}') {
      depth--;
      if (depth === 0) return src.slice(open, i + 1);
    }
  }
  return null;
}

const FORBIDDEN_HOT = ['List.generate', 'new List(', '.map(', '.toList(', 'List<', 'ByteData(', 'Uint8List(', '.sublist('];

function containsAny(body, needles) {
  return needles.filter((n) => body.includes(n));
}

// ---------------------------------------------------------------------------
// 1. File inventory
// ---------------------------------------------------------------------------
const REQUIRED = [
  'pubspec.yaml',
  'README.md',
  'lib/weft_flutter_tensor.dart',
  'lib/src/ring_header.dart',
  'lib/src/weft_ring.dart',
  'lib/src/weft_tensor_view.dart',
  'lib/src/weft_tensor_notifier.dart',
  'lib/src/weft_tensor_painter.dart',
  'lib/src/weft_video_overlay.dart',
  'example/lib/main.dart',
  'test/ring_logic_test.dart',
  'test/static_audit.mjs',
];
{
  const missing = REQUIRED.filter((f) => {
    const p = path.join(PKG, f);
    return !existsSync(p) || statSync(p).size < 32;
  });
  check(missing.length === 0,
    `C01 file inventory (${REQUIRED.length} files, ${missing.length} missing/empty${missing.length ? ': ' + missing.join(',') : ''})`);
}

// ---------------------------------------------------------------------------
// 2. pubspec contract
// ---------------------------------------------------------------------------
{
  const ps = read('pubspec.yaml') ?? '';
  const okName = /^name:\s*weft_flutter_tensor$/m.test(ps);
  const okDesc = /zero-GC AI overlay/.test(ps);
  const okSdk = /sdk:\s*">=3\.4\.0 <4\.0\.0"/.test(ps);
  const okFfi = /ffi:\s*\^1\.0\.0/.test(ps);
  const okFlutter = /flutter:\s*\n\s*sdk:\s*flutter/.test(ps);
  check(okName && okDesc && okSdk && okFfi && okFlutter,
    `C02 pubspec: name=${okName} desc(zero-GC)=${okDesc} sdk=">=3.4.0 <4.0.0"=${okSdk} ffi^1.0.0=${okFfi} flutter-dep=${okFlutter}`);
}

// ---------------------------------------------------------------------------
// 3. ring_header.dart purity (plain-VM law)
// ---------------------------------------------------------------------------
{
  const rh = stripComments(read('lib/src/ring_header.dart') ?? '');
  const clean = !/package:flutter/.test(rh) && !/dart:ffi/.test(rh) && /dart:typed_data/.test(rh);
  check(clean, 'C03 ring_header.dart is pure Dart (no flutter/ffi imports, dart:typed_data only)');
}

// ---------------------------------------------------------------------------
// 4. Law 2 — explicit Endian.little on EVERY multi-byte ByteData access
// ---------------------------------------------------------------------------
{
  const dartFiles = [];
  for (const dir of ['lib/src', 'example/lib', 'test']) {
    const abs = path.join(PKG, dir);
    if (existsSync(abs)) {
      for (const f of readdirSync(abs)) {
        if (f.endsWith('.dart')) dartFiles.push(path.join(abs, f));
      }
    }
  }
  let accesses = 0;
  const violations = [];
  for (const f of dartFiles) {
    const rel = path.relative(PKG, f);
    const src = stripComments(readFileSync(f, 'utf8'));
    const re = /\.(get|set)(Uint|Int|Float)(16|32|64)\s*\(/g;
    let m;
    while ((m = re.exec(src)) !== null) {
      accesses++;
      // Extract the balanced-paren argument list (multi-line safe).
      let depth = 0, i = re.lastIndex - 1;
      for (; i < src.length; i++) {
        if (src[i] === '(') depth++;
        else if (src[i] === ')') {
          depth--;
          if (depth === 0) break;
        }
      }
      const args = src.slice(re.lastIndex, i);
      if (!/Endian\.little/.test(args)) violations.push(`${rel}:${src.slice(0, m.index).split('\n').length}`);
    }
  }
  check(violations.length === 0 && accesses >= 40,
    `C04 Law2 endian discipline: ${accesses} multi-byte accesses scanned, ${violations.length} without Endian.little${violations.length ? ' -> ' + violations.join(', ') : ''}`);
}

// ---------------------------------------------------------------------------
// 5. Constants parity vs packages/weft-tensor/src/layout.js (NORMATIVE)
// ---------------------------------------------------------------------------
{
  const dart = stripComments(read('lib/src/ring_header.dart') ?? '');
  const scalarMap = {
    LAYOUT_VERSION: 'layoutVersion',
    RING_HEADER_SIZE: 'ringHeaderSize',
    SLOT_HEADER_SIZE: 'slotHeaderSize',
    OFF_MAGIC: 'offMagic',
    OFF_LAYOUT_VERSION: 'offLayoutVersion',
    OFF_HEADER_SIZE: 'offHeaderSize',
    OFF_SLOT_COUNT: 'offSlotCount',
    OFF_SLOT_STRIDE: 'offSlotStride',
    OFF_DTYPE_CODE: 'offDtypeCode',
    OFF_DTYPE_BITS: 'offDtypeBits',
    OFF_LANES: 'offLanes',
    OFF_ELEM_SIZE: 'offElemSize',
    OFF_SHAPE: 'offShape',
    OFF_STRIDES: 'offStrides',
    OFF_SCHEMA_ID: 'offSchemaId',
    OFF_PRODUCER_SEQ: 'offProducerSeq',
    OFF_TICK_HZ: 'offTickHz',
    OFF_FLAGS: 'offFlags',
    OFF_HEADER_CRC: 'offHeaderCrc',
    RING_FLAG_LITTLE_ENDIAN: 'ringFlagLittleEndian',
    RING_FLAG_SHARED_MEMORY: 'ringFlagSharedMemory',
    SOFF_MAGIC: 'soffMagic',
    SOFF_PAYLOAD_LEN: 'soffPayloadLen',
    SOFF_SEQ: 'soffSeq',
    SOFF_TIMESTAMP_NS: 'soffTimestampNs',
    SOFF_DURATION_US: 'soffDurationUs',
    SOFF_SLOT_FLAGS: 'soffSlotFlags',
    SOFF_FOURCC: 'soffFourcc',
    SOFF_RANK: 'soffRank',
    SOFF_PLANES: 'soffPlanes',
    SOFF_PLANE_OFFSET: 'soffPlaneOffset',
    SOFF_PLANE_SIZE: 'soffPlaneSize',
    SLOT_FLAG_COMMITTED: 'slotFlagCommitted',
    MAX_RANK: 'maxRank',
  };
  const dlMap = { INT: 'dlPackInt', UINT: 'dlPackUInt', FLOAT: 'dlPackFloat', BFLOAT: 'dlPackBFloat', COMPLEX: 'dlPackComplex', BOOL: 'dlPackBool' };
  const diffs = [];
  let compared = 0;
  const dartConst = (name) => {
    const m = dart.match(new RegExp(`const int ${name} = (0x[0-9a-fA-F]+|\\d+);`));
    return m ? Number(m[1]) : null;
  };
  for (const [ts, dartName] of Object.entries(scalarMap)) {
    compared++;
    if (layout[ts] !== dartConst(dartName)) diffs.push(`${ts}!=${dartName}`);
  }
  for (const [ts, dartName] of Object.entries(dlMap)) {
    compared++;
    if (layout.DLPackCode[ts] !== dartConst(dartName)) diffs.push(`DLPackCode.${ts}!=${dartName}`);
  }
  // Magic byte arrays.
  const magicArr = (name) => {
    const m = dart.match(new RegExp(`const List<int> ${name} = <int>\\[([^\\]]+)\\];`));
    return m ? m[1].split(',').map((s) => parseInt(s.trim(), 16)) : null;
  };
  compared += 2;
  const ringMagic = magicArr('ringMagicBytes');
  const slotMagic = magicArr('slotMagicBytes');
  if (!ringMagic || ringMagic.length !== 4 || layout.RING_MAGIC.some((v, i) => v !== ringMagic[i])) diffs.push('RING_MAGIC');
  if (!slotMagic || slotMagic.length !== 4 || layout.SLOT_MAGIC.some((v, i) => v !== slotMagic[i])) diffs.push('SLOT_MAGIC');
  check(diffs.length === 0 && compared >= 42,
    `C05 constants parity vs layout.js: ${compared} compared, ${diffs.length} diffs${diffs.length ? ' -> ' + diffs.join(', ') : ''}`);
}

// ---------------------------------------------------------------------------
// 6. CRC known-answers re-derived from node:zlib (no guessed vectors)
// ---------------------------------------------------------------------------
{
  const test = read('test/ring_logic_test.dart') ?? '';
  const kat1 = zlib.crc32(Buffer.from([0x57, 0x45, 0x46, 0x54])); // "WEFT"
  const kat3 = zlib.crc32(Buffer.from(Array.from({ length: 256 }, (_, i) => i)));
  // Rebuild the EXACT KAT2 header the Dart test builds (mirrored fields).
  const h = Buffer.alloc(128, 0);
  h[0] = 0x57; h[1] = 0x45; h[2] = 0x46; h[3] = 0x54;
  h.writeUInt16LE(1, 4);
  h.writeUInt16LE(128, 6);
  h.writeUInt32LE(4, 8);
  h.writeUInt32LE(128, 12);
  h[16] = 1; h[17] = 8;
  h.writeUInt16LE(1, 18);
  h.writeUInt32LE(1, 20);
  h.writeUInt32LE(2, 24); h.writeUInt32LE(3, 28);
  h.writeUInt32LE(3, 56); h.writeUInt32LE(1, 60);
  h.writeUInt32LE(0x55667788, 88); h.writeUInt32LE(0x11223344, 92);
  h.writeUInt32LE(120, 104);
  h.writeUInt32LE(1, 108);
  const kat2 = zlib.crc32(Buffer.concat([h.subarray(0, 96), h.subarray(104, 112)]));
  const embedded = (name) => {
    const m = test.match(new RegExp(`const int ${name} = (\\d+);`));
    return m ? Number(m[1]) : null;
  };
  const ok1 = embedded('katCrcWeftBytes') === kat1;
  const ok2 = embedded('katCrcHeaderRegion') === kat2;
  const ok3 = embedded('katCrcAscending256') === kat3;
  check(ok1 && ok2 && ok3,
    `C06 CRC KATs vs node:zlib: WEFT=${kat1}(0x${kat1.toString(16).toUpperCase()})=${ok1}, headerRegion=${kat2}(0x${kat2.toString(16).toUpperCase()})=${ok2}, asc256=${kat3}=${ok3}`);
}

// ---------------------------------------------------------------------------
// 7. CRC implementation shape (poly + region bounds)
// ---------------------------------------------------------------------------
{
  const dart = stripComments(read('lib/src/ring_header.dart') ?? '');
  const crcBody = extractFn(dart, 'ringHeaderCrc') ?? '';
  const poly = /0xEDB88320/i.test(stripComments(read('lib/src/ring_header.dart') ?? ''));
  const region = crcBody.includes('i < 96') && crcBody.includes('i = 104') && crcBody.includes('i < 112');
  const xorOut = crcBody.includes('^ 0xffffffff');
  check(poly && region && xorOut,
    `C07 CRC impl: poly 0xEDB88320=${poly}, region [0,96)+[104,112)=${region}, xor-out=${xorOut}`);
}

// ---------------------------------------------------------------------------
// 8-10. Law 1 — hot-path allocation scans
// ---------------------------------------------------------------------------
{
  const painter = stripComments(read('lib/src/weft_tensor_painter.dart') ?? '');
  const paintBody = extractFn(painter, 'void paint(') ?? '';
  const paintHits = containsAny(paintBody, FORBIDDEN_HOT);
  check(paintHits.length === 0 && paintBody.includes('acquireLatest'),
    `C08 painter.paint() hot path: ${paintHits.length} forbidden patterns${paintHits.length ? ' -> ' + paintHits.join(',') : ''}, deferred acquire=${paintBody.includes('acquireLatest')}`);

  const overlay = stripComments(read('lib/src/weft_video_overlay.dart') ?? '');
  const tickBody = extractFn(overlay, 'void _onTick(') ?? '';
  const tickHits = containsAny(tickBody, FORBIDDEN_HOT);
  const noSetState = !tickBody.includes('setState');
  check(tickHits.length === 0 && noSetState && tickBody.includes('acquireLatest') && tickBody.includes('.frame()'),
    `C09 overlay._onTick hot path: ${tickHits.length} forbidden patterns, setState=${!noSetState}, acquire+notify=${tickBody.includes('acquireLatest') && tickBody.includes('.frame()')}`);

  const notifier = stripComments(read('lib/src/weft_tensor_notifier.dart') ?? '');
  const frameBody = extractFn(notifier, 'void frame()') ?? '';
  const frameHits = containsAny(frameBody, [...FORBIDDEN_HOT, '.add(']);
  check(frameHits.length === 0 && frameBody.includes('for (var i = 0;'),
    `C10 notifier.frame() hot path: ${frameHits.length} forbidden patterns, indexed-loop=${frameBody.includes('for (var i = 0;')}`);
}

// ---------------------------------------------------------------------------
// 11. shouldRepaint => false
// ---------------------------------------------------------------------------
{
  const painter = stripComments(read('lib/src/weft_tensor_painter.dart') ?? '');
  // shouldRepaint may be expression-bodied (=> false) — match both forms.
  const ok = /bool shouldRepaint\([^)]*\)\s*(=>\s*false|\{\s*return false;?\s*\})/.test(painter);
  check(ok, 'C11 WeftTensorPainter.shouldRepaint => false (notifier-driven)');
}

// ---------------------------------------------------------------------------
// 12. Ticker lifecycle
// ---------------------------------------------------------------------------
{
  const overlay = read('lib/src/weft_video_overlay.dart') ?? '';
  const single = overlay.includes('SingleTickerProviderStateMixin');
  const created = overlay.includes('createTicker(');
  const disposeBody = extractFn(stripComments(overlay), 'void dispose()') ?? '';
  check(single && created && disposeBody.includes('.stop()') && disposeBody.includes('.dispose()'),
    `C12 ticker lifecycle: SingleTickerProviderStateMixin=${single}, createTicker=${created}, stop+dispose in dispose()=${disposeBody.includes('.stop()') && disposeBody.includes('.dispose()')}`);
}

// ---------------------------------------------------------------------------
// 13. Seqlock acquire protocol (bounded retry, validation, zero alloc)
// ---------------------------------------------------------------------------
{
  const ring = stripComments(read('lib/src/weft_ring.dart') ?? '');
  const latest = extractFn(ring, 'WeftTensorView? acquireLatest(') ?? '';
  const frame = extractFn(ring, 'WeftTensorView? acquireFrame(') ?? '';
  const lHits = containsAny(latest, FORBIDDEN_HOT);
  const fHits = containsAny(frame, FORBIDDEN_HOT);
  const bounded = latest.includes('attempt') && latest.includes('< sc');
  const validated = latest.includes('readSlotHeader') && latest.includes('_meta.seq == s');
  const counters = latest.includes('stats.tornReads++') && latest.includes('stats.overruns++');
  const flyweight = latest.includes('_view.bind(') && !latest.includes('WeftTensorView(');
  check(lHits.length === 0 && bounded && validated && counters && flyweight,
    `C13 acquireLatest seqlock: alloc-patterns=${lHits.length}, boundedRetry=${bounded}, magic+commit+seq validation=${validated}, torn/overrun counters=${counters}, reused flyweight=${flyweight}`);
  check(fHits.length === 0 && frame.includes('readSlotHeader') && frame.includes('_meta.seq == seq'),
    `C13b acquireFrame: alloc-patterns=${fHits.length}, validated=${frame.includes('readSlotHeader') && frame.includes('_meta.seq == seq')}`);
}

// ---------------------------------------------------------------------------
// 14. Producer publish order (WTR1 §4 / ring.js parity)
// ---------------------------------------------------------------------------
{
  const ring = stripComments(read('lib/src/weft_ring.dart') ?? '');
  const finish = extractFn(ring, 'int finishCommit(') ?? '';
  const flagsClear = finish.indexOf('soffSlotFlags, 0,');
  const flagsCommit = finish.indexOf('soffSlotFlags, slotFlagCommitted,');
  const seqHi = finish.indexOf('setUint32(offProducerSeq + 4');
  const seqLo = finish.indexOf('setUint32(offProducerSeq,');
  const orderOk = flagsClear >= 0 && flagsCommit > flagsClear && seqHi >= 0 && seqLo > seqHi;
  check(orderOk, `C14 finishCommit publish order: COMMITTED clear->set=${flagsClear >= 0 && flagsCommit > flagsClear}, seq hi->lo-LAST=${seqHi >= 0 && seqLo > seqHi}`);
}

// ---------------------------------------------------------------------------
// 15. Notifier zero-alloc storage
// ---------------------------------------------------------------------------
{
  const notifier = stripComments(read('lib/src/weft_tensor_notifier.dart') ?? '');
  const fixed = notifier.includes('List<VoidCallback?>.filled(');
  const swapRemove = notifier.includes('_listeners[i] = _listeners[_count]');
  const noGrowableAdd = !extractFn(notifier, 'void addListener(')?.includes('.add(');
  check(fixed && swapRemove && noGrowableAdd,
    `C15 notifier storage: fixed-capacity fill=${fixed}, swap-remove=${swapRemove}, no growable .add=${noGrowableAdd}`);
}

// ---------------------------------------------------------------------------
// 16. Export barrel completeness
// ---------------------------------------------------------------------------
{
  const barrel = read('lib/weft_flutter_tensor.dart') ?? '';
  const expected = ['ring_header.dart', 'weft_ring.dart', 'weft_tensor_view.dart', 'weft_tensor_notifier.dart', 'weft_tensor_painter.dart', 'weft_video_overlay.dart'];
  const missing = expected.filter((f) => !barrel.includes(`export 'src/${f}'`));
  check(missing.length === 0, `C16 export barrel: ${expected.length} exports, ${missing.length} missing${missing.length ? ' -> ' + missing.join(',') : ''}`);
}

// ---------------------------------------------------------------------------
// 17. Test purity
// ---------------------------------------------------------------------------
{
  const t = stripComments(read('test/ring_logic_test.dart') ?? '');
  const clean = !/package:flutter/.test(t) && !/dart:ffi/.test(t) && !/package:test/.test(t);
  const kats = t.includes('katCrcWeftBytes') && t.includes('katCrcHeaderRegion') && t.includes('katCrcAscending256');
  check(clean && kats, `C17 ring_logic_test.dart: pure Dart (no flutter/ffi/test pkg)=${clean}, zlib-derived KATs embedded=${kats}`);
}

// ---------------------------------------------------------------------------
// 18. Example wiring
// ---------------------------------------------------------------------------
{
  const ex = stripComments(read('example/lib/main.dart') ?? '');
  const le = (ex.match(/Endian\.little/g) ?? []).length;
  const attach = ex.includes('WeftRing.attachByteData(');
  const scratch = ex.includes('OverlayScratch(');
  const overlay = ex.includes('WeftVideoOverlay(');
  const noFrameSetState = !(extractFn(ex, 'Widget build(') ?? '').includes('setState');
  check(le >= 8 && attach && scratch && overlay && noFrameSetState,
    `C18 example wiring: explicit-LE x${le}, attachByteData=${attach}, OverlayScratch=${scratch}, WeftVideoOverlay=${overlay}, build()-setState-free=${noFrameSetState}`);
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('');
console.log(`static_audit: ${pass} passed, ${fail} failed (layout.js: ${WEFT_TENSOR_SRC})`);
process.exitCode = fail === 0 ? 0 : 1;
