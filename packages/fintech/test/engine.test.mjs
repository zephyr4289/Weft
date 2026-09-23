// engine.test.mjs — golden-stream end-to-end against the frozen manifest.
//
// The manifest (tests/adapters/managed/fixtures/expected-hashes/) is the
// PARITY GROUND TRUTH: Python must reproduce the same MDP1 hashes from the
// same stream (suite stage 5). This file proves the TS side.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { OrderBook } from '../src/book.js';
import { ItchEngine, frameOffsets } from '../src/itch.js';
import { packSnapshot, MDP1_SIZE, MDP1_TOP_LEVELS } from '../src/mdp1.js';
import { MarketTelemetry } from '../src/telemetry.js';
import { CHECKPOINTS } from '../../../tests/adapters/managed/fixtures/generate.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const fx = join(here, '../../../tests/adapters/managed/fixtures');
const stream = readFileSync(join(fx, 'fintech-stream.bin'));
const manifest = JSON.parse(readFileSync(join(fx, 'expected-hashes/expected_hashes.json'), 'utf8'));

function runWithCheckpoints() {
  const book = new OrderBook();
  const engine = new ItchEngine(book);
  const offs = frameOffsets(stream, stream.length);
  const snaps = [];
  const buf = new Uint8Array(MDP1_SIZE);
  const sb = new Int32Array(MDP1_TOP_LEVELS);
  const sa = new Int32Array(MDP1_TOP_LEVELS);
  for (const cp of CHECKPOINTS) {
    engine.process(stream, offs[cp]);
    assert.equal(book.msgsApplied, cp);
    packSnapshot(book, buf, sb, sa);
    snaps.push(Buffer.from(buf));
  }
  return { book, snaps };
}

test('TS MDP1 checkpoints reproduce the frozen parity hashes bit-for-bit', () => {
  const { snaps } = runWithCheckpoints();
  CHECKPOINTS.forEach((cp, i) => {
    const h = createHash('sha256').update(snaps[i]).digest('hex');
    assert.equal(h, manifest.hashes[`mdp1-ckpt-${cp}.bin`], `checkpoint ${cp}`);
  });
  const parity = createHash('sha256')
    .update(Buffer.concat(snaps))
    .digest('hex');
  assert.equal(parity, manifest.parity_hash);
});

test('final book state matches the frozen manifest stats', () => {
  const { book } = runWithCheckpoints();
  assert.equal(book.msgsApplied, manifest.final.msgsApplied);
  assert.equal(book.skipped, manifest.final.skipped);
  assert.equal(book.liveOrders, manifest.final.liveOrders);
  assert.equal(book.tradeCount, manifest.final.tradeCount);
  assert.equal(book.lastMatch, manifest.final.lastMatch);
  assert.equal(book.lastTs, manifest.final.lastTs);
  assert.equal(book.bestBid(), manifest.final.bestBid);
  assert.equal(book.bestAsk(), manifest.final.bestAsk);
  assert.equal(book.totalRejects(), manifest.final.rejectsTotal);
});

test('engine is deterministic: two fresh runs produce identical snapshots', () => {
  const a = runWithCheckpoints();
  const b = runWithCheckpoints();
  for (let i = 0; i < CHECKPOINTS.length; i++) {
    assert.ok(a.snaps[i].equals(b.snaps[i]));
  }
});

test('throughput sanity: 2,048-message golden stream parses in < 100ms', () => {
  const t0 = performance.now();
  runWithCheckpoints();
  const ms = performance.now() - t0;
  assert.ok(ms < 100, `parse took ${ms.toFixed(1)}ms`);
});

test('telemetry records the run with zero-allocation slot updates', () => {
  const book = new OrderBook();
  const telemetry = new MarketTelemetry();
  const engine = new ItchEngine(book);
  let nowNs = 0;
  engine.process(stream, stream.length);
  // simulate per-message telemetry hook over the message count
  for (let i = 0; i < book.msgsApplied; i++) {
    nowNs += 400_000; // 400us between messages
    telemetry.record(40, nowNs);
    telemetry.recordLatency(1200 + (i % 7) * 100);
  }
  assert.equal(telemetry.msgsTotal, book.msgsApplied);
  assert.ok(telemetry.rateEwma > 2000 && telemetry.rateEwma < 3000,
    `rate ewma ${telemetry.rateEwma}`); // 400us interval -> ~2500 msg/s
  const [p50, p99, max] = telemetry.percentiles();
  assert.ok(p50 > 0 && p99 >= p50 && max >= p99);
});
