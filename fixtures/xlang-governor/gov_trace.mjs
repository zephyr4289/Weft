// gov_trace.mjs — G5 trace emitter (RFC-0009), TS side.
//
// Emits the packed action log for the deterministic (behind, now_ms) trace
// shared with core/c/governor_test.c `xlang-dump` and
// core/rust/target/release/governor_xlang:
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N: state = xorshift32(state);
//                  behind = state % 128; now_ms = i;
//                  action = governor.step(behind, now_ms)
//                  emit byte (kind << 6) | min(skip_n, 63)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — the exact
// format the C and Rust emitters produce. run.sh byte-compares all three.
//
// Usage: node gov_trace.mjs [STEPS [SEED]]   (SEED in decimal or 0xHEX)

import { FreshnessGovernor } from '../../packages/core/dist/index.js';

function xorshift32(x) {
  x >>>= 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

const args = process.argv.slice(2);
const steps = args[0] ? Number(args[0]) : 10000;
const seedRaw = args[1] ?? '0x00C0FFEE';
const seed = seedRaw.startsWith('0x') ? parseInt(seedRaw, 16) : Number(seedRaw);

const gov = new FreshnessGovernor();
let state = seed >>> 0;
let out = '';
for (let i = 0; i < steps; i++) {
  state = xorshift32(state);
  const behind = state % 128;
  const a = gov.step(behind, i);
  const packed = (a.kind << 6) | Math.min(a.skipN, 63);
  out += packed.toString(16).padStart(2, '0');
}
out += '\n';
process.stdout.write(out);
