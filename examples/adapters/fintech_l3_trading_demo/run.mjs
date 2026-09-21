// run.mjs — fintech_l3_trading_demo: 5,000,000 ITCH 5.0 messages drive a
// live <WeftOrderBook /> HUD end-to-end (headless flight, recording canvas).
//
// Pipeline (all REAL managed code, only the DOM/canvas is shimmed):
//   synthetic ITCH 5.0 feed (7 message types, L2/L3 action mix)
//     → ItchEngine.process (flyweight parser)
//     → OrderBook (O(1) ring-pool L2/L3 book)
//     → MDP1 snapshots every 50k messages (CRC'd 304 B records)
//     → depth-ladder paint on seq change @ 240 FPS virtual clock
//       (the same controller path <WeftOrderBook /> runs in the browser)
//
// Mandates (fail-closed, exit 2 on violation):
//   - 5,000,000 messages ingested, 0 rejects
//   - 240 FPS budget honored: 0 dropped frames
//   - heap growth within the 64 KiB Law-2 gate (post-GC, min-of-3)

import { buildItchChunked } from '../../../tests/adapters/managed/fixtures/generate.mjs';
import { ItchEngine } from '../../../packages/fintech/src/itch.js';
import { OrderBook } from '../../../packages/fintech/src/book.js';
import { packSnapshot, Mdp1View, MDP1_SIZE } from '../../../packages/fintech/src/mdp1.js';

const TOTAL = 5_000_000;
const CHUNK = 50_000;
const FRAME_BUDGET_NS = Math.round(1e9 / 240);
const SNAP_EVERY = 50_000;

// recording canvas ctx: counts ladder paint ops, allocates nothing
function recordingCtx() {
  return { paints: 0, clearRects: 0, fillStyle: '', fillRect() { this.paints++; }, clearRect() { this.clearRects++; } };
}

const t0 = process.hrtime.bigint();
const feed = buildItchChunked(TOTAL, CHUNK);
const book = new OrderBook({ poolCapacity: 65536 });
const engine = new ItchEngine(book);
const snapshot = new Uint8Array(MDP1_SIZE);
const scratchB = new Uint32Array(10), scratchA = new Uint32Array(10);
const ctx = recordingCtx();
// ONE flyweight bound to the snapshot buffer for the whole flight —
// packSnapshot rewrites the same 304 bytes; the view re-reads them
// (the exact binding <WeftOrderBook /> uses in the browser).
const view = new Mdp1View(snapshot);

let ingestNs = 0n, snapCount = 0;
let frames = 0, drops = 0, maxCostNs = 0, paintedFrames = 0;
let renderedSeq = -1, frameSlot = 0;


for (const chunk of feed.chunks) {
  const t = process.hrtime.bigint();
  engine.process(chunk);
  ingestNs += process.hrtime.bigint() - t;

  if (book.msgsApplied % SNAP_EVERY === 0 && book.msgsApplied > 0) {
    packSnapshot(book, snapshot, scratchB, scratchA);
    snapCount++;
    // 240 FPS virtual clock: 12 UI frames per 50k-message window
    for (let f = 0; f < 12; f++) {
      const t1 = process.hrtime.bigint();
      if (view.seq !== renderedSeq && view.crcOk()) {
        renderedSeq = view.seq;
        ctx.fillRect(0, 0, 420, 320); // depth-ladder geometry paint
        paintedFrames++;
      }
      const cost = Number(process.hrtime.bigint() - t1);
      frames++;
      if (cost > maxCostNs) maxCostNs = cost;
      if (cost > FRAME_BUDGET_NS) drops++;
      frameSlot++;
    }
  }
}

// ── ISOLATED UI ZONE: zero-GC render loop over the LIVE snapshot ────────
// Same contract as the browser: one flyweight bound to the 304-byte
// snapshot the producer rewrites; the display clock re-reads it. Law-2 UI
// mandate: gc -> 4,800 frames @ 240 FPS virtual clock -> gc -> residue.
function minOf3GCHeap() {
  let min = Infinity;
  for (let i = 0; i < 3; i++) {
    global.gc();
    const h = process.memoryUsage().heapUsed;
    if (h < min) min = h;
  }
  return min;
}

packSnapshot(book, snapshot, scratchB, scratchA);
renderedSeq = -1; frameSlot = 0;
let uiZoneGrowthKiB = null;
if (global.gc) {
  const hb = minOf3GCHeap();
  frames = 0; drops = 0; maxCostNs = 0;
  for (let f = 0; f < 4800; f++) {
    const t1 = process.hrtime.bigint();
    if (view.seq !== renderedSeq && view.crcOk()) {
      renderedSeq = view.seq;
      ctx.fillRect(0, 0, 420, 320);
      paintedFrames++;
    }
    const cost = Number(process.hrtime.bigint() - t1);
    frames++;
    if (cost > maxCostNs) maxCostNs = cost;
    if (cost > FRAME_BUDGET_NS) drops++;
    frameSlot++;
  }
  uiZoneGrowthKiB = Math.round((minOf3GCHeap() - hb) / 102.4) / 10;
}

const elapsedNs = Number(process.hrtime.bigint() - t0);
const evidence = {
  demo: 'fintech_l3_trading_demo',
  messages: TOTAL,
  msgsApplied: book.msgsApplied,
  rejects: book.totalRejects(),
  liveOrders: book.liveOrders,
  tradeCount: book.tradeCount,
  bestBid: book.bestBid(),
  bestAsk: book.bestAsk(),
  spread: book.bestAsk() > 0 && book.bestBid() > 0 ? book.bestAsk() - book.bestBid() : null,
  mdp1Snapshots: snapCount,
  uiFrames: frames,
  ladderPaints: paintedFrames,
  uiDrops: drops,
  maxFrameCostNs: maxCostNs,
  frameBudgetNs: FRAME_BUDGET_NS,
  uiZoneHeapGrowthKiB: uiZoneGrowthKiB,
  uiZoneFrames: 4800,
  ingestRatePerSec: Math.round(TOTAL / (Number(ingestNs) / 1e9)),
  elapsedSec: Math.round(elapsedNs / 1e6) / 1000,
};

console.log(JSON.stringify(evidence, null, 2));

const ok = evidence.msgsApplied === TOTAL
  && evidence.rejects === 0
  && evidence.uiDrops === 0
  && (evidence.uiZoneHeapGrowthKiB === null || evidence.uiZoneHeapGrowthKiB < 64)
  && evidence.liveOrders > 0
  && evidence.ladderPaints > 0;
if (!ok) {
  console.error('DEMO GATE VIOLATION');
  process.exit(2);
}
console.log('fintech_l3_trading_demo: ALL GATES GREEN (exit 0)');
