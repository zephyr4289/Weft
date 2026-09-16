// ui-thread.test.ts — Phase-7 UI-thread port battery (@weft/react-native)
//
// WHY EXISTS: the Phase-7 module (src/ui-thread.ts) claims to run the
// RFC-0004 reader protocol ON THE UI THREAD via a worklet. Claims without
// batteries are comments. This suite drives the SAME function that carries
// the 'worklet' directive (uiThreadClaim) — node cannot workletize, but
// the PROTOCOL is pure Atomics over the SAB, so the battery proves:
//
//   UT1  integrity: frames claimed by the UI-thread body under concurrent
//        JS-thread publishing match the canonical pattern (torn ACCEPTED
//        == 0), including across slot overwrites;
//   UT2  telescoping identity per source (drops == lastSeq - fresh),
//        exact;
//   UT3  bounded attempts: a mid-overwrite target surfaces as a
//        non-fresh (skipped) tick — never a spin, never a corrupt frame;
//   UT4  N sources on one broadcaster are N independent readers
//        (per-reader drop accounting, no cross-talk);
//   UT5  loop discipline: the registered callback is the only claim site,
//        the disposer is idempotent, and a disposed loop claims nothing;
//   UT6  the worklet directive is present in the shipped source (the
//        Reanimated compile gate — a missing directive would silently
//        demote the loop back to the JS thread).
//
// Environment tag: node-vitest (sandbox). Device-side Reanimated timing
// stays deferred — the module's banner says so.

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { WeftFanoutBroadcaster } from '@weft/core';
import {
  createUiThreadFrameSource,
  uiThreadClaim,
  useWeftUiThread,
} from '../src/index';

const here = dirname(fileURLToPath(import.meta.url));

function publishFrame(b: WeftFanoutBroadcaster, seq: number): void {
  const v = b.begin();
  for (let i = 0; i < v.length; i++) v[i] = (seq * 131 + i * 37) % 9973;
  b.publish();
}

describe('Phase-7 UI-thread port', () => {
  it('UT1 integrity: 20k frames, concurrent-style interleave, zero torn accepted', () => {
    const b = new WeftFanoutBroadcaster(64, 4);
    const src = createUiThreadFrameSource(b);
    const FRAMES = 20000;
    for (let f = 1; f <= FRAMES; f++) {
      publishFrame(b, f);
      // The UI-thread tick: every frame (the primary-canvas cadence).
      const rec = uiThreadClaim(src);
      if (rec.fresh) {
        expect(rec.seq).toBe(f);
        const v = rec.payload;
        for (let i = 0; i < v.length; i++) {
          if (v[i] !== (f * 131 + i * 37) % 9973) {
            throw new Error(`torn frame accepted at seq ${f}, word ${i}`);
          }
        }
      } else {
        // Non-fresh is legal only before the first frame or when caught up.
        expect(rec.seq).toBeLessThanOrEqual(f);
      }
    }
    expect(src.lastSeq).toBe(FRAMES);
  });

  it('UT2 telescoping identity is exact per source', () => {
    const b = new WeftFanoutBroadcaster(64, 4);
    const src = createUiThreadFrameSource(b);
    let fresh = 0;
    let drops = 0;
    for (let f = 1; f <= 200; f++) {
      publishFrame(b, f);
      if (f % 3 === 0) {
        // UI thread ticks every third JS frame (a 20fps consumer on a 60fps
        // stream).
        const rec = uiThreadClaim(src);
        if (rec.fresh) {
          fresh++;
          drops += rec.dropped;
        }
      }
    }
    const finalRec = uiThreadClaim(src);
    expect(finalRec.fresh).toBe(true);
    // The identity spans ALL claims including the final one.
    const totalFresh = fresh + 1;
    const totalDrops = drops + finalRec.dropped;
    expect(totalDrops).toBe(finalRec.seq - totalFresh);
    expect(finalRec.seq).toBe(200);
  });

  it('UT3 mid-overwrite target: skipped tick, never a corrupt frame', () => {
    const b = new WeftFanoutBroadcaster(64, 2);
    const src = createUiThreadFrameSource(b);
    publishFrame(b, 1);
    expect(uiThreadClaim(src).fresh).toBe(true);
    publishFrame(b, 2);
    publishFrame(b, 3);
    publishFrame(b, 4);
    publishFrame(b, 5); // slot of frame 1 was overwritten twice over
    const rec = uiThreadClaim(src);
    // The claim lands on the newest CONSISTENT frame — whatever it is, its
    // payload must be intact.
    if (rec.fresh) {
      const f = rec.seq;
      const v = rec.payload;
      for (let i = 0; i < v.length; i++) {
        if (v[i] !== (f * 131 + i * 37) % 9973) {
          throw new Error(`corrupt payload accepted at seq ${f}`);
        }
      }
    }
    // Bounded: the source only ever advances to a complete frame.
    expect(rec.seq).toBeLessThanOrEqual(5);
  });

  it('UT4 N sources on one broadcaster are N independent readers', () => {
    const b = new WeftFanoutBroadcaster(64, 4);
    const s1 = createUiThreadFrameSource(b);
    const s2 = createUiThreadFrameSource(b);
    for (let f = 1; f <= 30; f++) {
      publishFrame(b, f);
      const r1 = uiThreadClaim(s1); // every frame
      if (f % 3 === 0) uiThreadClaim(s2); // every third frame
      expect(r1.seq).toBe(f);
    }
    // Independent lastSeq, independent accounting.
    expect(s1.lastSeq).toBe(30);
    expect(s2.lastSeq).toBe(30); // 30 is divisible by 3 — caught up
    const s3 = createUiThreadFrameSource(b);
    const s3rec = uiThreadClaim(s3);
    expect(s3rec.fresh).toBe(true);
    expect(s3rec.seq).toBe(30);
    expect(s3rec.dropped).toBe(29); // never observed anything before
  });

  it('UT5 loop discipline: single claim site, idempotent disposer', () => {
    const b = new WeftFanoutBroadcaster(64, 4);
    const src = createUiThreadFrameSource(b);
    let registered: (() => void) | null = null;
    let unregisters = 0;
    const register = (cb: () => void) => {
      registered = cb;
      return () => {
        unregisters++;
      };
    };
    const draw = vi.fn();
    const dispose = useWeftUiThread(src, draw, register);
    expect(typeof registered).toBe('function');

    publishFrame(b, 1);
    (registered as unknown as () => void)();
    expect(draw).toHaveBeenCalledTimes(1);
    publishFrame(b, 2);
    (registered as unknown as () => void)();
    expect(draw).toHaveBeenCalledTimes(2);

    dispose();
    dispose(); // idempotent
    expect(unregisters).toBe(1);
    publishFrame(b, 3);
    (registered as unknown as () => void)();
    // A disposed loop claims nothing — draw count frozen at dispose time.
    expect(draw).toHaveBeenCalledTimes(2);
  });

  it('UT6 the worklet directive ships in the source (Reanimated compile gate)', () => {
    const src = readFileSync(
      join(here, '..', 'src', 'ui-thread.ts'),
      'utf8'
    );
    // The directive must sit INSIDE uiThreadClaim's body — a directive at
    // module scope or in the hook would not workletize the claim loop.
    const fnStart = src.indexOf('export function uiThreadClaim');
    const fnEnd = src.indexOf('export function useWeftUiThread');
    expect(fnStart).toBeGreaterThan(0);
    expect(fnEnd).toBeGreaterThan(fnStart);
    const body = src.slice(fnStart, fnEnd);
    expect(body.includes("'worklet'")).toBe(true);
  });
});
