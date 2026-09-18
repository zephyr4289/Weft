// cadence_trace.mjs — PC3 cadence trace emitter (RFC-0009 §cadence), TS side.
//
// Emits the packed decision log for the deterministic arrival trace shared
// with the Kotlin/Swift/Dart emitters (fixtures/xlang-cadence/{kotlin,swift,
// dart}/):
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N:
//     state = xorshift32(state); arrivals = state % 5; latest += arrivals
//     for policy in [LATEST_WINS, PACED_INTERPOLATE, BURST_COALESCE,
//                    PREDICTIVE_PACED]:
//       d = policy.step(latest)
//       emit byte1 = (present<<7) | (interp<<6) | (alphaQ12 >> 7)
//       emit byte2 = min(coalesced, 255)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — 8 bytes
// per tick, four policies in kind order (PC3 v2, RFC-0012 added kind 3).
// run.sh byte-compares all ports.
//
// Usage: node cadence_trace.mjs [STEPS [SEED]]   (SEED in decimal or 0xHEX)
// Requires: ../../packages/core/dist built (pnpm --filter @weft/core build)

import { CadencePolicy, CadencePolicyKind } from '../../packages/core/dist/index.js';

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

const pols = [
  new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS }),
  new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE }),
  new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE }),
  new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED }),
];
let state = seed >>> 0;
let latest = 0;
let out = '';
for (let i = 0; i < steps; i++) {
  state = xorshift32(state);
  latest += state % 5;
  for (const p of pols) {
    const a = p.step(latest);
    const b1 =
      ((a.present ? 1 : 0) << 7) |
      ((a.interp ? 1 : 0) << 6) |
      (a.alphaQ12 >> 7);
    const b2 = Math.min(a.coalesced, 255);
    out += b1.toString(16).padStart(2, '0');
    out += b2.toString(16).padStart(2, '0');
  }
}
out += '\n';
process.stdout.write(out);
