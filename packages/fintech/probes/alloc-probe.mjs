// alloc-probe.mjs — Law 2 heap evidence for @weft/fintech (--expose-gc).
//
//   node --expose-gc probes/alloc-probe.mjs ingest    # 1,000,000 ITCH msgs
//   node --expose-gc probes/alloc-probe.mjs control   # negative control (MUST bite)
//
// ingest: parses + applies 1,000,000 framed ITCH 5.0 messages through the
//   engine/book in bounded chunks (real-feed cadence), forcing GC before and
//   after. Steady-state heap growth MUST stay <= 64 KiB (gate). The feed
//   generator itself allocates (it is the producer), but nothing it hands to
//   the engine is retained by the engine — that is the property under proof.
//
// control: same loop, but retains one {ref,price,size} object per 100
//   messages (10,000 retained objects). This is the biting negative control:
//   it MUST exceed the gate and exit 2, proving the probe can fail.
//
// Output: single JSON line {"mode","ok","growthKiB","gateKiB",...}
// Exit codes: 0 ok | 2 gate exceeded (control mode) | 1 usage error.

import { buildItchChunked } from '../../../tests/adapters/managed/fixtures/generate.mjs';
import { OrderBook } from '../src/book.js';
import { ItchEngine } from '../src/itch.js';

const GATE_KIB = 64;
const MESSAGES = 1_000_000;
const CHUNK = 50_000;

function kib(n) { return Math.round((n / 1024) * 100) / 100; }

function runIngest() {
  const feed = buildItchChunked(MESSAGES, CHUNK);
  // warmup: a FULL first pass over all chunks — V8 tiers up DURING a
  // 1M-message loop, and code-space growth from tiering is not managed
  // allocation. Measuring the SECOND (fully tiered) pass is the honest
  // steady-state probe.
  {
    const b = new OrderBook({ poolCapacity: 65536 });
    const e = new ItchEngine(b);
    for (const chunk of feed.chunks) e.process(chunk);
  }
  global.gc();
  const before = process.memoryUsage().heapUsed;

  const book = new OrderBook({ poolCapacity: 65536 });
  const engine = new ItchEngine(book);
  let peakDuring = 0;
  for (const chunk of feed.chunks) {
    engine.process(chunk);
    const h = process.memoryUsage().heapUsed;
    if (h > peakDuring) peakDuring = h;
  }

  // 3 gc+sample rounds, take the MIN: the fairest "residue after a full
  // collection" measure (V8 code-space jitter inflates single samples).
  let after = Infinity;
  for (let i = 0; i < 3; i++) {
    global.gc();
    const h = process.memoryUsage().heapUsed;
    if (h < after) after = h;
  }
  const growth = after - before;

  const ok = growth <= GATE_KIB * 1024
    && book.msgsApplied === feed.total
    && book.totalRejects() === 0
    && book.liveOrders > 0;

  return {
    mode: 'ingest',
    ok,
    growthKiB: kib(growth),
    gateKiB: GATE_KIB,
    messages: book.msgsApplied,
    liveOrders: book.liveOrders,
    rejects: book.totalRejects(),
    skipped: book.skipped,
  };
}

// retained sink OUTSIDE any closure the optimizer can kill (Pillar 5 lesson)
const SINK = [];

function runControl() {
  const feed = buildItchChunked(MESSAGES, CHUNK);
  const book = new OrderBook({ poolCapacity: 65536 });
  const engine = new ItchEngine(book);
  global.gc();
  const before = process.memoryUsage().heapUsed;

  let n = 0;
  for (const chunk of feed.chunks) {
    engine.process(chunk);
    // retain one object per 100 messages — the textbook GC leak
    for (let k = 0; k < 500; k++) {
      SINK.push({ ref: (n += 100), price: 999000, size: 100 });
    }
  }

  global.gc();
  const after = process.memoryUsage().heapUsed;
  const growth = after - before;
  const ok = growth > GATE_KIB * 1024 && SINK.length > 0;

  return {
    mode: 'control',
    ok,
    growthKiB: kib(growth),
    gateKiB: GATE_KIB,
    retained: SINK.length,
    messages: book.msgsApplied,
  };
}

const mode = process.argv[2] ?? 'ingest';
if (mode !== 'ingest' && mode !== 'control') {
  console.error('usage: node --expose-gc probes/alloc-probe.mjs [ingest|control]');
  process.exit(1);
}
const result = mode === 'ingest' ? runIngest() : runControl();
console.log(JSON.stringify(result));
process.exit(result.ok ? 0 : 2);
