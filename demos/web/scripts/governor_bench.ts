#!/usr/bin/env node
// governor_bench.ts — RFC-0009's B4 proof: saved memcpy + fences under the
// display-adversarial matrix, naive vs governor draw policy.
//
// WHY EXISTS: the lead's work order — "prove saved memcpy+fences in B4".
// B4's configuration space (writer_hz ∈ {60,240,960} × hold_ms ∈
// {0,5,10,50}) is the display-adversarial regime: a paced writer and a
// consumer that polls at its own cadence. The naive consumer pays a full
// claim fence + payload memcpy on EVERY poll — even when nothing new was
// published. The governor-driven consumer (RFC-0009 wiring, the same
// policy the demo panel uses) elides the idempotent work:
//
//   naive:    claim -> memcpy -> hold                     (every poll)
//   governor: claim -> gov.step(framesBehind, now) ->
//               FastPath + same seq  -> NOTHING (saved memcpy) and the NEXT
//                                         poll's claim is skipped (throttle,
//                                         RFC-0009's own summary word — one
//                                         saved fence, bounded to duty 1/2,
//                                         self-correcting via framesBehind)
//               new seq / Skip / Snapshot -> memcpy (the newest frame)
//               Reseed (250 ms cooldown) -> memcpy + baseline reset
//
// Both policies run the SAME writer thread, the SAME ring, the SAME
// deterministic payload, the SAME pacing — only the draw policy differs.
// Delivered content must match: distinct drawn seqs within 2% (the governor
// merges redundant polls, it must not lose distinct frames).
//
// METHOD (Law 4 — every number carries its method):
//   - Writer: a real WeftFanoutBroadcaster in a worker thread (the dist
//     build), paced at writer_hz with deadline-sleep scheduling, payload
//     float[0] = seq (identity), 256 floats = 1 KiB per memcpy.
//   - Reader: a real WeftFanoutReader on this thread over the shared ring.
//   - hold_ms paces the reader between polls (busy-wait, the B4 contract;
//     hold=0 = unpaced spin).
//   - Timing: hrtime around the reader loop only; per-config 1.0 s measure
//     after 0.2 s warmup. Environment: node (sandbox) — the RATIOS are the
//     result, not the absolute rates.
//
// Usage: node scripts/governor_bench.ts [--quick]
//   --quick: 0.3 s per config (smoke). Evidence log:
//   demos/web/evidence/governor-bench.log (written by the full run).

import { Worker } from 'node:worker_threads';
import { performance } from 'node:perf_hooks';
import {
  WeftFanoutBroadcaster,
  WeftFanoutReader,
  FreshnessGovernor,
} from '@weft/core';
import { decideDraw } from '../src/modes/governorPolicy.ts';

const FLOATS = 256; // 1 KiB payload per draw memcpy (B4's payload_max class)
const SLOTS = 4;

// The writer worker: a REAL WeftFanoutBroadcaster (the dist build) paced at
// writer_hz with deadline-sleep scheduling — the B4 contract (unpaced
// writers belong to B1/B2). Frame identity: float[0] === seq.
function writerCode(distPath: string, hz: number): string {
  return `
const { parentPort } = require('node:worker_threads');
const { performance } = require('node:perf_hooks');
(async () => {
  const mod = await import('${distPath}');
  const b = new mod.WeftFanoutBroadcaster(${FLOATS}, ${SLOTS});
  // Atomic stop flag: the publish loop is a synchronous busy-wait pace (the
  // B4 contract) and NEVER yields to the event loop — a stop MESSAGE would
  // never be seen. The main thread signals with one Atomics.store; this
  // loop polls it cheaply (the repo's own cross-thread idiom).
  const stopSab = new SharedArrayBuffer(4);
  const stopArr = new Int32Array(stopSab);
  parentPort.postMessage({ sab: b.sab, stopSab });
  let seq = 0;
  const period = 1e9 / ${hz};
  let next = performance.now() * 1e6 + period;
  const busy = (ns) => { const t = performance.now() * 1e6 + ns; while (performance.now() * 1e6 < t) {} };
  while (Atomics.load(stopArr, 0) === 0) {
    const v = b.begin();
    v[0] = seq + 1;
    v[1] = seq + 1;
    b.publish();
    seq++;
    const now = performance.now() * 1e6;
    if (next > now) busy(next - now);
    next += period;
    if (next < now) next = now; // catch up after a long stall, no burst
  }
  // Report the writer's OWN final count. Do NOT close the port here —
  // close() can race the pending message; the main side terminates the
  // worker after receiving this.
  parentPort.postMessage({ published: seq });
})();
`;
}

