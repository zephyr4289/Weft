// generate.mjs — Pillar 6 deterministic fixture generator (zero entropy).
//
// Canonical source of the frozen golden fixtures consumed by TS / Python /
// Swift / Dart managed adapters:
//   fintech-stream.bin  framed ITCH 5.0 stream (u16 BE length + payload)
//   sbe-schema.json     SBE schema descriptor (docs/adapters/MANAGED-SEAMS-V1.md §3)
//   sbe-stream.bin      u32 LE length-framed SBE records (incl. version-extension)
//   rng1-ring.bin       RNG1 ring snapshot (valid + torn + overwrite cases)
//   scenario.json       counts + checksums (parity ground truth)
//
// Determinism contract: xorshift32 PRNG (integer-only), no Date.now(), no
// Math.random(), no Object key iteration order dependence. Double-run is
// byte-identical (asserted by suite stage 2).
//
// Cold path: BigInt is used here for u64 order-ref bookkeeping. This is the
// FIXTURE GENERATOR, not steady-state managed code — the managed book uses
// lo/hi u32 pairs (Law: no floats / no BigInt on the hot path).

import { writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

// ---------------------------------------------------------------------------
// xorshift32 — deterministic, portable, integer-only
// ---------------------------------------------------------------------------
export function makeRng(seed) {
  let s = seed >>> 0;
  if (s === 0) s = 0x9e3779b9;
  return function next() {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s;
  };
}

const STOCK = 'WEFTUSD'; // 8-byte ITCH stock field, space-padded
const LOCATE = 7777;
const TRACKING = 42;
const MID = 1_000_000; // $100.0000 in ticks

// ---------------------------------------------------------------------------
// ITCH 5.0 payload writers (big-endian, docs/adapters/MANAGED-SEAMS-V1.md §2)
// ---------------------------------------------------------------------------
function prefix(len) { const b = Buffer.alloc(2); b.writeUInt16BE(len, 0); return b; }

export function itchSystemEvent(ts, code) {
  const p = Buffer.alloc(36);
  p.writeUInt8(0x53, 0); // 'S'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  p.writeUInt16BE(Math.floor(ts / 2 ** 32), 5);
  p.writeUInt32BE(ts >>> 0 === ts ? ts : ts % 2 ** 32, 7);
  p.writeUInt8(code.charCodeAt(0), 11);
  return Buffer.concat([prefix(36), p]);
}

function writeTs6(p, ts) {
  const hi = Math.floor(ts / 2 ** 32);
  const lo = ts % 2 ** 32;
  p.writeUInt16BE(hi, 5);
  p.writeUInt32BE(lo, 7);
}

export function itchAdd(ts, refHi, refLo, side, shares, price) {
  const p = Buffer.alloc(36);
  p.writeUInt8(0x41, 0); // 'A'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  writeTs6(p, ts);
  p.writeUInt32BE(refHi, 11);
  p.writeUInt32BE(refLo, 15);
  p.writeUInt8(side === 'B' ? 0x42 : 0x53, 19); // 'B' / 'S'
  p.writeUInt32BE(shares, 20);
  p.write(STOCK.padEnd(8, ' '), 24, 8, 'latin1');
  p.writeUInt32BE(price, 32);
  return Buffer.concat([prefix(36), p]);
}

export function itchExecute(ts, refHi, refLo, shares, match) {
  const p = Buffer.alloc(31);
  p.writeUInt8(0x45, 0); // 'E'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  writeTs6(p, ts);
  p.writeUInt32BE(refHi, 11);
  p.writeUInt32BE(refLo, 15);
  p.writeUInt32BE(shares, 19);
  p.writeUInt32BE(Math.floor(match / 2 ** 32), 23);
  p.writeUInt32BE(match % 2 ** 32, 27);
  return Buffer.concat([prefix(31), p]);
}

export function itchCancel(ts, refHi, refLo, cancelled) {
  const p = Buffer.alloc(23);
  p.writeUInt8(0x58, 0); // 'X'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  writeTs6(p, ts);
  p.writeUInt32BE(refHi, 11);
  p.writeUInt32BE(refLo, 15);
  p.writeUInt32BE(cancelled, 19);
  return Buffer.concat([prefix(23), p]);
}

export function itchDelete(ts, refHi, refLo) {
  const p = Buffer.alloc(19);
  p.writeUInt8(0x44, 0); // 'D'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  writeTs6(p, ts);
  p.writeUInt32BE(refHi, 11);
  p.writeUInt32BE(refLo, 15);
  return Buffer.concat([prefix(19), p]);
}

export function itchReplace(ts, origHi, origLo, newHi, newLo, shares, price) {
  const p = Buffer.alloc(35);
  p.writeUInt8(0x55, 0); // 'U'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  writeTs6(p, ts);
  p.writeUInt32BE(origHi, 11);
  p.writeUInt32BE(origLo, 15);
  p.writeUInt32BE(newHi, 19);
  p.writeUInt32BE(newLo, 23);
  p.writeUInt32BE(shares, 27);
  p.writeUInt32BE(price, 31);
  return Buffer.concat([prefix(35), p]);
}

export function itchTrade(ts, refHi, refLo, side, shares, price, match) {
  const p = Buffer.alloc(44);
  p.writeUInt8(0x50, 0); // 'P'
  p.writeUInt16BE(LOCATE, 1);
  p.writeUInt16BE(TRACKING, 3);
  writeTs6(p, ts);
  p.writeUInt32BE(refHi, 11);
  p.writeUInt32BE(refLo, 15);
  p.writeUInt8(side === 'B' ? 0x42 : 0x53, 19);
  p.writeUInt32BE(shares, 20);
  p.write(STOCK.padEnd(8, ' '), 24, 8, 'latin1');
  p.writeUInt32BE(price, 32);
  p.writeUInt32BE(Math.floor(match / 2 ** 32), 36);
  p.writeUInt32BE(match % 2 ** 32, 40);
  return Buffer.concat([prefix(44), p]);
}

// ---------------------------------------------------------------------------
// Scenario engine — deterministic market lifecycle over `count` messages
// ---------------------------------------------------------------------------
export const CHECKPOINTS = [512, 1024, 1536, 2048];

// Live order tracking for the scenario: [hi, lo, side, shares, price]
// Big refs (hi = 0x00C0FFEE) exceed 2^53 when materialized as doubles — the
// generator never does that; managed books must handle lo/hi pairs exactly.
export const BIG_REF_HI = 0x00c0ffee;

function runScenario(count, seed, emit, opts = {}) {
  // Core deterministic scenario state machine. Emits framed messages one by
  // one (emit(buffer)) so callers can stream into a fixture array, a chunk
  // pipeline, or a probe loop without holding 1M+ Buffer objects.
  //
  // `liveCap`: when the live-order set reaches this size, add-phases are
  // re-routed into executes so the book self-balances (realistic intraday
  // depth). The 2,048-message fixture uses Infinity — byte stream unchanged;
  // long-running probes/demos use 32,768.
  // `fastDelete`: O(1) swap-delete on the live set. The fixture path keeps
  // exact splice semantics (byte-reproducible); long runs default to fast.
  const liveCap = opts.liveCap ?? Infinity;
  const fast = opts.fastDelete ?? false;
  const rng = makeRng(seed);
  const counts = { S: 0, A: 0, E: 0, X: 0, D: 0, U: 0, P: 0 };
  const live = [];       // FIFO of [hi, lo, side, shares, price]
  let adds = 0;
  let match = 1;
  let emitted = 0;

  emit(itchSystemEvent(0, 'O'));
  counts.S += 1;
  emitted += 1;

  const removeAt = (slot) => {
    if (fast) {
      live[slot] = live[live.length - 1];
      live.pop();
    } else {
      live.splice(slot, 1);
    }
  };

  let i = emitted; // message index (S counted)
  while (emitted < count - 1) {
    const ts = i * 1_000_000;
    const phase = i % 10;
    if (live.length === 0 || ((phase <= 3 || phase === 9) && live.length < liveCap)) {
      // Add order: alternate sides; odd adds use big refs
      adds += 1;
      const side = adds % 2 === 0 ? 'B' : 'S';
      const base = side === 'B' ? MID - 100 : MID + 100;
      const drift = rng() % 500;
      const price = side === 'B' ? base - drift : base + drift;
      const shares = 100 + (rng() % 10) * 100;
      const hi = adds % 2 === 1 ? BIG_REF_HI : 0;
      const lo = adds % 2 === 1 ? adds : 1_000_000 + adds;
      emit(itchAdd(ts, hi, lo, side, shares, price));
      counts.A += 1;
      live.push([hi, lo, side, shares, price]);
    } else {
      const slot = rng() % live.length;
      const ord = live[slot];
      const ts2 = ts;
      if (phase === 4) {
        // Execute (full or partial)
        const shares = ord[3] > 1 ? 1 + (rng() % ord[3]) : 1;
        match += 1;
        emit(itchExecute(ts2, ord[0], ord[1], shares, match));
        counts.E += 1;
        ord[3] -= shares;
        if (ord[3] === 0) removeAt(slot);
      } else if (phase === 5) {
        // Partial cancel
        const shares = ord[3] > 1 ? 1 + (rng() % (ord[3] - 1)) : 1;
        emit(itchCancel(ts2, ord[0], ord[1], shares));
        counts.X += 1;
        ord[3] -= shares;
        if (ord[3] === 0) removeAt(slot);
      } else if (phase === 6) {
        // Full delete
        emit(itchDelete(ts2, ord[0], ord[1]));
        counts.D += 1;
        removeAt(slot);
      } else if (phase === 7) {
        // Replace: feed removes the original order implicitly (ITCH 'U')
        const drift = rng() % 400;
        const price = ord[2] === 'B' ? ord[4] - drift : ord[4] + drift;
        adds += 1;
        const hi = adds % 2 === 1 ? BIG_REF_HI : 0;
        const lo = adds % 2 === 1 ? adds : 1_000_000 + adds;
        emit(itchReplace(ts2, ord[0], ord[1], hi, lo, ord[3], price));
        counts.U += 1;
        if (fast) {
          live[slot] = [hi, lo, ord[2], ord[3], price]; // O(1) swap-in
        } else {
          live.splice(slot, 1);
          live.push([hi, lo, ord[2], ord[3], price]);
        }
      } else {
        // Trade non-cross against the resting side
        match += 1;
        emit(itchTrade(ts2, ord[0], ord[1], ord[2], ord[3], ord[4], match));
        counts.P += 1;
      }
    }
    emitted += 1;
    i += 1;
  }
  emit(itchSystemEvent(i * 1_000_000, 'C'));
  counts.S += 1;
  emitted += 1;
  return { counts, adds, total: emitted };
}

export function buildItchStream(count, { seed = 0x5eed1234 } = {}) {
  const out = [];
  const { counts, adds } = runScenario(count, seed, (b) => out.push(b));
  return { chunks: out, counts, adds };
}

// Chunked pipeline feed: `chunkMsgs` messages per chunk. Used by the 1M
// alloc probe and the 5M trading demo (bounded memory, real-feed cadence).
// liveCap keeps the live-order set bounded so long runs never exhaust the
// book pool — the depth self-balances like a real intraday market.
export function buildItchChunked(count, chunkMsgs = 50_000, { seed = 0x5eed1234, liveCap = 32_768, fastDelete = true } = {}) {
  const chunks = [];
  const acc = [];
  let accMsgs = 0;
  const { counts, adds, total } = runScenario(count, seed, (b) => {
    acc.push(b);
    accMsgs++;
    if (accMsgs === chunkMsgs) {
      chunks.push(Buffer.concat(acc));
      acc.length = 0;
      accMsgs = 0;
    }
  }, { liveCap, fastDelete });
  if (accMsgs > 0) chunks.push(Buffer.concat(acc));
  return { chunks, counts, adds, total, messagesPerChunk: chunkMsgs };
}

// ---------------------------------------------------------------------------
// SBE stream — schema 1, template 1001 BookRefresh (32B) / 1002 ext (40B)
// ---------------------------------------------------------------------------
export function buildSbeStream(records = 4096) {
  const out = [];
  for (let i = 0; i < records; i++) {
    const ext = i % 8 === 7;
    const body = 24;
    const total = 8 + body + (ext ? 8 : 0); // header(8) + body(+ extension)
    const rec = Buffer.alloc(total);
    rec.writeUInt16LE(body, 0);
    rec.writeUInt16LE(ext ? 1002 : 1001, 2);
    rec.writeUInt16LE(1, 4); // schemaId
    rec.writeUInt16LE(0, 6); // version
    rec.writeBigUInt64LE(BigInt(i + 1), 8);        // seq
    rec.writeBigUInt64LE(BigInt((i + 1) * 250_000), 16); // tsNs
    rec.writeBigInt64LE(BigInt(100_000 + (i % 7)), 24);  // bidPrice
    if (ext) rec.writeUInt32LE(0xdeadbeef, 32); // trailing extension bytes
    const pre = Buffer.alloc(4);
    pre.writeUInt32LE(total, 0);
    out.push(Buffer.concat([pre, rec]));
  }
  return Buffer.concat(out);
}

export const SBE_SCHEMA = {
  semanticVersion: '5.2',
  littleEndian: true,
  header: {
    blockLength: 8,
    fields: [
      { name: 'blockLength', type: 'u16', offset: 0 },
      { name: 'templateId', type: 'u16', offset: 2 },
      { name: 'schemaId', type: 'u16', offset: 4 },
      { name: 'version', type: 'u16', offset: 6 },
    ],
  },
  messages: {
    1001: {
      name: 'BookRefresh', blockLength: 24,
      fields: [
        { name: 'seq', type: 'u64', offset: 0 },
        { name: 'tsNs', type: 'u64', offset: 8 },
        { name: 'bidPrice', type: 'i64', offset: 16 },
      ],
    },
    1002: {
      name: 'BookRefreshExt', blockLength: 24,
      fields: [
        { name: 'seq', type: 'u64', offset: 0 },
        { name: 'tsNs', type: 'u64', offset: 8 },
        { name: 'bidPrice', type: 'i64', offset: 16 },
      ],
    },
  },
};

// ---------------------------------------------------------------------------
// RNG1 ring fixture — 8 slots x 4096B, 10 records incl. 1 torn + 2 overwrites
// ---------------------------------------------------------------------------
const RING_HEADER = 128;
const RING_SLOTS = 8;
const RING_SLOT_SIZE = 4096;

function ringSlotHeader(seq, len, topic, ts, fmt, flags) {
  const h = Buffer.alloc(64);
  h.writeBigUInt64LE(BigInt(seq), 0);
  h.writeUInt32LE(len, 8);
  h.writeUInt32LE(topic, 12);
  h.writeBigUInt64LE(BigInt(ts), 16);
  h.writeUInt32LE(fmt, 24);
  h.writeUInt32LE(flags, 28);
  return h;
}

export function buildRng1Ring() {
  const buf = Buffer.alloc(RING_HEADER + RING_SLOTS * RING_SLOT_SIZE);
  buf.write('RNG1', 0, 'latin1');
  buf.writeUInt16LE(1, 4);
  buf.writeUInt16LE(RING_HEADER, 6);
  buf.writeUInt32LE(RING_SLOT_SIZE, 8);
  buf.writeUInt32LE(RING_SLOTS, 12);
  // topic table: 1=IMU6DOF, 2=POINTS_F32, 3=FRAME_DESC, 4=BOXES_F32
  buf.writeUInt32LE(1, 64);
  buf.writeUInt32LE(2, 68);
  buf.writeUInt32LE(3, 72);
  buf.writeUInt32LE(4, 76);

  const imuPayload = (k) => {
    const p = Buffer.alloc(64);
    const f = k => { const v = Buffer.alloc(8); v.writeDoubleLE(k, 0); return v; };
    return Buffer.concat([f(1000 + k), f(1.0), f(0.01 * k), f(0.02 * k), f(0.03 * k), f(0.1 * k), f(0.2 * k), f(0.3 * k)]);
  };
  const pointsPayload = (n, off) => {
    const p = Buffer.alloc(n * 12);
    for (let i = 0; i < n; i++) {
      const j = off + i;
      p.writeFloatLE((j * 3) % 101, i * 12);
      p.writeFloatLE((j * 7) % 103, i * 12 + 4);
      p.writeFloatLE((j * 11) % 107, i * 12 + 8);
    }
    return p;
  };
  const frameDesc = () => {
    const p = Buffer.alloc(32);
    p.write('FRM1', 0, 'latin1');
    p.writeUInt32LE(3840, 4);
    p.writeUInt32LE(2160, 8);
    p.writeUInt32LE(11520, 12);
    p.writeUInt32LE(1, 16); // RGB8
    p.writeUInt32LE(42, 20); // handle_ns
    p.writeUInt32LE(7, 24);  // handle_lo
    p.writeUInt32LE(0, 28);
    return p;
  };
  const boxesPayload = (m) => {
    const p = Buffer.alloc(m * 24);
    for (let j = 0; j < m; j++) {
      p.writeFloatLE(j * 10, j * 24);
      p.writeFloatLE(j * 20, j * 24 + 4);
      p.writeFloatLE(j * 5, j * 24 + 8);
      p.writeFloatLE(4, j * 24 + 12);
      p.writeFloatLE(2, j * 24 + 16);
      p.writeFloatLE(0.5 + j * 0.1, j * 24 + 20);
    }
    return p;
  };

  const put = (recordSeq, topic, ts, fmt, payload) => {
    const slotIdx = (recordSeq - 1) % RING_SLOTS;
    const at = RING_HEADER + slotIdx * RING_SLOT_SIZE;
    buf.writeBigUInt64LE(BigInt(recordSeq), 16); // write_seq (in flight value)
    const head = ringSlotHeader(recordSeq, payload.length, topic, ts, fmt, 0);
    head.copy(buf, at);
    payload.copy(buf, at + 64);
  };

  // records 1..10; record 4 is TORN (crash mid-write: seq odd, len poisoned)
  put(1, 1, 1000, 1, imuPayload(1));
  put(2, 2, 2000, 2, pointsPayload(100, 0));
  put(3, 1, 3000, 1, imuPayload(2));
  {
    // torn: seq written as odd (4|1), len = 0xDEADBEEF, payload garbage
    const slotIdx = 3;
    const at = RING_HEADER + slotIdx * RING_SLOT_SIZE;
    buf.writeBigUInt64LE(5n, 16); // write_seq left odd (in-flight)
    const head = Buffer.alloc(64);
    head.writeBigUInt64LE(5n, 0);
    head.writeUInt32LE(0xdeadbeef, 8);
    head.writeUInt32LE(2, 12);
    head.writeBigUInt64LE(4000n, 16);
    head.writeUInt32LE(2, 24);
    head.copy(buf, at);
    buf.fill(0xab, at + 64, at + 640);
  }
  put(5, 2, 5000, 2, pointsPayload(100, 100));
  put(6, 1, 6000, 1, imuPayload(3));
  put(7, 2, 7000, 2, pointsPayload(100, 200));
  put(8, 3, 8000, 3, frameDesc());
  put(9, 4, 9000, 4, boxesPayload(3));
  put(10, 1, 10000, 1, imuPayload(4));

  // final committed header state
  buf.writeBigUInt64LE(10n, 16);        // write_seq (even = idle)
  buf.writeBigUInt64LE(10n, 24);        // committed_seq
  buf.writeBigUInt64LE(0n, 32);         // drop_count
  buf.writeBigUInt64LE(2n, 40);         // overwrite_count (seqs 1,2 overwritten)
  return buf;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
export function sha256(buf) { return createHash('sha256').update(buf).digest('hex'); }

export function buildAll() {
  const itch = buildItchStream(2048);
  const stream = Buffer.concat(itch.chunks);
  const sbe = buildSbeStream(4096);
  const ring = buildRng1Ring();
  const scenario = {
    stream_bytes: stream.length,
    message_count: itch.chunks.length,
    counts: itch.counts,
    adds: itch.adds,
    big_ref_hi: BIG_REF_HI,
    checkpoints: CHECKPOINTS,
    locate: LOCATE,
    tracking: TRACKING,
    stock: STOCK,
    mid_ticks: MID,
    seed: 0x5eed1234,
    sha256: {
      'fintech-stream.bin': sha256(stream),
      'sbe-stream.bin': sha256(sbe),
      'sbe-schema.json': sha256(Buffer.from(JSON.stringify(SBE_SCHEMA))),
      'rng1-ring.bin': sha256(ring),
    },
  };
  return { stream, sbe, ring, scenario };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const outDir = process.argv[2] && process.argv[2] !== '--out'
    ? process.argv[2]
    : (process.argv.indexOf('--out') >= 0 ? process.argv[process.argv.indexOf('--out') + 1] : '.');
  mkdirSync(outDir, { recursive: true });
  const { stream, sbe, ring, scenario } = buildAll();
  writeFileSync(join(outDir, 'fintech-stream.bin'), stream);
  writeFileSync(join(outDir, 'sbe-stream.bin'), sbe);
  writeFileSync(join(outDir, 'sbe-schema.json'), JSON.stringify(SBE_SCHEMA, null, 2) + '\n');
  writeFileSync(join(outDir, 'rng1-ring.bin'), ring);
  writeFileSync(join(outDir, 'scenario.json'), JSON.stringify(scenario, null, 2) + '\n');
  console.log(`fintech-stream.bin ${stream.length}B (${itchMsgs(scenario)} msgs), sbe-stream.bin ${sbe.length}B, rng1-ring.bin ${ring.length}B`);
}
function itchMsgs(s) { return s.message_count; }
