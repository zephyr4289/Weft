// trend_emitter.mjs — RFC 0020 cross-language verdict-stream emitter (TS).
//
// Drives the trend estimator with the deterministic behind trace
// (behind = xorshift32(state) % 64, seed 0x00C0FFEE) and prints the packed
// verdict stream (verdict << 6 | min(skip_n, 63), hex, one line) —
// byte-identical to the C/C++/Rust/VM emitters. fixtures/xlang-trend/
// byte-compares them all.
//
// Usage: node trend_emitter.mjs [STEPS] [SEED]

const mod = await import(process.env.WEFT_CORE_PATH ||
  '../../packages/core/dist/index.js');
const { trendInit, trendObserve, trendPack } = mod;

function xorshift32(x) {
  x = (x ^ ((x << 13) | 0)) | 0;
  x = (x ^ (x >>> 17)) | 0;
  x = (x ^ ((x << 5) | 0)) | 0;
  return x | 0;
}

const steps = Number(process.argv[2] ?? 10000);
let seed = process.argv[3] ?? '0x00C0FFEE';
seed = seed.startsWith('0x')
  ? parseInt(seed.slice(2), 16) | 0
  : parseInt(seed, 10) | 0;

const t = trendInit();
const o = { verdict: 0, pred_raw: 0, skip_n: 0 };
let out = '';
for (let i = 0; i < steps; i++) {
  seed = xorshift32(seed);
  const behind = (seed >>> 0) % 64;
  trendObserve(t, behind, o);
  out += trendPack(o).toString(16).padStart(2, '0');
}
console.log(out);
