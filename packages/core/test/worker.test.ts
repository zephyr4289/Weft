/// Worker module tests (test/worker.test.ts)
///
/// WHY EXISTS: the worker module was rewritten to fix three defects — rAF
/// used inside workers (where it does not exist), ignored fpsCap/shared
/// options, and a promised-but-missing Transferable fallback. These tests
/// pin the fixes: worker-safe pacing, option validation, and the
/// latest-wins transfer channel semantics over real MessagePorts (Node's
/// node:worker_threads MessageChannel is browser-MessageChannel-compatible).
import { describe, it, expect, vi } from 'vitest';
import { MessageChannel } from 'node:worker_threads';
import {
  createFrameLoop,
  createWeftWorkerLoop,
  WeftTransferWriter,
  WeftTransferReader,
  createWeftTransferChannel,
  type WeftPort,
} from '../src/worker';
import { Weft } from '../src/index';

function nodePortAsWeftPort(port: {
  postMessage: (m: unknown, transfer?: unknown[]) => void;
  onmessage: ((ev: { data: unknown }) => void) | null;
}): WeftPort {
  return port as unknown as WeftPort;
}

describe('createFrameLoop', () => {
  it('paces ticks to fpsCap with an injected scheduler', async () => {
    let scheduled: Array<(now: number) => void> = [];
    const scheduler = (cb: (now: number) => void) => {
      scheduled.push(cb);
      return () => {
        scheduled = [];
      };
    };
    const now = { v: 0 };
    const ticks: number[] = [];
    const loop = createFrameLoop((t) => ticks.push(t), {
      fpsCap: 60, // minInterval ≈ 16.67 ms
      scheduler,
      now: (n) => now.v,
    });
    loop.start();
    expect(scheduled.length).toBe(1);

    // First tick at t=0 → fires.
    scheduled[0](0);
    expect(ticks.length).toBe(1);

    // Early tick at t=5 (< 16.67) → skipped.
    now.v = 5;
    scheduled[0](5);
    expect(ticks.length).toBe(1);

    // Later tick at t=20 (≥ 16.67) → fires.
    now.v = 20;
    scheduled[0](20);
    expect(ticks.length).toBe(2);

    loop.stop();
    expect(loop.isRunning()).toBe(false);
  });

  it('timer fallback drives ticks without rAF (worker reality)', async () => {
    // No global rAF stub installed → setTimeout heartbeat path.
    vi.stubGlobal('requestAnimationFrame', undefined);
    const ticks: number[] = [];
    const loop = createFrameLoop((t) => ticks.push(t), { fpsCap: 1000 });
    loop.start();
    await new Promise((r) => setTimeout(r, 30));
    loop.stop();
    await new Promise((r) => setTimeout(r, 10));
    const countAfterStop = ticks.length;
    expect(countAfterStop).toBeGreaterThan(0);
    await new Promise((r) => setTimeout(r, 20));
    expect(ticks.length).toBe(countAfterStop); // stopped cleanly
    vi.unstubAllGlobals();
  });
});

describe('createWeftWorkerLoop option validation', () => {
  function makeFakeCanvas(): OffscreenCanvas {
    const ctx2d = {} as OffscreenCanvasRenderingContext2D;
    return {
      getContext: (_: string) => ctx2d,
    } as unknown as OffscreenCanvas;
  }

  it('rejects shared:false with a pointer to the transfer channel (was silently ignored)', () => {
    const weft = new Weft(64);
    expect(() =>
      createWeftWorkerLoop(makeFakeCanvas(), weft, () => {}, { shared: false })
    ).toThrow(/WeftTransferChannel/);
  });

  it('rejects a non-SAB-backed weft', () => {
    const weft = new Weft(64);
    Object.defineProperty(weft, 'sab', { value: new ArrayBuffer(64) });
    expect(() => createWeftWorkerLoop(makeFakeCanvas(), weft, () => {})).toThrow(/SharedArrayBuffer/);
  });
});

describe('WeftTransferChannel — the Transferable fallback', () => {
  it('delivers the freshest frame and recycles buffers (zero steady-state alloc)', async () => {
    const ch = new MessageChannel();
    const writer = new WeftTransferWriter(nodePortAsWeftPort(ch.port2), 16, 2);
    const reader = new WeftTransferReader(nodePortAsWeftPort(ch.port1), writer);

    writer.publish((u8, seq) => {
      u8[0] = seq;
    });
    writer.publish((u8, seq) => {
      u8[0] = seq;
    });
    // Node ports deliver asynchronously — pump the event loop first.
    await new Promise((r) => setTimeout(r, 20));
    const drawn: Array<{ seq: number; first: number }> = [];
    reader.claimAndDraw((payload, seq) => drawn.push({ seq, first: payload[0] }));
    expect(drawn.length).toBe(1);
    // After recycle the writer can keep publishing — steady state never
    // needs a fresh allocation (Law 2).
    expect(writer.publish(() => {})).toBe('sent');
    expect(writer.publishes).toBe(3);
  });

  it('drops frames when the pool is momentarily empty — never back-pressures (I5)', () => {
    const writer = new WeftTransferWriter(
      { postMessage: () => {}, onmessage: null },
      8,
      1
    );
    expect(writer.publish(() => {})).toBe('sent');
    expect(writer.publish(() => {})).toBe('dropped'); // pool exhausted
  });

  it('latest-wins: older buffered frames are dropped with telemetry', async () => {
    const ch = new MessageChannel();
    const writer = new WeftTransferWriter(nodePortAsWeftPort(ch.port2), 16, 3);
    const reader = new WeftTransferReader(nodePortAsWeftPort(ch.port1), writer);

    for (let s = 1; s <= 3; s++) {
      writer.publish((u8) => {
        u8[0] = s;
      });
    }
    // Wait for all three messages to arrive; only the newest may remain.
    await new Promise((r) => setTimeout(r, 20));
    expect(reader.dropped).toBe(2);
    const seen: number[] = [];
    reader.claimAndDraw((payload, seq) => seen.push(seq, payload[0]));
    expect(seen).toEqual([3, 3]);
  });

  it('createWeftTransferChannel wires both halves over MessageChannel', async () => {
    const ch = new MessageChannel();
    const { writer, reader } = createWeftTransferChannel(
      nodePortAsWeftPort(ch.port1),
      nodePortAsWeftPort(ch.port2),
      32,
      3
    );
    writer.publish((u8, seq) => {
      u8.fill(seq);
    });
    await new Promise((r) => setTimeout(r, 20));
    expect(reader.peekSeq()).toBe(1);
    let ok = false;
    reader.claimAndDraw((payload, seq) => {
      ok = payload[7] === seq;
    });
    expect(ok).toBe(true);
  });
});
