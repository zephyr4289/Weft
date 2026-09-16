// spikes/fanout-heddles/fanout_concurrent_test.js — TRUE-parallelism torture
// test for the concurrent fan-out ring (fanout.js).
//
// Topology: 1 writer worker + 4 reader workers over ONE SharedArrayBuffer.
// This is the test the original prototype could never pass: it had no
// atomics, so there was no torn-read possibility to expose. Here the writer
// sprints while readers race it, and every claimed frame is verified against
// the full-width differential pattern (patWord — the 04-LITMUS §0.6 family).
//
// Phase 2 is the RFC 0004 composition ("daisy-chain"): ONE kernel reader
// (the broadcaster) claims from a real Triad instance (core/ts — the frozen
// 1W/1R kernel) and republishes into the fan-out ring. Consumers never touch
// the kernel; the kernel never learns about the fan-out. Kernel Freeze holds.
//
// Hard gates (any failure exits 1):
//   G1  writer completed exactly N publishes (kernel-side pubCount === N)
//   G2  every reader: 0 integrity violations across every fresh claim
//   G3  every reader converged to the final frame (lastSeen === N)
//   G4  every reader: drop accounting is EXACT —
//         totalDropped === lastSeq − firstSeq − (freshCount − 1)
//       (telescoping identity: each inter-claim gap is counted exactly once)
//   G5  every reader made ≥ 1 fresh claim
//   G6  no reader hit the safety timeout
'use strict';

const { isMainThread, Worker, workerData, parentPort } = require('worker_threads');
const { FanoutRing, patWord, ST_FRESH, ST_STALE, ST_NOT_READY, ST_EXHAUSTED } = require('./fanout.cjs');

const SLOT_COUNT = 4;
const PAYLOAD_WORDS = 64;      // 256 B payload per slot
const FRAMES = 400000;         // phase 1: writer sprint length
const DAISY_FRAMES = 120000;   // phase 2: kernel→ring daisy-chain length
const READERS = 4;
const TIMEOUT_MS = 30000;

// ─── worker roles ────────────────────────────────────────────────────────────

function writerLoop() {
  const { sab, frames } = workerData;
  const ring = new FanoutRing(sab, SLOT_COUNT, PAYLOAD_WORDS);
  const t0 = performance.now();
  for (let F = 1; F <= frames; F++) ring.publish(F);
  const elapsed = performance.now() - t0;
  ring.setDone(1);
  parentPort.postMessage({ elapsedMs: elapsed, pubPerSec: frames / (elapsed / 1000) });
}

function readerLoop() {
  const { sab, finalSeq, verify } = workerData;
  const ring = new FanoutRing(sab, SLOT_COUNT, PAYLOAD_WORDS);
  const target = new Int32Array(PAYLOAD_WORDS);
  const state = new Int32Array(3);   // [lastSeen, hasClaimed, tornRetries]
  const out = new Int32Array(4);

  let fresh = 0, dropped = 0, stale = 0, notReady = 0, exhausted = 0, violations = 0;
  let firstSeq = 0, lastSeq = 0;
  const t0 = performance.now();
  const deadline = t0 + TIMEOUT_MS;
  let spin = 0;

  for (;;) {
    ring.claimInto(target, state, out);
    const st = out[0];
    if (st === ST_FRESH) {
      const F = out[1] >>> 0;
      if (out[3] === 1) firstSeq = F;            // baseline claim: nothing behind
      else dropped = (dropped + out[2]) >>> 0;
      if (verify && !FanoutRing.verify(target, F | 0, PAYLOAD_WORDS)) violations++;
      fresh++;
      lastSeq = F;
      if (lastSeq === finalSeq) break;           // converged on the final frame
    } else if (st === ST_STALE) stale++;
    else if (st === ST_NOT_READY) notReady++;
    else if (st === ST_EXHAUSTED) exhausted++;
    else { violations++; break; }                // unknown status — hard defect
    // Contention nap: a reader that cannot make progress paces itself so the
    // writer keeps publishing (1 writer + 4 readers share the sandbox cores).
    if (st !== ST_FRESH && ++spin % 64 === 0) {
      let s = 0; while (s < 128) s++;            // brief spin — no allocation
      if (performance.now() > deadline) { parentPort.postMessage({ flag: 'timeout' }); return; }
    }
  }
  parentPort.postMessage({
    fresh, dropped: dropped >>> 0, stale, notReady, exhausted,
    violations, firstSeq, lastSeq, tornRetries: state[2] >>> 0,
    elapsedMs: performance.now() - t0,
  });
}