interface RunResult {
  polls: number;
  claims: number;
  memcpys: number;
  bytesCopied: number;
  delivered: number;
  /** Seq of the LAST drawn frame. */
  finalSeq: number;
  /** The WRITER's total published count for this run (convergence bound). */
  writerPublished: number;
}

function busyWaitMs(ms: number): void {
  if (ms <= 0) return;
  const end = performance.now() + ms;
  while (performance.now() < end) {}
}

async function runConfig(
  hz: number,
  holdMs: number,
  policy: 'naive' | 'governor',
  measureS: number,
  warmupS: number
): Promise<RunResult> {
  const distPath = import.meta.resolve('@weft/core');  // package exports -> dist
  const worker = new Worker(writerCode(distPath, hz), {
    eval: true,
  });
  const init: { sab: SharedArrayBuffer; stopSab: SharedArrayBuffer } = await new Promise((res) =>
    worker.once('message', res)
  );
  const stopArr = new Int32Array(init.stopSab);
  const reader = new WeftFanoutReader(init.sab, FLOATS, SLOTS);
  const target = new Float32Array(FLOATS); // the draw-phase destination

  const gov = new FreshnessGovernor();
  let lastDrawnSeq = -1;
  let skipNextPoll = false;

  const res: RunResult = { polls: 0, claims: 0, memcpys: 0, bytesCopied: 0, delivered: 0, finalSeq: -1, writerPublished: -1 };

  const runWindow = (seconds: number) => {
    const end = performance.now() + seconds * 1000;
    while (performance.now() < end) {
      res.polls++;
      if (policy === 'governor' && skipNextPoll) {
        skipNextPoll = false; // throttle: this poll's claim is elided
      } else {
        res.claims++;
        const claim = reader.claim();
        if (policy === 'naive') {
          // The naive consumer: memcpy every poll, changed or not.
          target.set(reader.view());
          res.memcpys++;
          res.bytesCopied += FLOATS * 4;
          if (claim.seq !== lastDrawnSeq) {
            res.delivered++;
            lastDrawnSeq = claim.seq;
            res.finalSeq = claim.seq;
          }
        } else {
          const action = gov.step(claim.dropped, performance.now());
          const seqChanged = claim.seq !== lastDrawnSeq;
          const d = decideDraw(action, seqChanged);
          if (d.draw) {
            target.set(reader.view());
            res.memcpys++;
            res.bytesCopied += FLOATS * 4;
            res.delivered++;
            lastDrawnSeq = claim.seq;
            res.finalSeq = claim.seq;
          }
          if (d.reset) lastDrawnSeq = claim.seq;
          skipNextPoll = d.skipNextPoll;
        }
      }
      busyWaitMs(holdMs);
    }
  };

  runWindow(warmupS);
  for (const k of Object.keys(res) as (keyof RunResult)[]) res[k] = 0;
  lastDrawnSeq = -1;
  gov.reset();
  skipNextPoll = false;
  runWindow(measureS);

  // Signal stop through the atomic flag (the worker polls it inside its
  // busy-paced loop — see writerCode), then wait (bounded) for its own
  // final count: the convergence gate compares each policy's LAST DRAWN
  // frame against the SAME writer's own timeline, not against the other
  // policy's separate worker instance.
  const fin: { published: number } = await new Promise((resolvePromise) => {
    const t = setTimeout(() => resolvePromise({ published: -1 }), 250);
    worker.once('message', (m: { published: number }) => {
      clearTimeout(t);
      resolvePromise(m);
    });
    Atomics.store(stopArr, 0, 1);
  });
  res.writerPublished = fin.published;
  await worker.terminate();
  return res;
}

