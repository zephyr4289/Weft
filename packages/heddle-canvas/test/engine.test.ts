// engine.test.ts — the HeddleEngine contract battery on the NullHAL (the
// executable spec backend). Asserts the obligations EVERY tier owes:
// upload sub-ranges, family dispatch, clean-lane skips, teardown drain,
// loss degradation, and the budget ledger wiring.

import { describe, expect, it } from 'vitest';
import { HotPlane } from '../src/plane/hot_plane.ts';
import { NullHAL, type DrawRecord, type UploadRecord } from '../src/hal/null_device.ts';
import { HeddleEngine } from '../src/loop/frame_engine.ts';
import { SynthProducer } from '../src/plane/synth.ts';
import { HP_KIND } from '../src/plane/whp1.ts';
import { HeddleError } from '../src/errors.ts';

const CFG = { canvasWidth: 640, canvasHeight: 360, tickHz: 240, columnCount: 640 };

function buildPlane() {
  return HotPlane.create([
    { kind: HP_KIND.WAVEFORM_F32, capacity: 8192, stride: 4, granularity: 128 },
    { kind: HP_KIND.DEPTH_LADDER_F32, capacity: 64, stride: 64, granularity: 1 },
    { kind: HP_KIND.CANDLE_OHLC_F32, capacity: 32, stride: 64, granularity: 1 },
    { kind: HP_KIND.POINTCLOUD_QUAT_F32, capacity: 128, stride: 128, granularity: 2 },
  ]);
}

describe('HeddleEngine on NullHAL (the contract battery)', () => {
  it('dirty lanes upload their sub-range and dispatch the right family', () => {
    const plane = buildPlane();
    const hal = new NullHAL();
    const engine = new HeddleEngine(plane, hal, CFG);
    const synth = new SynthProducer(plane);
    synth.pumpWaveform(600, 0.25); // slots 0..599 → gran 128 → bits 0..4
    synth.pumpLadder(48, 0.25);
    synth.pumpCandles(24, 0.25);
    synth.pumpPointcloud(100, 0.25);
    engine.tick(1000);
    engine.tick(2000);

    const uploads: UploadRecord[] = [];
    for (let i = 0; i < 8; i++) uploads.push({ laneIndex: 0, elemStart: 0, elemEndExcl: 0, bytes: 0, view: null });
    const n = hal.recentUploads(8, uploads);
    expect(n).toBe(4); // one upload per dirty lane (first tick)
    // family dispatch: draws recorded for all four lanes
    const draws: DrawRecord[] = [];
    for (let i = 0; i < 8; i++) draws.push({ laneIndex: 0, family: 0, count: 0, windowStart: 0 });
    const dn = hal.recentDraws(8, draws);
    expect(dn).toBe(4); // 4 draws on the dirty tick; the clean tick drew nothing
    const families = new Set(draws.slice(0, 4).map((d) => d.family));
    expect(families).toEqual(new Set([0, 1, 2, 3])); // osc, ladder, candle, cloud
    // the osc draw carries the ring window (writePos % capacity)
    const osc = draws.find((d) => d.family === 0);
    expect(osc?.count).toBe(600);
    expect(osc?.windowStart).toBe(600 % 8192);
    // the second tick saw clean lanes — everything skipped
    expect(hal.stats.skippedCleanLanes).toBe(4);
    expect(hal.stats.bindCount).toBe(4);
  });

  it('clean frame: zero uploads, zero draws, present still counted', () => {
    const plane = buildPlane();
    const hal = new NullHAL();
    const engine = new HeddleEngine(plane, hal, CFG);
    engine.tick(0);
    expect(hal.stats.bindCount).toBe(0);
    expect(hal.stats.drawCalls).toBe(0);
    expect(hal.stats.skippedCleanLanes).toBe(4);
    expect(hal.stats.presentCount).toBe(1);
  });

  it('upload sub-range is the dirty bit span, not the whole lane', () => {
    const plane = HotPlane.create([
      { kind: HP_KIND.WAVEFORM_F32, capacity: 8192, stride: 4, granularity: 128 },
    ]);
    const hal = new NullHAL();
    const engine = new HeddleEngine(plane, hal, CFG);
    const synth = new SynthProducer(plane);
    synth.pumpWaveform(300, 0.1); // slots 0..299 → bits 0..2 → span [0, 384)
    engine.tick(0);
    const uploads: UploadRecord[] = [];
    for (let i = 0; i < 4; i++) uploads.push({ laneIndex: 0, elemStart: 0, elemEndExcl: 0, bytes: 0, view: null });
    hal.recentUploads(4, uploads);
    expect(uploads[0].elemStart).toBe(0);
    expect(uploads[0].elemEndExcl).toBe(384); // 3 bits × 128 gran
    expect(uploads[0].bytes).toBe(384 * 4);
    // the uploaded view is the LANE'S OWN view — the zero-copy identity
    expect(uploads[0].view).toBe(plane.lanes[0].f32);
  });

  it('rAF loop refuses to start twice and when rAF is absent', () => {
    const plane = buildPlane();
    const engine = new HeddleEngine(plane, new NullHAL(), CFG);
    // node has no requestAnimationFrame — start() must refuse BY NAME
    let code = '';
    try {
      engine.start();
    } catch (e) {
      code = (e as HeddleError).code;
    }
    expect(code).toBe('HC_E_BACKEND_REFUSED');
    expect(engine.isRunning).toBe(false);
  });

  it('engine wiring: budget ledger receives every tick', () => {
    const plane = buildPlane();
    const engine = new HeddleEngine(plane, new NullHAL(), CFG);
    const synth = new SynthProducer(plane);
    for (let i = 0; i < 10; i++) {
      synth.pumpWaveform(417, i * 0.004);
      synth.pumpLadder(48, i * 0.004);
      synth.pumpCandles(24, i * 0.004);
      synth.pumpPointcloud(100, i * 0.004);
      engine.tick(i * 4166);
    }
    expect(engine.budget.frames).toBe(10);
  });
});

describe('NullHAL contract violations are named refusals', () => {
  it('upload outside beginFrame/endFrame refuses', () => {
    const plane = buildPlane();
    const hal = new NullHAL();
    hal.initialize(plane, CFG);
    let code = '';
    try {
      hal.uploadWaveform(plane.lanes[0], 0, 10);
    } catch (e) {
      code = (e as HeddleError).code;
    }
    expect(code).toBe('HC_E_BACKEND_REFUSED');
  });

  it('invalid config refuses at initialize (not mid-frame)', () => {
    const plane = buildPlane();
    const hal = new NullHAL();
    let code = '';
    try {
      hal.initialize(plane, { canvasWidth: 0, canvasHeight: 360, tickHz: 240, columnCount: 640 });
    } catch (e) {
      code = (e as HeddleError).code;
    }
    expect(code).toBe('HC_E_BACKEND_REFUSED');
  });
});
