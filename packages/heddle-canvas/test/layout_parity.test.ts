// layout_parity.test.ts — the cross-language contract gate. Parses
// native/whp1_layout.h and asserts every constant matches the TS side
// (whp1.ts). Two languages, one contract, one CI gate: a change on
// either side without the other is a FAILURE, not a silent drift.

import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import * as W from '../src/plane/whp1.ts';

const here = dirname(fileURLToPath(import.meta.url));
const header = readFileSync(join(here, '..', 'native', 'whp1_layout.h'), 'utf8');

function macro(name: string): string {
  // capture to end of line so (1u << 26)-style values parse whole
  const m = new RegExp(`#define\\s+${name}\\s+([^\\n]*)`).exec(header);
  if (m === null) throw new Error(`macro ${name} missing from whp1_layout.h`);
  return m[1].replace(/\/\*.*?\*\//g, '').trim();
}

function macroNum(name: string): number {
  const raw = macro(name);
  const shift = /^\(1u?\s*<<\s*(\d+)\)$/.exec(raw);
  if (shift !== null) return 2 ** Number(shift[1]);
  const n = Number(raw.replace(/u+$/, ''));
  if (Number.isNaN(n)) throw new Error(`macro ${name}: unparsable value ${raw}`);
  return n;
}

describe('WHP1 layout parity: native/whp1_layout.h == src/plane/whp1.ts', () => {
  it('plane identity + structure constants', () => {
    expect(macroNum('WHP1_MAGIC')).toBe(W.WHP1_MAGIC);
    expect(macroNum('WHP1_LANE_MAGIC')).toBe(W.WHP1_LANE_MAGIC);
    expect(macroNum('WHP1_VERSION')).toBe(W.WHP1_VERSION);
    expect(macroNum('WHP1_HEADER_BYTES')).toBe(W.WHP1_HEADER_BYTES);
    expect(macroNum('WHP1_MAX_LANES')).toBe(W.WHP1_MAX_LANES);
    expect(macroNum('WHP1_LANE_TABLE')).toBe(W.WHP1_LANE_TABLE);
    expect(macroNum('WHP1_LANE_STRIDE_TABLE')).toBe(W.WHP1_LANE_STRIDE_TABLE);
    expect(macroNum('WHP1_DATA_START')).toBe(W.WHP1_DATA_START);
    expect(macroNum('WHP1_MAX_LANE_CAPACITY')).toBe(W.WHP1_MAX_LANE_CAPACITY);
  });

  it('plane header word offsets', () => {
    expect(macroNum('WHP1_OFF_MAGIC')).toBe(W.WHP1_OFF_MAGIC);
    expect(macroNum('WHP1_OFF_VERSION')).toBe(W.WHP1_OFF_VERSION);
    expect(macroNum('WHP1_OFF_HEADER_BYTES')).toBe(W.WHP1_OFF_HEADER_BYTES);
    expect(macroNum('WHP1_OFF_LANE_COUNT')).toBe(W.WHP1_OFF_LANE_COUNT);
    expect(macroNum('WHP1_OFF_EPOCH')).toBe(W.WHP1_OFF_EPOCH);
    expect(macroNum('WHP1_OFF_PRODUCER_SEQ')).toBe(W.WHP1_OFF_PRODUCER_SEQ);
    expect(macroNum('WHP1_OFF_FLAGS')).toBe(W.WHP1_OFF_FLAGS);
    expect(macroNum('WHP1_OFF_DATA_START')).toBe(W.WHP1_OFF_DATA_START);
    expect(macroNum('WHP1_OFF_PLANE_BYTES')).toBe(W.WHP1_OFF_PLANE_BYTES);
  });

  it('lane descriptor word offsets (incl. the dirty word pair at +0x30)', () => {
    expect(macroNum('WHP1_LANE_OFF_MAGIC')).toBe(W.WHP1_LANE_OFF_MAGIC);
    expect(macroNum('WHP1_LANE_OFF_KIND')).toBe(W.WHP1_LANE_OFF_KIND);
    expect(macroNum('WHP1_LANE_OFF_DTYPE')).toBe(W.WHP1_LANE_OFF_DTYPE);
    expect(macroNum('WHP1_LANE_OFF_GRANULARITY')).toBe(W.WHP1_LANE_OFF_GRANULARITY);
    expect(macroNum('WHP1_LANE_OFF_OFFSET')).toBe(W.WHP1_LANE_OFF_OFFSET);
    expect(macroNum('WHP1_LANE_OFF_CAPACITY')).toBe(W.WHP1_LANE_OFF_CAPACITY);
    expect(macroNum('WHP1_LANE_OFF_STRIDE')).toBe(W.WHP1_LANE_OFF_STRIDE);
    expect(macroNum('WHP1_LANE_OFF_WRITE_POS')).toBe(W.WHP1_LANE_OFF_WRITE_POS);
    expect(macroNum('WHP1_LANE_OFF_SEQ')).toBe(W.WHP1_LANE_OFF_SEQ);
    expect(macroNum('WHP1_LANE_OFF_DIRTY_LO')).toBe(W.WHP1_LANE_OFF_DIRTY_LO);
    expect(macroNum('WHP1_LANE_OFF_DIRTY_HI')).toBe(W.WHP1_LANE_OFF_DIRTY_HI);
    expect(macroNum('WHP1_LANE_OFF_FLAGS')).toBe(W.WHP1_LANE_OFF_FLAGS);
  });

  it('lane kinds + strides (the render families)', () => {
    expect(macroNum('HP_KIND_WAVEFORM_F32')).toBe(W.HP_KIND.WAVEFORM_F32);
    expect(macroNum('HP_KIND_DEPTH_LADDER_F32')).toBe(W.HP_KIND.DEPTH_LADDER_F32);
    expect(macroNum('HP_KIND_CANDLE_OHLC_F32')).toBe(W.HP_KIND.CANDLE_OHLC_F32);
    expect(macroNum('HP_KIND_POINTCLOUD_QUAT_F32')).toBe(W.HP_KIND.POINTCLOUD_QUAT_F32);
    expect(macroNum('HP_DTYPE_F32')).toBe(W.HP_DTYPE.F32);
    // the C stride table row-by-row
    const cStrides = /\{ 4u, 64u, 64u, 128u \}/.test(header);
    expect(cStrides).toBe(true);
    expect(W.HP_KIND_STRIDE[W.HP_KIND.WAVEFORM_F32]).toBe(4);
    expect(W.HP_KIND_STRIDE[W.HP_KIND.DEPTH_LADDER_F32]).toBe(64);
    expect(W.HP_KIND_STRIDE[W.HP_KIND.CANDLE_OHLC_F32]).toBe(64);
    expect(W.HP_KIND_STRIDE[W.HP_KIND.POINTCLOUD_QUAT_F32]).toBe(128);
  });

  it('row word maps (ladder / candle / point cloud)', () => {
    expect(macroNum('HP_LADDER_W_PRICE')).toBe(W.HP_LADDER_W_PRICE);
    expect(macroNum('HP_LADDER_W_SIZE')).toBe(W.HP_LADDER_W_SIZE);
    expect(macroNum('HP_LADDER_W_SIDE')).toBe(W.HP_LADDER_W_SIDE);
    expect(macroNum('HP_CANDLE_W_OHLC')).toBe(W.HP_CANDLE_W_OHLC);
    expect(macroNum('HP_CANDLE_W_VOLUME')).toBe(W.HP_CANDLE_W_VOLUME);
    expect(macroNum('HP_PC_W_POSSIZE')).toBe(W.HP_PC_W_POSSIZE);
    expect(macroNum('HP_PC_W_QUAT')).toBe(W.HP_PC_W_QUAT);
    expect(macroNum('HP_PC_W_COLOR')).toBe(W.HP_PC_W_COLOR);
  });

  it('the C plane-size function agrees with whp1PlaneBytes (via clang eval)', async () => {
    // Compile-and-run the C helper against TS-computed sizes: the real
    // cross-language arithmetic check, not a string match.
    const { execFileSync } = await import('node:child_process');
    const specs = [
      { stride: 4, capacity: 65536 },
      { stride: 64, capacity: 256 },
      { stride: 128, capacity: 1024 },
      { stride: 4, capacity: 1 },
    ];
    const cSrc = `#include <stdio.h>
#include <stdint.h>
#include "whp1_layout.h"
int main(void) {
  uint32_t s[] = {${specs.map((x) => x.stride).join(',')}};
  uint32_t c[] = {${specs.map((x) => x.capacity).join(',')}};
  printf("%llu\\n", (unsigned long long)whp1_plane_bytes(s, c, ${specs.length}));
  return 0;
}`;
    const os = await import('node:os');
    const fs = await import('node:fs');
    const path = await import('node:path');
    const dir = fs.mkdtempSync(join(os.tmpdir(), 'whp1-parity-'));
    fs.writeFileSync(join(dir, 'main.c'), cSrc);
    fs.copyFileSync(join(here, '..', 'native', 'whp1_layout.h'), join(dir, 'whp1_layout.h'));
    execFileSync('cc', ['-O1', '-o', join(dir, 'a.out'), join(dir, 'main.c')]);
    const out = execFileSync(join(dir, 'a.out'), { encoding: 'utf8' }).trim();
    fs.rmSync(dir, { recursive: true, force: true });
    expect(Number(out)).toBe(W.whp1PlaneBytes(specs));
  });
});
