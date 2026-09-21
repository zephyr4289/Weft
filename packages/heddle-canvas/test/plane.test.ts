// plane.test.ts — the WHP1 contract battery. Every PLANE_* refusal code in
// src/errors.ts fires exactly once here (Law 4: the ladder is MEASURED),
// the create→open roundtrip is proven, and the dirty-mask protocol
// semantics are pinned so Engineer 1's engine can substitute for
// SynthProducer without moving this file.

import { describe, expect, it } from 'vitest';
import {
  HP_KIND,
  WHP1_DATA_START,
  WHP1_HEADER_BYTES,
  WHP1_LANE_OFF_CAPACITY,
  WHP1_LANE_OFF_DIRTY_HI,
  WHP1_LANE_OFF_DIRTY_LO,
  WHP1_LANE_OFF_DTYPE,
  WHP1_LANE_OFF_GRANULARITY,
  WHP1_LANE_OFF_KIND,
  WHP1_LANE_OFF_MAGIC,
  WHP1_LANE_OFF_OFFSET,
  WHP1_LANE_OFF_STRIDE,
  WHP1_LANE_OFF_WRITE_POS,
  WHP1_LANE_TABLE,
  WHP1_LANE_STRIDE_TABLE,
  WHP1_MAGIC,
  WHP1_OFF_DATA_START,
  WHP1_OFF_EPOCH,
  WHP1_OFF_HEADER_BYTES,
  WHP1_OFF_LANE_COUNT,
  WHP1_OFF_MAGIC,
  WHP1_OFF_PLANE_BYTES,
  WHP1_OFF_VERSION,
} from '../src/plane/whp1.ts';
import { HotPlane } from '../src/plane/hot_plane.ts';
import { SynthProducer, mulberry32, synthSample } from '../src/plane/synth.ts';
import { HeddleError } from '../src/errors.ts';

const WAVE = { kind: HP_KIND.WAVEFORM_F32, capacity: 4096, stride: 4, granularity: 64 };
const CANDLE = { kind: HP_KIND.CANDLE_OHLC_F32, capacity: 256, stride: 64, granularity: 4 };
const LADDER = { kind: HP_KIND.DEPTH_LADDER_F32, capacity: 128, stride: 64, granularity: 2 };
const CLOUD = { kind: HP_KIND.POINTCLOUD_QUAT_F32, capacity: 512, stride: 128, granularity: 8 };

function expectCode(fn: () => void, code: string) {
  let caught: unknown;
  try {
    fn();
  } catch (e) {
    caught = e;
  }
  expect(caught).toBeInstanceOf(HeddleError);
  expect((caught as HeddleError).code).toBe(code);
}

function corrupt(plane: HotPlane, byteOffset: number, value: number) {
  new DataView(plane.sab).setUint32(byteOffset, value, true);
}

