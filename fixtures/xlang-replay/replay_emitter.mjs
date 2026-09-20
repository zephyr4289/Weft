// replay_emitter.mjs — RFC 0019 cross-language replay hash-log emitter (TS).
//
// Folds the deterministic RFC-0019 fixture scenario and prints the
// per-step u64 state hashes as one lowercase-hex line — byte-identical to
// core/c/replay_runner.c (the C reference), the Rust replay_xlang bin, and
// the Kotlin/Swift/Dart VM emitters. fixtures/xlang-replay/run.sh
// byte-compares them all.
//
// Usage: node replay_emitter.mjs [STEPS] [SEED]

const mod = await import(process.env.WEFT_CORE_PATH ||
  '../../packages/core/dist/index.js');
const { replayNew, replayStep } = mod;

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

// scenario mirror (RFC-0019 fixture grammar — normative table in run.sh)
const scen = {
  latest: 0, w_work: 1, r_work: 2, epoch: 0, revoked: 0, seq: 0,
  bufseq: [0, 0, 0],
};

function scenNext(state) {
  state.v = xorshift32(state.v);
  const u = state.v >>> 0;
  const op = u & 15;
  if (op < 7) {
    scen.seq = (scen.seq + 1) >>> 0;
    const len = Math.floor(u / 16) % 1024;
    if (!scen.revoked) {
      scen.bufseq[scen.w_work] = scen.seq;
      const old = scen.latest;
      scen.latest = scen.w_work; scen.w_work = old;
      return { kind: 1, aux: len, data: scen.seq };
    }
    scen.epoch = (scen.epoch + 1) >>> 0;
    return { kind: 3, aux: scen.epoch & 0xffff, data: scen.seq };
  }
  if (op < 12) {
    const data = scen.bufseq[scen.latest];
    const mine = scen.latest;
    scen.latest = scen.r_work; scen.r_work = mine;
    return { kind: 2, aux: 0, data };
  }
  if (op === 12) {
    if (!scen.revoked) { scen.revoked = 1; return { kind: 4, aux: 0, data: scen.epoch }; }
    return { kind: 5, aux: 0, data: scen.epoch };
  }
  if (op === 13) {
    if (scen.revoked) { scen.revoked = 0; return { kind: 5, aux: 0, data: scen.epoch }; }
    return { kind: 6, aux: 0, data: Math.floor(u / 16) % 8 };
  }
  if (op === 14) return { kind: 7, aux: 0, data: scen.seq };
  return { kind: 8, aux: 0, data: scen.seq };
}

const s = replayNew();
const state = { v: seed };  // SHARED — the xorshift advances across steps
let out = '';
for (let i = 0; i < steps; i++) {
  const e = scenNext(state);
  const rc = replayStep(s, e);
  if (rc !== 0) {
    console.error(`replay_emitter: fold disagreement at step ${i}`);
    process.exit(1);
  }
  out += s.hash.toString(16).padStart(16, '0');
}
console.log(out);