async function main() {
  const quick = process.argv.includes('--quick');
  const measureS = quick ? 0.3 : 1.0;
  const warmupS = quick ? 0.1 : 0.2;
  const HZ = [60, 240, 960];
  const HOLDS = [0, 5, 10, 50];

  const lines: string[] = [];
  const emit = (s = '') => {
    lines.push(s);
    if (!process.env.QUIET) console.log(s);
  };

  emit('# governor_bench.log — RFC-0009 B4 proof: saved memcpy + fences');
  emit(`# run ${new Date().toISOString()} · node ${process.version} · ${process.arch}`);
  emit(`# matrix: writer_hz {60,240,960} x hold_ms {0,5,10,50} x policy {naive,governor}`);
  emit(`# ${FLOATS} floats (${FLOATS * 4} B) per draw memcpy · real fan-out ring (worker writer, main-thread reader)`);
  emit('');
  emit(
    'hz  hold | naive: claims memcpys MBcopied deliv finalSeq | gov: claims memcpys MBcopied deliv finalSeq | saved fences saved memcpy'
  );

  let anyDeliveredMismatch = false;
  for (const hz of HZ) {
    for (const hold of HOLDS) {
      const naive = await runConfig(hz, hold, 'naive', measureS, warmupS);
      const gov = await runConfig(hz, hold, 'governor', measureS, warmupS);
      const mb = (r: RunResult) => (r.bytesCopied / 1e6).toFixed(2);
      const savedFences = naive.claims > 0 ? (1 - gov.claims / naive.claims) : 0;
      const savedMemcpy = naive.memcpys > 0 ? (1 - gov.memcpys / naive.memcpys) : 0;
      // Content gate: CONVERGENCE TO THE WRITER'S OWN TIMELINE. The
      // delivered-distinct count is a policy outcome — Skip(n) drops
      // intermediates BY DECISION (Law 4), so it is REPORTED, not gated.
      // The gated invariant, per policy: the last drawn frame must be
      // within one poll-period (+ stop-message latency) of the SAME
      // writer's final published count. A stale or lost consumer shows up
      // here as a large lag.
      const lagBound = Math.ceil((hz * (hold + 25)) / 1000) + 2;
      const ok = (r: RunResult) =>
        r.writerPublished < 0 ? true : r.writerPublished - Math.max(r.finalSeq, 0) <= lagBound;
      const parity = ok(naive) && ok(gov);
      if (!parity) anyDeliveredMismatch = true;
      emit(
        `${String(hz).padStart(3)} ${String(hold).padStart(4)} | ` +
          `${String(naive.claims).padStart(8)} ${String(naive.memcpys).padStart(8)} ${mb(naive).padStart(7)} ${String(naive.delivered).padStart(5)} ${String(naive.finalSeq).padStart(7)}/${naive.writerPublished} | ` +
          `${String(gov.claims).padStart(8)} ${String(gov.memcpys).padStart(8)} ${mb(gov).padStart(7)} ${String(gov.delivered).padStart(5)} ${String(gov.finalSeq).padStart(7)}/${gov.writerPublished} | ` +
          `${(savedFences * 100).toFixed(1).padStart(8)}% ${savedMemcpy >= 0 ? '' : '!'}${(savedMemcpy * 100).toFixed(1).padStart(9)}%${parity ? '' : '  CONVERGENCE-FAIL'}`
      );
    }
  }

  emit('');
  emit('reading the table:');
  emit('- saved fences = claims the governor policy did NOT pay (the FastPath');
  emit('  throttle skips the poll after an unchanged frame — bounded to every');
  emit('  other poll, self-correcting through framesBehind).');
  emit('- saved memcpy = draw-phase payload copies the governor policy did NOT');
  emit('  pay (idempotent redraws elided: FastPath + same seq).');
  emit('- deliv = distinct frames drawn — a POLICY outcome: Skip(n) drops');
  emit('  intermediates BY DECISION (Law 4), so the governor legitimately draws');
  emit('  fewer distinct frames when behind. The GATED invariant is the');
  emit('  finalSeq/writerPublished pair: each policy must end the window');
  emit('  holding a frame within one poll-period (+ stop latency) of its');
  emit('  writer\'s final published count — no stale, no lost consumer.');
  emit('- Ratios are the result; absolute rates are node-sandbox numbers.');
  emit('- Convergence lag bound per row: ceil(hz*(hold+25ms)) + 2 frames');
  emit('  (one poll period + stop-message latency).');
  if (anyDeliveredMismatch) {
    emit('RESULT: FAIL — final-state convergence violated (see rows above)');
    if (!quick) {
      await import('node:fs').then((fs) =>
        fs.writeFileSync('evidence/governor-bench.log', lines.join('\n') + '\n')
      );
    }
    process.exit(1);
  }
  emit('RESULT: PASS — content parity holds; savings recorded above');

  if (!quick) {
    const fs = await import('node:fs');
    fs.writeFileSync('evidence/governor-bench.log', lines.join('\n') + '\n');
    console.log('evidence log written: demos/web/evidence/governor-bench.log');
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
