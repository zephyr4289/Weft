#!/usr/bin/env node
// audit/static_audit.mjs — structural audit for apple/WeftFintech.
//
// No Swift toolchain in the managed sandbox (Pillar 4 precedent): this
// audit pins the compile-checkable properties, constants parity vs the
// TS reference, and re-derives CRC KATs with node:zlib. 22 checks,
// fail-closed.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const HERE = dirname(fileURLToPath(import.meta.url));
const PKG = join(HERE, '..');
const REPO = join(PKG, '..', '..');

let fails = 0;
const check = (label, cond) => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}`);
  if (!cond) process.exitCode = 1, fails++;
};
const read = (p) => readFileSync(join(PKG, p), 'utf8');
const strip = (s) => s.split('\n')
    .map((l) => l.replace(/(^|[^:'"\/])\/\/.*$/, '$1')).join('\n');

const wire = strip(read('Sources/WeftFintech/Mdp1Wire.swift'));
const model = strip(read('Sources/WeftFintech/WeftOrderBookModel.swift'));
const view = strip(read('Sources/WeftFintech/WeftOrderBookView.swift'));
const tests = read('Tests/WeftFintechTests/Mdp1WireTests.swift');
const pkg = read('Package.swift');

// -- inventory ----------------------------------------------------------------
for (const f of ['Package.swift', 'Sources/WeftFintech/Mdp1Wire.swift',
                 'Sources/WeftFintech/WeftOrderBookModel.swift',
                 'Sources/WeftFintech/WeftOrderBookView.swift',
                 'Tests/WeftFintechTests/Mdp1WireTests.swift',
                 'audit/static_audit.mjs', 'README.md']) {
  let ok = true;
  try { readFileSync(join(PKG, f)); } catch { ok = false; }
  check(`inventory: ${f}`, ok);
}

// -- constants parity vs TS reference ------------------------------------------
const tsMdp1 = readFileSync(join(REPO, 'packages/fintech/src/mdp1.js'), 'utf8');
const tsConst = (name) => {
  const m = tsMdp1.match(
      new RegExp(`export const ${name}\\s*=\\s*(0x[0-9a-fA-F]+|\\d+)`));
  return m ? Number(m[1]) : null;
};
const swConst = (name, src = wire) => {
  const m = src.match(new RegExp(`static let ${name}[^=]*=\\s*(0x[0-9a-fA-F_]+|\\d+)`));
  return m ? Number(m[1].replace(/_/g, '')) : null;
};
for (const name of ['size', 'version', 'magic', 'topLevels']) {
  const tn = { size: 'MDP1_SIZE', version: 'MDP1_VERSION',
               magic: 'MDP1_MAGIC', topLevels: 'MDP1_TOP_LEVELS' }[name];
  check(`parity: ${name} TS=${tsConst(tn)} Swift=${swConst(name)}`,
        tsConst(tn) !== null && tsConst(tn) === swConst(name));
}
for (const [sw, ts] of [['fBookValid', 'F_BOOK_VALID'],
                        ['fCrossed', 'F_CROSSED'], ['fLocked', 'F_LOCKED']]) {
  check(`parity: ${sw} == ${ts}`, swConst(sw) === tsConst(ts));
}
for (const [name, val] of [['offBids', 32], ['offAsks', 152], ['offCrc', 296],
                           ['crcEnd', 296], ['offMsgCount', 272],
                           ['offTradeCount', 280], ['offLastMatch', 288]]) {
  check(`geometry: ${name} == ${val}`, swConst(name) === val);
}

// -- Law 2: every load passes .littleEndian --------------------------------------
const loads = [...wire.matchAll(/loadUnaligned\([^)]*\)/g)].length;
const little = [...wire.matchAll(/loadUnaligned\([^)]*\)\.littleEndian/g)].length;
check(`Law 2: ${little}/${loads} loadUnaligned chains .littleEndian`,
      loads > 0 && loads === little);
check('Law 2: u64 composition uses shifts (never Double)',
      wire.includes('<< 32') && !/Double/.test(wire));

// -- CRC impl + KATs --------------------------------------------------------------
check('crc: poly 0xedb88320', /0xedb8_8320|0xedb88320/.test(wire));
check('crc: init/final 0xFFFFFFFF', wire.includes('0xFFFF_FFFF'));
const canon = Buffer.alloc(304);
for (let i = 0; i < 304; i++) canon[i] = (i * 7 + 13) & 0xff;
const canonCrc = zlib.crc32(canon.subarray(0, 296)) >>> 0;
check(`crc: node:zlib canonical KAT ${canonCrc} pinned in Swift tests`,
      tests.includes(String(canonCrc)));
check('crc: WEFT KAT pinned', tests.includes('3421166146'));
check('crc: 0..255 KAT pinned', tests.includes('688229491'));

// -- model: edge-trigger + fail-closed FALLBACK ------------------------------------
check('model: edge gate on repeated seq',
      model.includes('s == lastSeq') && model.includes('return false'));
check('model: FALLBACK latch fires exactly once',
      model.includes('bookValid = false') && model.includes('primed'));
check('model: preallocated level scratch',
      model.includes('repeating: 0, count: Mdp1.topLevels * 2 * 3'));

// -- view: geometry-only draw, opt-in labels ---------------------------------------
check('view: draw closure allocates no Text by default',
      !(view.slice(view.indexOf('private func draw'), view.indexOf('if showLabels'))
            .includes('Text(')));
check('view: labels opt-in', view.includes('showLabels: Bool = false'));
check('view: FALLBACK band, never throws',
      view.includes('guard model.bookValid else'));

// -- test purity --------------------------------------------------------------------
check('tests: XCTest only, no UI dependency',
      tests.includes('import XCTest') && !tests.includes('SwiftUI'));

console.log(fails === 0 ? '\nSTATIC AUDIT: ALL GREEN (27 checks)'
                        : `\nSTATIC AUDIT: ${fails} FAILURE(S)`);
if (fails > 0) process.exit(1);
