// shaders.test.ts — the shader drift gate + structural sanity.
//
// DRIFT GATE: the TS bundles are GENERATED from the canonical text files
// (scripts/emit_shaders.mjs). This test re-emits and asserts byte
// equality — a hand-edited bundle is a CI failure.
//
// STRUCTURAL: every shader family declares the entry points/uniforms the
// HALs feed. A rename in a shader without the binder (or vice versa) is
// a compile failure in the browser leg — this catches it in node first.

import { describe, expect, it } from 'vitest';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { WGSL_SOURCES } from '../src/hal/webgpu/wgsl_sources.ts';
import { GLSL_SOURCES } from '../src/hal/webgl2/glsl_sources.ts';

const here = dirname(fileURLToPath(import.meta.url));
const pkgRoot = join(here, '..');

describe('shader bundle drift gate', () => {
  it('bundles are byte-identical to a fresh emit from shaders/', () => {
    const before = new Map<string, string>([
      ['src/hal/webgpu/wgsl_sources.ts', readFileSync(join(pkgRoot, 'src/hal/webgpu/wgsl_sources.ts'), 'utf8')],
      ['src/hal/webgl2/glsl_sources.ts', readFileSync(join(pkgRoot, 'src/hal/webgl2/glsl_sources.ts'), 'utf8')],
    ]);
    execFileSync('node', ['scripts/emit_shaders.mjs'], { cwd: pkgRoot });
    for (const [file, was] of before) {
      const now = readFileSync(join(pkgRoot, file), 'utf8');
      expect(now, `${file} drifted from shaders/ — run node scripts/emit_shaders.mjs`).toBe(was);
    }
  });
});

describe('shader structural contracts (what the HALs feed)', () => {
  it('WGSL bundle: every family, entry points, and the shared param layout', () => {
    const families = Object.keys(WGSL_SOURCES);
    expect(families).toEqual(
      expect.arrayContaining(['oscilloDecimate', 'oscilloRibbon', 'depthLadder', 'candle', 'pointcloud']),
    );
    expect(WGSL_SOURCES.oscilloDecimate).toContain('@compute');
    expect(WGSL_SOURCES.oscilloDecimate).toContain('@workgroup_size(1)');
    // the reference window walk, verbatim in the shader
    expect(WGSL_SOURCES.oscilloDecimate).toContain('params.a.w + capacity - vis');
    for (const [k, src] of Object.entries(WGSL_SOURCES)) {
      if (k === 'oscilloDecimate') continue;
      expect(src, `${k}: vertex entry`).toContain('@vertex');
      expect(src, `${k}: fragment entry`).toContain('@fragment');
      expect(src, `${k}: vs_main`).toContain('fn vs_main');
      expect(src, `${k}: fs_main`).toContain('fn fs_main');
    }
    // row shaders honor the params-based instance cull (bundle replay)
    expect(WGSL_SOURCES.depthLadder, 'ladder instance cull').toContain('>= rowCount');
    expect(WGSL_SOURCES.candle, 'candle instance cull').toContain('>= rowCount');
    expect(WGSL_SOURCES.pointcloud, 'pointcloud instance cull').toContain('>= pointCount');
  });

  it('GLSL bundle: TF pass + every render family with the bound attrib names', () => {
    expect(GLSL_SOURCES.oscilloDecimateVert).toContain('#version 300 es');
    expect(GLSL_SOURCES.oscilloDecimateVert).toContain('out vec2 vMinMax');
    expect(GLSL_SOURCES.oscilloDecimateVert).toContain('uniform uint uWindowStart;');
    expect(GLSL_SOURCES.oscilloDecimateVert).toContain('uniform uint uCapacity;');
    // ribbon consumes aMinMax (TF output feeding the instanced draw)
    expect(GLSL_SOURCES.oscilloRibbonVert).toContain('in vec2 aMinMax');
    // ladder attribs: aPrice/aSize/aSide (divisor 1, plane stride)
    expect(GLSL_SOURCES.ladderVert).toContain('in float aPrice');
    expect(GLSL_SOURCES.ladderVert).toContain('in float aSize');
    expect(GLSL_SOURCES.ladderVert).toContain('in float aSide');
    // candle attribs: aOhlc vec4 + aVolume
    expect(GLSL_SOURCES.candleVert).toContain('in vec4 aOhlc');
    expect(GLSL_SOURCES.candleVert).toContain('in float aVolume');
    // pointcloud attribs: aPosSize/aQuat/aColor
    expect(GLSL_SOURCES.pointcloudVert).toContain('in vec4 aPosSize');
    expect(GLSL_SOURCES.pointcloudVert).toContain('in vec4 aQuat');
    expect(GLSL_SOURCES.pointcloudVert).toContain('in vec4 aColor');
  });

  it('all five WGSL files and ten GLSL files exist as canonical text', () => {
    const must = [
      'shaders/wgsl/oscillo_decimate.wgsl',
      'shaders/wgsl/oscillo_ribbon.wgsl',
      'shaders/wgsl/depth_ladder.wgsl',
      'shaders/wgsl/candles.wgsl',
      'shaders/wgsl/pointcloud.wgsl',
      'shaders/glsl/webgl2/oscillo_decimate_tf.vert',
      'shaders/glsl/webgl2/oscillo_ribbon.vert',
      'shaders/glsl/webgl2/depth_ladder.vert',
      'shaders/glsl/webgl2/candles.vert',
      'shaders/glsl/webgl2/pointcloud.vert',
      'shaders/vk/osc_decimate.comp',
      'shaders/msl/heddle_shaders.metal',
    ];
    for (const f of must) {
      expect(() => readFileSync(join(pkgRoot, f)), f).not.toThrow();
    }
  });
});