describe('WHP1 open() refusal ladder (every code fires)', () => {
  it('accepts a well-formed 4-lane plane (all kinds)', () => {
    const p = HotPlane.create([WAVE, CANDLE, LADDER, CLOUD]);
    expect(p.laneCount).toBe(4);
    expect(p.lanes[0].kind).toBe(HP_KIND.WAVEFORM_F32);
    expect(p.lanes[0].f32.length).toBe(4096);
    expect(p.lanes[1].strideBytes).toBe(64);
    expect(p.lanes[3].u32.length).toBe(512 * 32);
    expect(p.lanes[2].dirtyGranularity).toBe(2);
  });

  it('HC_E_NOT_SAB — plain ArrayBuffer is refused (cross-thread contract)', () => {
    expectCode(() => HotPlane.open(new ArrayBuffer(8192)), 'HC_E_NOT_SAB');
    expectCode(() => HotPlane.open(null), 'HC_E_NOT_SAB');
    expectCode(() => HotPlane.open({ byteLength: 9999 }), 'HC_E_NOT_SAB');
  });

  it('HC_E_PLANE_TOO_SMALL — below header + table + one lane', () => {
    expectCode(() => HotPlane.open(new SharedArrayBuffer(WHP1_DATA_START)), 'HC_E_PLANE_TOO_SMALL');
  });

  it('HC_E_PLANE_MAGIC', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_OFF_MAGIC, 0xdeadbeef);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_MAGIC');
  });

  it('HC_E_PLANE_VERSION — future versions refuse with a name', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_OFF_VERSION, 99);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_VERSION');
  });

  it('HC_E_PLANE_HEADER_BYTES — Law 2 header size is contract', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_OFF_HEADER_BYTES, 128);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_HEADER_BYTES');
  });

  it('HC_E_PLANE_HEADER_BYTES — data_start is the FIXED constant', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_OFF_DATA_START, WHP1_DATA_START + 128);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_HEADER_BYTES');
  });

  it('HC_E_PLANE_HEADER_BYTES — plane_bytes must equal buffer size', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_OFF_PLANE_BYTES, 12345);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_HEADER_BYTES');
  });

  it('HC_E_PLANE_LANE_COUNT — zero and 17 both refuse', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_OFF_LANE_COUNT, 0);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_LANE_COUNT');
    corrupt(p, WHP1_OFF_LANE_COUNT, 17);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_PLANE_LANE_COUNT');
  });

  it('HC_E_LANE_MAGIC', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_MAGIC, 0);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_MAGIC');
  });

  it('HC_E_LANE_KIND — unknown kind number', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_KIND, 42);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_KIND');
  });

  it('HC_E_LANE_KIND — dtype beyond F32 is a named refusal in v1', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_DTYPE, 2);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_KIND');
  });

  it('HC_E_LANE_STRIDE — kind/stride law (Law 2 word strides)', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_STRIDE, 8);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_STRIDE');
    const q = HotPlane.create([CANDLE]);
    corrupt(q, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_STRIDE, 32);
    expectCode(() => HotPlane.open(q.sab), 'HC_E_LANE_STRIDE');
  });

  it('HC_E_LANE_CAPACITY — zero refuses', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_CAPACITY, 0);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_CAPACITY');
  });

  it('HC_E_LANE_GRANULARITY — bits must cover the lane, and not dwarf it', () => {
    const p = HotPlane.create([WAVE]); // cap 4096 → gran in [64, 4096]
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_GRANULARITY, 32);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_GRANULARITY');
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_GRANULARITY, 8192);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_GRANULARITY');
  });

  it('HC_E_LANE_OFFSET — misaligned (Law 2 128-B granularity)', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_OFFSET, WHP1_DATA_START + 4);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_OFFSET');
  });

  it('HC_E_LANE_OFFSET — overlap with the previous lane refuses', () => {
    const p = HotPlane.create([WAVE, CANDLE]);
    const dv = new DataView(p.sab);
    const l1 = WHP1_LANE_TABLE + 1 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_OFFSET;
    const good = dv.getUint32(l1, true);
    dv.setUint32(l1, good - CANDLE.stride * CANDLE.capacity, true);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_OFFSET');
  });

  it('HC_E_LANE_OFFSET — past plane end refuses', () => {
    const p = HotPlane.create([WAVE]);
    corrupt(p, WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_OFFSET, WHP1_DATA_START + 4096 * 4);
    expectCode(() => HotPlane.open(p.sab), 'HC_E_LANE_OFFSET');
  });
});

