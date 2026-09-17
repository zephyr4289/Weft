// gen_trace_refs.mjs — canonical RFC-0009 trace reference generator (Series 7).
// Emits the packed byte streams + FNV-1a 64 hashes for the G5 ladder trace
// and the PC3 cadence trace, so every port's battery can pin cross-language
// parity locally (the xlang fixtures do the full byte-compare in CI).
import { FreshnessGovernor, CadencePolicy, CadencePolicyKind } from '../packages/core/dist/index.js';

function xorshift32(x) { x >>>= 0; x ^= (x << 13) >>> 0; x ^= x >>> 17; x ^= (x << 5) >>> 0; return x >>> 0; }
function fnv1a64(bytes) {
  let h = 0xcbf29ce484222325n;
  for (const b of bytes) { h ^= BigInt(b); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
  return h;
}
const STEPS = 10000, SEED = 0x00c0ffee;

// Ladder trace (G5 shape — fixtures/xlang-governor)
{
  const gov = new FreshnessGovernor();
  const bytes = [];
  const classes = new Set();
  let state = SEED;
  for (let i = 0; i < STEPS; i++) {
    state = xorshift32(state);
    const behind = state % 128;
    const a = gov.step(behind, i);
    bytes.push((a.kind << 6) | Math.min(a.skipN, 63));
    classes.add(a.kind);
  }
  console.log('LADDER  fnv1a64 = 0x' + fnv1a64(bytes).toString(16).padStart(16, '0'), ' bytes =', bytes.length, ' classes =', [...classes].sort().join(','));
}
// Cadence trace (PC3 shape — fixtures/xlang-cadence)
{
  const pols = [0, 1, 2].map((k) => new CadencePolicy({ policy: k }));
  const bytes = [];
  let state = SEED, latest = 0;
  const presented = [false, false, false];
  for (let i = 0; i < STEPS; i++) {
    state = xorshift32(state);
    latest += state % 5;
    for (let pi = 0; pi < 3; pi++) {
      const a = pols[pi].step(latest);
      if (a.present) presented[pi] = true;
      bytes.push(((a.present ? 1 : 0) << 7) | ((a.interp ? 1 : 0) << 6) | (a.alphaQ12 >> 7));
      bytes.push(Math.min(a.coalesced, 255));
    }
  }
  console.log('CADENCE fnv1a64 = 0x' + fnv1a64(bytes).toString(16).padStart(16, '0'), ' bytes =', bytes.length, ' presentedAll =', presented.every(Boolean));
}