async function daisyWriter() {
  const { sab, frames } = workerData;
  const { Weft } = await import('../../core/ts/weft.ts');
  const ring = new FanoutRing(sab, SLOT_COUNT, PAYLOAD_WORDS);
  const weft = new Weft(PAYLOAD_WORDS * 4);    // kernel payload = 256 B
  const t0 = performance.now();
  for (let F = 1; F <= frames; F++) {
    // 1. Write the differential pattern (as bytes, litmus convention)
    //    through the kernel's writer cursor, publish, claim back.
    const bytes = weft.wBegin();
    for (let i = 0; i < PAYLOAD_WORDS; i++) {
      const w = patWord(F, i) >>> 0;
      bytes[i * 4 + 0] = w & 0xff;
      bytes[i * 4 + 1] = (w >>> 8) & 0xff;
      bytes[i * 4 + 2] = (w >>> 16) & 0xff;
      bytes[i * 4 + 3] = (w >>> 24) & 0xff;
    }
    weft.publish(F, PAYLOAD_WORDS * 4);
    weft.claim();
    const live = weft.rLive();                 // zero-alloc reader-held view
    // 2. Republish the claimed bytes into the ring slot (writer protocol).
    ring.publishBytes(F, live);
  }
  const elapsed = performance.now() - t0;
  ring.setDone(1);
  parentPort.postMessage({ role: 'daisy-writer', elapsedMs: elapsed, pubPerSec: frames / (elapsed / 1000) });
}

// ─── main ────────────────────────────────────────────────────────────────────

function runPhase(sab, frames, role) {
  const workers = [
    new Worker(__filename, { workerData: { role, sab, frames } }),
    ...Array.from({ length: READERS }, () =>
      new Worker(__filename, { workerData: { role: 'reader', sab, finalSeq: frames, verify: true } })),
  ];
  return new Promise((resolve, reject) => {
    let done = 0;
    const out = { writer: null, readers: [] };
    for (const w of workers) {
      w.on('message', (m) => {
        if (m.role === role || m.pubPerSec !== undefined) out.writer = m;
        else out.readers.push(m);
        if (++done === workers.length) resolve(out);
      });
      w.on('error', reject);
    }
  });
}

