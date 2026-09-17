// blend_verify.mjs — TS verifier for the xlang-blend golden vectors.
//
// Replays the C kernel's deterministic buffer construction (the exact
// xorshift32 pair-fill blend_test.c uses, seed 0x5EEDBEEF) for every
// (size, alpha) row of golden-vectors.csv, blends with @weft/core's
// blendQ12Words, and FNV-1a-64 digests the output — the digest MUST equal
// the C kernel's. Any divergence in channel order, shift depth, rounding,
// or endianness shows up here as a hard byte mismatch.
//
// Usage: node blend_verify.mjs [CSV]   (default golden-vectors.csv)
// Requires: ../../packages/core/dist built (tsup).

import { blendQ12Words, fnv1a64Words } from '../../packages/core/dist/index.js';
import { readFileSync } from 'node:fs';

function xorshift32(x) {
  x >>>= 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

function fillRandom(words, stateRef) {
  // Mirrors blend_test.c fill_random EXACTLY: two xorshift32 draws per
  // word (prev then newest), over words + 8 guard words, mutating state.
  const prev = new Uint32Array(words + 8);
  const newest = new Uint32Array(words + 8);
  for (let i = 0; i < words + 8; i++) {
    stateRef.v = xorshift32(stateRef.v);
    prev[i] = stateRef.v;
    stateRef.v = xorshift32(stateRef.v);
    newest[i] = stateRef.v;
  }
  return [prev, newest];
}

const csv = process.argv[2] ?? 'golden-vectors.csv';
const lines = readFileSync(new URL(`./${csv}`, import.meta.url), 'utf8')
  .split('\n')
  .filter((l) => /^[0-9]+,/.test(l));

let pass = 0;
let fail = 0;
for (const line of lines) {
  const [sizeStr, alphaStr, want] = line.trim().split(',');
  const words = Number(sizeStr);
  const alpha = Number(alphaStr);
  const stateRef = { v: 0x5eedbeef };
  const [prev, newest] = fillRandom(words, stateRef);
  const out = new Uint32Array(words + 8); // the guard words stay zero —
  blendQ12Words(prev, newest, alpha, out); // the digest covers `words` only
  const got = fnv1a64Words(out.subarray(0, words));
  if (got === want) {
    pass++;
  } else {
    fail++;
    console.error(`MISMATCH size=${words} alpha=${alpha}: want ${want} got ${got}`);
  }
}
console.log(`xlang-blend TS verifier: ${pass}/${pass + fail} golden digests match`);
if (fail > 0) process.exit(1);
