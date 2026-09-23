// freeze.mjs — freeze MDP1 checkpoint snapshots + parity hash manifest.
//
// Runs the TS engine over the golden ITCH stream, snapshots the book at the
// scenario checkpoints (512/1024/1536/2048 msgs), and writes:
//   expected-hashes/mdp1-ckpt-<N>.bin     frozen 304-byte MDP1 snapshots
//   expected-hashes/expected_hashes.json  sha256 of each + parityHash +
//                                         final book stats (ground truth)
//
// The manifest is the PARITY GROUND TRUTH: Python (and the Swift/Dart
// mirrors pinned by static audit) must reproduce these hashes bit-for-bit
// from the same stream. Double-run determinism is asserted by the suite.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { OrderBook } from '../../../../packages/fintech/src/book.js';
import { ItchEngine, frameOffsets } from '../../../../packages/fintech/src/itch.js';
import { packSnapshot, MDP1_SIZE, MDP1_TOP_LEVELS } from '../../../../packages/fintech/src/mdp1.js';
import { CHECKPOINTS } from './generate.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const outDir = join(here, 'expected-hashes');
mkdirSync(outDir, { recursive: true });

const stream = readFileSync(join(here, 'fintech-stream.bin'));

function runOnce() {
  const book = new OrderBook({ poolCapacity: 8192, baseTick: 900_000, tickCount: 262_144 });
  const engine = new ItchEngine(book);
  const offs = frameOffsets(stream, stream.length);
  const snaps = new Map();
  const scratchBids = new Int32Array(MDP1_TOP_LEVELS);
  const scratchAsks = new Int32Array(MDP1_TOP_LEVELS);
  const snapBuf = new Uint8Array(MDP1_SIZE);
  for (const cp of CHECKPOINTS) {
    engine.process(stream, cp < offs.length ? offs[cp] : stream.length);
    if (book.msgsApplied !== cp) throw new Error(`checkpoint drift: applied ${book.msgsApplied} != ${cp}`);
    packSnapshot(book, snapBuf, scratchBids, scratchAsks);
    snaps.set(cp, Buffer.from(snapBuf)); // copy — snapBuf is reused
  }
  return { book, snaps, offs };
}

const { book, snaps } = runOnce();
const { book: book2, snaps: snaps2 } = runOnce();

// determinism gate: double-run identical
for (const cp of CHECKPOINTS) {
  if (!snaps.get(cp).equals(snaps2.get(cp))) throw new Error(`nondeterministic snapshot @${cp}`);
}

const hashes = {};
const concat = [];
for (const cp of CHECKPOINTS) {
  const buf = snaps.get(cp);
  writeFileSync(join(outDir, `mdp1-ckpt-${cp}.bin`), buf);
  hashes[`mdp1-ckpt-${cp}.bin`] = createHash('sha256').update(buf).digest('hex');
  concat.push(buf);
}
const parityHash = createHash('sha256').update(Buffer.concat(concat)).digest('hex');

const manifest = {
  schema: 'weft-adapters-parity-v1',
  stream_sha256: createHash('sha256').update(stream).digest('hex'),
  checkpoints: CHECKPOINTS,
  parity_hash: parityHash,
  final: {
    msgsApplied: book.msgsApplied,
    skipped: book.skipped,
    liveOrders: book.liveOrders,
    tradeCount: book.tradeCount,
    lastMatch: book.lastMatch,
    lastTs: book.lastTs,
    bestBid: book.bestBid(),
    bestAsk: book.bestAsk(),
    rejectsTotal: book.totalRejects(),
  },
  hashes,
};

writeFileSync(join(outDir, 'expected_hashes.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(`frozen ${CHECKPOINTS.length} checkpoints; parityHash=${parityHash.slice(0, 16)}; liveOrders=${book.liveOrders}; rejects=${book.totalRejects()}; skipped=${book.skipped}`);
