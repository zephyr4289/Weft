/// FrameCursor tests — RFC-0008 freshness telemetry (driver layer).
///
/// Pins: per-reader drop accounting from claimed envelope seqs, the null
/// frame baseline (first claim reports 0 behind), zero drops for a reader
/// that keeps pace, correct accounting when the writer outruns the reader,
/// and the wrap/reset rule (decreasing seq resets accounting).
import { describe, it, expect } from 'vitest';
import { FrameCursor } from '../src/cursor';
import { Weft, mix32, pat } from '../src/index';

describe('FrameCursor (RFC-0008)', () => {
  it('pat(0,0) fixture sanity (differential seed A5)', () => {
    expect(pat(0, 0)).toBe(mix32((Math.imul(0, 2654435761) + Math.imul(0, 2246822519)) >>> 0) & 0xff);
  });

  it('first claim is the baseline: framesBehind = 0, first = true', () => {
    const weft = new Weft(64);
    const cursor = new FrameCursor();
    weft.publish(1, 64);
    const c = cursor.claim(weft);
    expect(c.first).toBe(true);
    expect(c.framesBehind).toBe(0);
    expect(c.seq).toBe(1);
  });

  it('a paced reader sees zero drops', () => {
    const weft = new Weft(64);
    const cursor = new FrameCursor();
    for (let seq = 1; seq <= 100; seq++) {
      weft.publish(seq, 64);
      const c = cursor.claim(weft);
      expect(c.framesBehind).toBe(0);
      expect(c.seq).toBe(seq);
    }
    expect(cursor.totalDropped).toBe(0);
    expect(cursor.claims).toBe(100);
  });

  it('counts frames published between claims that the reader never saw', () => {
    const weft = new Weft(64);
    const cursor = new FrameCursor();

    weft.publish(1, 64);
    const first = cursor.claim(weft); // baseline: seq 1

    // Writer sprints ahead: publishes 2..10, reader skips all but the last.
    for (let seq = 2; seq <= 10; seq++) weft.publish(seq, 64);
    const second = cursor.claim(weft);

    expect(first.framesBehind).toBe(0);
    expect(second.seq).toBe(10);
    // Frames 2..9 were published and never seen.
    expect(second.framesBehind).toBe(8);
    expect(cursor.totalDropped).toBe(8);

    // Back-to-back claims against a quiet writer: the protocol hands the
    // reader its recycled previous hold (the buffer it returned to `latest`
    // at claim 2 — seq 1 here). Nothing new was published, so nothing is
    // counted as dropped; the decreasing seq hits the reset rule (0).
    const third = cursor.claim(weft);
    expect(third.framesBehind).toBe(0);
    expect(third.seq).toBe(1);
    expect(cursor.totalDropped).toBe(8);
  });

  it('payload is the live held view, not a snapshot', () => {
    const weft = new Weft(16);
    const cursor = new FrameCursor();
    weft.publish(1, 16); // envelope seq = 1; payload stays the null-frame pat(0, i)
    const c = cursor.claim(weft);
    expect(c.seq).toBe(1);
    // The null-frame fixture fills payloads with pat(0, i) (04-LITMUS §0.6);
    // publish() itself only writes the envelope. Verify the differential
    // pattern is readable through the cursor's live view.
    const expected0 = pat(0, 0);
    expect(c.payload[0]).toBe(expected0);
  });

  it('treats a decreasing seq as a writer reset (no burst accounting)', () => {
    const weft = new Weft(64);
    const cursor = new FrameCursor();
    weft.publish(10, 64);
    cursor.claim(weft); // baseline: seq 10
    weft.publish(1, 64); // writer reset (new epoch semantics)
    const c = cursor.claim(weft);
    expect(c.seq).toBe(1);
    expect(c.framesBehind).toBe(0);
    expect(cursor.totalDropped).toBe(0);
  });
});