describe('WHP1 writer protocol (SynthProducer = executable spec)', () => {
  it('create→open roundtrip yields the documented header', () => {
    const p = HotPlane.create([WAVE, CANDLE]);
    const dv = new DataView(p.sab);
    expect(dv.getUint32(WHP1_OFF_MAGIC, true)).toBe(0x314c5057); // "WPL1" LE
    expect(dv.getUint32(WHP1_OFF_VERSION, true)).toBe(1);
    expect(dv.getUint32(WHP1_OFF_HEADER_BYTES, true)).toBe(WHP1_HEADER_BYTES);
    expect(dv.getUint32(WHP1_OFF_LANE_COUNT, true)).toBe(2);
    // lane offsets are 128-B aligned and strictly increasing (Law 2)
    const o0 = dv.getUint32(WHP1_LANE_TABLE + WHP1_LANE_OFF_OFFSET, true);
    const o1 = dv.getUint32(WHP1_LANE_TABLE + WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_OFFSET, true);
    expect(o0 % 128).toBe(0);
    expect(o1 % 128).toBe(0);
    expect(o1).toBeGreaterThan(o0);
  });

  it('pumpWaveform writes samples, advances write_pos, raises + clears dirt', () => {
    const p = HotPlane.create([{ ...WAVE, capacity: 64, granularity: 1 }]);
    const synth = new SynthProducer(p);
    const i32 = p.i32;
    const lane = p.lanes[0];
    synth.pumpWaveform(10, 0.5);
    expect(Atomics.load(i32, lane.writePosI32)).toBe(10);
    const lo = Atomics.exchange(i32, lane.dirtyLoI32, 0) | 0;
    const hi = Atomics.exchange(i32, lane.dirtyHiI32, 0) | 0;
    expect(lo).not.toBe(0); // lane 0 bit raised (whole-range granularity 1 → 10 bits)
    expect(hi).toBe(0);
    // second harvest is clean — read-clear happened in the SAME op
    expect(Atomics.load(i32, lane.dirtyLoI32)).toBe(0);
    // samples are the deterministic waveform, f32-exact
    for (let i = 0; i < 10; i++) {
      expect(p.lanes[0].f32[i]).toBe(Math.fround(synthSample(i, 0.5)));
    }
  });

  it('wraps the ring without losing the waveform', () => {
    const p = HotPlane.create([{ ...WAVE, capacity: 64, granularity: 1 }]);
    const synth = new SynthProducer(p);
    synth.pumpWaveform(200, 1.25); // 3+ laps around a 64-slot ring
    expect(Atomics.load(p.i32, p.lanes[0].writePosI32)).toBe(200);
    for (let k = 0; k < 64; k++) {
      const globalIndex = 200 - 64 + k;
      const slot = globalIndex % 64;
      expect(p.lanes[0].f32[slot]).toBe(Math.fround(synthSample(globalIndex, 1.25)));
    }
  });

  it('row publishers write valid, side-consistent rows (ladder/candle/cloud)', () => {
    const p = HotPlane.create([{ ...LADDER, capacity: 16, granularity: 1 }, { ...CANDLE, capacity: 8, granularity: 1 }, { ...CLOUD, capacity: 32, granularity: 1 }]);
    const synth = new SynthProducer(p);
    synth.pumpLadder(16, 0.3);
    synth.pumpCandles(8, 0.3);
    synth.pumpPointcloud(32, 0.3);
    const i32 = p.i32;
    expect(Atomics.load(i32, p.lanes[0].writePosI32)).toBe(16);
    expect(Atomics.load(i32, p.lanes[1].writePosI32)).toBe(8);
    expect(Atomics.load(i32, p.lanes[2].writePosI32)).toBe(32);
    // every lane's dirty word raised then cleared
    for (let l = 0; l < 3; l++) {
      const lo = Atomics.exchange(i32, p.lanes[l].dirtyLoI32, 0) | 0;
      expect(lo).not.toBe(0);
    }
    // candle OHLC sanity: high >= max(open, close), low <= min(open, close)
    const c = p.lanes[1].f32;
    const words = 16;
    for (let r = 0; r < 8; r++) {
      const o = c[r * words], h = c[r * words + 1], low = c[r * words + 2], cl = c[r * words + 3];
      expect(h).toBeGreaterThanOrEqual(Math.max(o, cl));
      expect(low).toBeLessThanOrEqual(Math.min(o, cl));
    }
    // point cloud quaternions are unit-norm (IMU attitude contract)
    const q = p.lanes[2].f32;
    const qwords = 32;
    for (let pt = 0; pt < 32; pt++) {
      const x = q[pt * qwords + 4], y = q[pt * qwords + 5], z = q[pt * qwords + 6], w = q[pt * qwords + 7];
      expect(Math.hypot(x, y, z, w)).toBeCloseTo(1, 5);
    }
  });

  it('plane epoch advances with every publication (liveness heartbeat)', () => {
    const p = HotPlane.create([{ ...WAVE, capacity: 16, granularity: 1 }]);
    const synth = new SynthProducer(p);
    const e0 = Atomics.load(p.i32, WHP1_OFF_EPOCH / 4);
    synth.pumpWaveform(4, 0);
    synth.pumpWaveform(4, 0);
    expect(Atomics.load(p.i32, WHP1_OFF_EPOCH / 4) - e0).toBeGreaterThanOrEqual(2);
  });

  it('synthSample is f32-stable and clamped (cross-engine hash precondition)', () => {
    for (let i = 0; i < 5000; i++) {
      const v = synthSample(i, i * 0.001);
      expect(Number.isFinite(v)).toBe(true);
      expect(v).toBeGreaterThanOrEqual(-1.5);
      expect(v).toBeLessThanOrEqual(1.5);
    }
  });

  it('mulberry32 sequence is deterministic across instances', () => {
    const a = mulberry32(1234);
    const b = mulberry32(1234);
    for (let i = 0; i < 100; i++) expect(a()).toBe(b());
  });

  it('write_pos rides its documented descriptor word (parity with native probe)', () => {
    const p = HotPlane.create([{ ...WAVE, capacity: 64, granularity: 1 }]);
    const synth = new SynthProducer(p);
    synth.pumpWaveform(7, 0);
    const dv = new DataView(p.sab);
    const wpByte = WHP1_LANE_TABLE + 0 * WHP1_LANE_STRIDE_TABLE + WHP1_LANE_OFF_WRITE_POS;
    expect(dv.getUint32(wpByte, true)).toBe(7);
  });
});
