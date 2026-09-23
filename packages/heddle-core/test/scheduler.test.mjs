// test/scheduler.test.mjs — fixed cadence, drop-not-queue, pause/resume,
// zero-allocation frame context reuse. Virtual clock = fully deterministic.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { FrameScheduler } from '../src/index.js';

const HZ240 = 1e9 / 240; // 4_166_666.67 ns

test('renders once per interval at 240 Hz on a virtual clock', () => {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  const seen = [];
  s.arm((ctx) => seen.push(ctx.frame));
  let t = 0;
  for (let i = 0; i < 480; i++) { t += HZ240; s.pump(t); }
  assert.equal(seen.length, 480);
  assert.equal(s.rendered, 480);
  assert.equal(s.skipped, 0);
  s.stop();
});

test('does not render before the interval elapses', () => {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  let n = 0;
  s.arm(() => { n += 1; });
  let t = 0;
  for (let i = 0; i < 100; i++) { t += HZ240 / 2; s.pump(t); }
  assert.equal(n, 50); // half-rate pumps → every other pump renders
  s.stop();
});

test('drop-not-queue: a 10 ms stall is followed by ONE frame, skips counted', () => {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  const frames = [];
  s.arm((ctx) => frames.push({ f: ctx.frame, skipped: ctx.skipped }));
  let t = 0;
  s.pump(t);             // frame 1 at t=0
  t += 10 * 1e6;         // 10 ms stall = 2.4 intervals at 240 Hz
  assert.equal(s.pump(t), true);
  assert.equal(frames.length, 2);
  assert.ok(frames[1].skipped >= 1, 'stall must surface as skipped frames');
  assert.equal(s.skipped, frames[1].skipped);
  // exactly one interval of advance — no catch-up burst
  t += HZ240;
  assert.equal(s.pump(t), true);
  assert.equal(frames.length, 3);
  s.stop();
});

test('a 1-second stall produces ~240 skips and one render — never 240 renders', () => {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  let n = 0;
  s.arm(() => { n += 1; });
  let t = 0;
  s.pump(t);
  t += 1e9; // full second frozen
  s.pump(t);
  assert.equal(n, 2);
  assert.ok(s.skipped >= 230 && s.skipped <= 242, `skips=${s.skipped}`);
  s.stop();
});

test('frame context object is REUSED across frames (Law 1)', () => {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  const ctxs = [];
  s.arm((ctx) => ctxs.push(ctx));
  let t = 0;
  for (let i = 0; i < 10; i++) { t += HZ240; s.pump(t); }
  assert.equal(ctxs.length, 10);
  for (let i = 1; i < 10; i++) assert.equal(ctxs[i], ctxs[0]);
  assert.equal(ctxs[0].frame, 10);
  s.stop();
});

test('pause/resume: paused pumps render nothing; resume does not catch up', () => {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  let n = 0;
  s.arm(() => { n += 1; });
  let t = 0;
  s.pump(t);
  s.pause();
  for (let i = 0; i < 50; i++) { t += HZ240; s.pump(t); }
  assert.equal(n, 1);
  assert.equal(s.pauseCount, 1);
  s.resume(t);
  assert.equal(s.resumeCount, 1);
  t += HZ240;
  assert.equal(s.pump(t), true); // renders again exactly one interval later
  assert.equal(n, 2);
  s.stop();
});

test('startWithTimer drives a real timer loop (setTimeout path)', async () => {
  // real advancing clock — the frozen virtual clock would never re-arm
  const s = new FrameScheduler({ hz: 1000, raf: null, now: () => performance.now() * 1e6 });
  let n = 0;
  s.startWithTimer(() => { n += 1; }, 2);
  await new Promise((r) => setTimeout(r, 12));
  s.stop();
  assert.ok(n >= 2, `timer loop should pump, got ${n}`);
  const before = n;
  await new Promise((r) => setTimeout(r, 6));
  assert.equal(n, before, 'stop() halts the loop');
});

test('hz guard', () => {
  assert.throws(() => new FrameScheduler({ hz: 0 }), RangeError);
  assert.throws(() => new FrameScheduler({ hz: -5 }), RangeError);
});