async function main() {
  // Phase 1: sprint torture — writer at full speed vs 4 verifying readers.
  const sab1 = new SharedArrayBuffer(FanoutRing.bytesFor(SLOT_COUNT, PAYLOAD_WORDS));
  const t0 = performance.now();
  const phase1 = await runPhase(sab1, FRAMES, 'writer');
  const p1ms = performance.now() - t0;

  // Phase 2: daisy-chain — Triad kernel → broadcaster → ring → readers.
  const sab2 = new SharedArrayBuffer(FanoutRing.bytesFor(SLOT_COUNT, PAYLOAD_WORDS));
  const t1 = performance.now();
  const phase2 = await runPhase(sab2, DAISY_FRAMES, 'daisy-writer');
  const p2ms = performance.now() - t1;

  // Gates.
  const failures = [];
  const gate = (ok, id, msg) => { if (!ok) failures.push(`${id}: ${msg}`); };

  // G1: the writers' own completion counters, read from main over the SABs.
  const pub1 = new Int32Array(sab1, 0, 4)[2] >>> 0;   // C_PUB_COUNT = word 2
  const pub2 = new Int32Array(sab2, 0, 4)[2] >>> 0;
  gate(pub1 === FRAMES, 'G1-phase1', `pubCount ${pub1} != ${FRAMES}`);
  gate(pub2 === DAISY_FRAMES, 'G1-phase2', `pubCount ${pub2} != ${DAISY_FRAMES}`);

  for (const [name, ph, total] of [['phase1', phase1, FRAMES], ['phase2', phase2, DAISY_FRAMES]]) {
    gate(ph.writer !== null, `G-writer-${name}`, 'writer report missing');
    gate(ph.readers.length === READERS, `G-readers-${name}`, `got ${ph.readers.length} reader reports`);
    for (let i = 0; i < ph.readers.length; i++) {
      const r = ph.readers[i];
      gate(!r.flag, `G6-${name}-r${i}`, 'reader hit safety timeout');
      gate(r.violations === 0, `G2-${name}-r${i}`, `integrity violations = ${r.violations}`);
      gate(r.lastSeq === total, `G3-${name}-r${i}`, `converged to ${r.lastSeq}, want ${total}`);
      const expectDropped = (r.lastSeq - r.firstSeq - (r.fresh - 1)) >>> 0;
      gate(r.dropped === expectDropped, `G4-${name}-r${i}`, `dropped ${r.dropped} != exact ${expectDropped}`);
      gate(r.fresh >= 1, `G5-${name}-r${i}`, 'no fresh claims');
    }
  }
  gate(p1ms < TIMEOUT_MS && p2ms < TIMEOUT_MS, 'G-time', 'phases finished within budget');

  // Report.
  const line = '─'.repeat(72);
  console.log(line);
  console.log('RFC 0004 — CONCURRENT fan-out torture test (worker_threads, true parallelism)');
  console.log(line);
  const w1 = phase1.writer, w2 = phase2.writer;
  console.log(`phase 1  sprint : writer ${(w1.pubPerSec / 1e6).toFixed(3)}M pub/s over ${FRAMES} frames (${w1.elapsedMs.toFixed(0)} ms) — 4 verifying readers`);
  for (let i = 0; i < phase1.readers.length; i++) {
    const r = phase1.readers[i];
    console.log(`  reader[${i}] fresh=${r.fresh} dropped=${r.dropped} stale=${r.stale} tornRetries=${r.tornRetries} notReady=${r.notReady} exhausted=${r.exhausted} violations=${r.violations} (${r.elapsedMs.toFixed(0)} ms)`);
  }
  console.log(`phase 2  daisy  : Triad kernel → broadcaster → ring → 4 readers over ${DAISY_FRAMES} frames (${w2.elapsedMs.toFixed(0)} ms, ${(w2.pubPerSec / 1e3).toFixed(0)}K frames/s)`);
  for (let i = 0; i < phase2.readers.length; i++) {
    const r = phase2.readers[i];
    console.log(`  reader[${i}] fresh=${r.fresh} dropped=${r.dropped} stale=${r.stale} tornRetries=${r.tornRetries} notReady=${r.notReady} exhausted=${r.exhausted} violations=${r.violations} (${r.elapsedMs.toFixed(0)} ms)`);
  }
  console.log(line);
  if (failures.length) {
    console.log('GATE: FAILED ❌');
    for (const f of failures) console.log('  ✗ ' + f);
    process.exit(1);
  }
  console.log('GATE: ALL PASSED ✅  (integrity, convergence, exact drop accounting, daisy-chain)');
  process.exit(0);
}

if (isMainThread) {
  main().catch((e) => { console.error('torture test error:', e); process.exit(2); });
} else if (workerData.role === 'writer') writerLoop();
else if (workerData.role === 'reader') readerLoop();
else if (workerData.role === 'daisy-writer') daisyWriter();
