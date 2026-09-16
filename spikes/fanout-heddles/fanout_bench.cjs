// spikes/fanout-heddles/fanout_bench.cjs — throughput evidence for the
// CONCURRENT fan-out ring. Companion to fanout_concurrent_test.cjs (which
// gates correctness); this file measures rates under the same true
// parallelism (1 writer worker + 4 reader workers, SharedArrayBuffer).
//
// RFC 0004's "Verified 1.27 million publishes/sec across 4 concurrent
// readers" claim was produced by a single-threaded prototype — no atomics,
// no torn-read exposure, one core. This bench produces the honest number:
// writer rate WITH the full latch protocol AND readers actively contending.
//
// Usage: node fanout_bench.cjs [frames]
'use strict';

const { isMainThread, Worker, workerData, parentPort } = require('worker_threads');
const { FanoutRing, ST_FRESH } = require('./fanout.cjs');

const SLOT_COUNT = 4;
const PAYLOAD_WORDS = 64;          // 256 B payload — same as the prototype
const DEFAULT_FRAMES = 1000000;
const READERS = 4;
const TIMEOUT_MS = 60000;

function writerLoop() {
  const { sab, frames } = workerData;
  const ring = new FanoutRing(sab, SLOT_COUNT, PAYLOAD_WORDS);
  const t0 = performance.now();
  for (let F = 1; F <= frames; F++) ring.publish(F);
  const elapsed = performance.now() - t0;
  ring.setDone(1);
  parentPort.postMessage({ elapsedMs: elapsed, pubPerSec: frames / (elapsed / 1000) });
}

// Bench reader: same hot path as the torture reader, verification off —
// measures the raw claim rate a consumer can sustain under writer pressure.
function benchReaderLoop() {
  const { sab, frames } = workerData;
  const ring = new FanoutRing(sab, SLOT_COUNT, PAYLOAD_WORDS);
  const target = new Int32Array(PAYLOAD_WORDS);
  const state = new Int32Array(3);
  const out = new Int32Array(4);
  let fresh = 0, dropped = 0, claims = 0;
  const t0 = performance.now();
  const deadline = t0 + TIMEOUT_MS;
  let spin = 0;
  for (;;) {
    ring.claimInto(target, state, out);
    claims++;
    if (out[0] === ST_FRESH) {
      if (out[3] !== 1) dropped = (dropped + out[2]) >>> 0;
      fresh++;
      if ((out[1] >>> 0) === frames) break;
    } else if (++spin % 64 === 0 && performance.now() > deadline) {
      parentPort.postMessage({ flag: 'timeout', fresh, claims }); return;
    }
  }
  parentPort.postMessage({
    fresh, dropped: dropped >>> 0, claims,
    elapsedMs: performance.now() - t0,
  });
}

async function main() {
  const frames = Number(process.argv[2]) || DEFAULT_FRAMES;
  const sab = new SharedArrayBuffer(FanoutRing.bytesFor(SLOT_COUNT, PAYLOAD_WORDS));
  const workers = [
    new Worker(__filename, { workerData: { role: 'writer', sab, frames } }),
    ...Array.from({ length: READERS }, () =>
      new Worker(__filename, { workerData: { role: 'bench-reader', sab, frames } })),
  ];
  const res = await new Promise((resolve, reject) => {
    let done = 0;
    const out = { writer: null, readers: [] };
    for (const w of workers) {
      w.on('message', (m) => {
        if (m.pubPerSec !== undefined) out.writer = m; else out.readers.push(m);
        if (++done === workers.length) resolve(out);
      });
      w.on('error', reject);
    }
  });

  const line = '─'.repeat(72);
  console.log(line);
  console.log('RFC 0004 — CONCURRENT fan-out benchmark (1 writer + 4 readers, worker_threads)');
  console.log(line);
  console.log('Environment Tag: node / linux-sandbox (SharedArrayBuffer + Atomics, all SeqCst)');
  console.log(`Writer: ${frames} publishes in ${res.writer.elapsedMs.toFixed(0)} ms  →  ${(res.writer.pubPerSec / 1e6).toFixed(3)}M publishes/sec  (latch protocol INCLUDED)`);
  let totalFresh = 0;
  for (let i = 0; i < res.readers.length; i++) {
    const r = res.readers[i];
    totalFresh += r.fresh;
    const rate = r.fresh / (r.elapsedMs / 1000);
    console.log(`  reader[${i}]: fresh=${r.fresh} dropped=${r.dropped} claims=${r.claims} → ${(rate / 1e6).toFixed(3)}M fresh claims/sec (${r.elapsedMs.toFixed(0)} ms)`);
  }
  console.log(`Aggregate: ${(totalFresh / 1e6).toFixed(3)}M fresh deliveries across 4 readers; drops are per-reader (Law 4) and exact.`);
  console.log('Law 2 invariant: publish/claim paths allocate nothing (pre-allocated views + out-params).');
  console.log(line);
}

if (isMainThread) {
  main().catch((e) => { console.error('bench error:', e); process.exit(2); });
} else if (workerData.role === 'writer') writerLoop();
else if (workerData.role === 'bench-reader') benchReaderLoop();
