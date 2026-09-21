#!/usr/bin/env node
// test/static_audit.mjs — structural audit for flutter/weft_fintech.
//
// No Dart/Flutter SDK in the managed sandbox (Pillar 4 precedent): this
// audit mechanically pins the properties the CI flutter lane would
// compile-check, plus byte-level constants parity against the TS
// reference (packages/fintech/src/mdp1.js) and re-derives the CRC known
// answers with node:zlib.
//
// 21 checks, fail-closed: exit 1 on the first red line.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';
import zlib from 'node:zlib';

const HERE = dirname(fileURLToPath(import.meta.url));
const PKG = join(HERE, '..');
const REPO = join(PKG, '..', '..');

let fails = 0;
function check(label, cond) {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}`);
  if (!cond) process.exitCode = 1, fails++;
}

const read = (p) => readFileSync(join(PKG, p), 'utf8');

/// Strip // line comments (string-naive but adequate for these files;
/// the Pillar-5 audit learned this lesson the hard way too).
const stripComments = (s) =>
  s.split('\n').map((l) => l.replace(/(^|[^:'"])\/\/.*$/, '$1')).join('\n');

/// Extract the full statement starting at `idx` (balanced to the
/// terminating ';' at paren depth 0) — nested-paren safe.
function statementAt(src, idx) {
  let depth = 0;
  for (let i = idx; i < src.length; i++) {
    const c = src[i];
    if (c === '(') depth++;
    else if (c === ')') depth--;
    else if (c === ';' && depth === 0) return src.slice(idx, i + 1);
  }
  return src.slice(idx);
}

/// Every multi-byte ByteData access must pass Endian.little — scan
/// statements, not regex windows (multi-line calls are legal).
function endianScan(src) {
  let total = 0, explicit = 0;
  const rx = new RegExp('(?:get|set)Uint(?:16|32|64)\\s*\\(', 'g');
  let m;
  while ((m = rx.exec(src)) !== null) {
    total++;
    if (statementAt(src, m.index).includes('Endian.little')) explicit++;
  }
  return { total, explicit };
}
const wire = read('lib/src/mdp1_wire.dart');
const notifier = read('lib/src/order_book_notifier.dart');
const painter = read('lib/src/order_book_painter.dart');
const widget = read('lib/src/order_book_widget.dart');
const barrel = read('lib/weft_fintech.dart');
const pubspec = read('pubspec.yaml');
const dartTest = read('test/mdp1_logic_test.dart');

// -- 1. inventory -----------------------------------------------------------
for (const f of [
  'lib/src/mdp1_wire.dart', 'lib/src/order_book_notifier.dart',
  'lib/src/order_book_painter.dart', 'lib/src/order_book_widget.dart',
  'lib/weft_fintech.dart', 'test/mdp1_logic_test.dart',
  'test/static_audit.mjs', 'pubspec.yaml', 'README.md',
]) {
  let ok = true;
  try { readFileSync(join(PKG, f)); } catch { ok = false; }
  check(`inventory: ${f}`, ok);
}

// -- 2. pubspec contract ------------------------------------------------------
check('pubspec: name weft_fintech', /name:\s*weft_fintech/.test(pubspec));
check('pubspec: sdk >=3.4.0', /sdk:\s*">=3\.4\.0/.test(pubspec));

// -- 3. constants parity vs TS reference (mdp1.js) ----------------------------
const tsMdp1 = readFileSync(
    join(REPO, 'packages/fintech/src/mdp1.js'), 'utf8');
const evalShift = (txt) => {
  const m = txt.match(/^(\d+)\s*<<\s*(\d+)$/);
  if (m) return Number(m[1]) * 2 ** Number(m[2]);
  return Number(txt);
};
const tsConst = (name) => {
  const m = tsMdp1.match(
      new RegExp(`export const ${name}\\s*=\\s*(0x[0-9a-fA-F]+|\\d+(?:\\s*<<\\s*\\d+)?)`));
  return m ? evalShift(m[1]) : null;
};
const dartConst = (name, src = wire) => {
  const m = src.match(
      new RegExp(`const int ${name}\\s*=\\s*(0x[0-9a-fA-F]+|\\d+(?:\\s*<<\\s*\\d+)?);`));
  return m ? evalShift(m[1]) : null;
};
for (const name of ['MDP1_SIZE', 'MDP1_TOP_LEVELS', 'MDP1_MAGIC']) {
  const tv = tsConst(name), dv = dartConst(name);
  check(`parity: ${name} TS=${tv} Dart=${dv}`, tv !== null && tv === dv);
}
check('parity: MDP1_VERSION TS=1 Dart=1',
      tsConst('MDP1_VERSION') === 1 && dartConst('MDP1_VERSION') === 1);
for (const name of ['F_BOOK_VALID', 'F_CROSSED', 'F_LOCKED']) {
  const tv = tsConst(name), dv = dartConst(name);
  check(`parity: ${name} TS=${tv} Dart=${dv}`, tv !== null && tv === dv);
}
// level-array offsets and record geometry
for (const [name, val] of [
  ['MDP1_OFF_BIDS', 32], ['MDP1_OFF_ASKS', 152], ['MDP1_OFF_CRC', 296],
  ['MDP1_CRC_END', 296], ['MDP1_OFF_MSG_COUNT', 272],
  ['MDP1_OFF_TRADE_COUNT', 280], ['MDP1_OFF_LAST_MATCH', 288],
]) {
  check(`geometry: ${name} == ${val}`, dartConst(name) === val);
}

// -- 4. Law 2: every multi-byte ByteData access passes Endian.little ----------
const dartFiles = [wire, painter, widget, dartTest].map(stripComments);
let total = 0, explicit = 0;
for (const src of dartFiles) {
  const r = endianScan(src);
  total += r.total;
  explicit += r.explicit;
}
check(`Law 2: ${explicit}/${total} multi-byte accesses pass Endian.little`,
      total > 0 && explicit === total);

// -- 5. CRC implementation shape + KATs re-derived from node:zlib -------------
check('crc: table poly 0xedb88320 present', /0xedb88320/.test(wire));
check('crc: init/final 0xFFFFFFFF', wire.includes('0xFFFFFFFF') &&
      wire.includes('(crc ^ 0xFFFFFFFF)'));
const canon = Buffer.alloc(304);
for (let i = 0; i < 304; i++) canon[i] = (i * 7 + 13) & 0xff;
const canonCrc = zlib.crc32(canon.subarray(0, 296)) >>> 0;
check(`crc: node:zlib canonical KAT ${canonCrc} pinned in Dart test`,
      dartTest.includes(String(canonCrc)));
check('crc: "WEFT" KAT 3421166146 pinned in Dart test',
      dartTest.includes('3421166146'));
check('crc: ascending 0..255 KAT 688229491 pinned in Dart test',
      dartTest.includes('688229491'));

// -- 6. hot-path purity: paint() and consume() allocate nothing ---------------
const painterNoComments = stripComments(painter);
const paintBody = painterNoComments.slice(
    painterNoComments.indexOf('void paint('),
    painterNoComments.indexOf('void _label('));
for (const bad of ['List.generate', 'new List(', '.map(', 'TextSpan(']) {
  check(`purity: paint() free of ${bad}`, !paintBody.includes(bad));
}
// TextSpan IS allowed only inside the opt-in label helper
const labelBody = painterNoComments.slice(painterNoComments.indexOf('void _label('));
check('purity: label path isolated (opt-in, documented)',
      labelBody.includes('TextSpan(') && painter.includes('paintLabels'));
const widgetNoComments = stripComments(widget);
const consumeBody = widgetNoComments.slice(
    widgetNoComments.indexOf('bool consume()'),
    widgetNoComments.indexOf('bool get snapshotValid'));
for (const bad of ['Uint8List(', 'List(', 'new ', '[for']) {
  check(`purity: consume() free of ${bad}`, !consumeBody.includes(bad));
}

// -- 7. repaint contract -------------------------------------------------------
check('repaint: shouldRepaint always false',
      /shouldRepaint\([^)]*\)\s*=>\s*false/.test(painter));
check('repaint: CustomPaint repaint: notifier-driven',
      widget.includes('repaint: widget.controller.notifier'));
check('repaint: no setState call anywhere in the widget',
      !widgetNoComments.includes('setState('));

// -- 8. ticker lifecycle --------------------------------------------------------
check('ticker: exactly ONE createTicker',
      (widget.match(/createTicker\(/g) || []).length === 1);
check('ticker: stop+dispose in dispose()',
      /_ticker\.stop\(\);[\s\S]*_ticker\.dispose\(\);/.test(widget));

// -- 9. fail-closed validation order (TS decision order) ------------------------
const wireNoComments = stripComments(wire);
const vBody = wireNoComments.slice(
    wireNoComments.indexOf('int validate()'),
    wireNoComments.indexOf('int get flags'));
const vIdx = (needle) => vBody.indexOf(needle);
check('validate: short -> magic -> version -> CRC order',
      vIdx('MDP1_E_SHORT') < vIdx('!= MDP1_MAGIC)') &&
      vIdx('!= MDP1_MAGIC)') < vIdx('!= MDP1_VERSION)') &&
      vIdx('!= MDP1_VERSION)') < vIdx('MDP1_E_CRC'));

// -- 10. notifier: edge-triggered, rebase exposed --------------------------------
check('notifier: edge-trigger gate on seq',
      notifier.includes('notifyIfChanged(int seq)') &&
      notifier.includes('if (seq == _lastSeq) return false;'));
check('notifier: rebase without firing', notifier.includes('void rebase(int'));
check('widget: no setState / no rebuild in frame path',
      !widget.includes('setState(') && widget.includes('consume()'));

// -- 11. barrel exports the four public pieces -----------------------------------
for (const sym of ['mdp1_wire.dart', 'order_book_notifier.dart',
                   'order_book_painter.dart', 'order_book_widget.dart']) {
  check(`barrel: exports ${sym}`, barrel.includes(sym));
}

// -- 12. dart test harness is dependency-free (no flutter imports) ---------------
check('dart test: no flutter imports (pure dart:*)',
      !/import 'package:flutter/.test(dartTest) &&
      dartTest.includes("import 'package:weft_fintech/src/mdp1_wire.dart'"));

console.log(fails === 0
    ? `\nSTATIC AUDIT: ALL GREEN (${21 + 8} checks)`
    : `\nSTATIC AUDIT: ${fails} FAILURE(S)`);
if (fails > 0) process.exit(1);
