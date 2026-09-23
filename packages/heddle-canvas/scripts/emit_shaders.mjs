#!/usr/bin/env node
// emit_shaders.mjs — generate the TS shader bundles from the CANONICAL
// shader text files (shaders/wgsl, shaders/glsl/webgl2, shaders/vk,
// shaders/msl).
//
// WHY GENERATED: the published package must be self-contained (the HALs
// compile shader STRINGS — no fs, no fetch), while the repo's reviewable
// artifacts and the native builds need plain text files. One of the two
// had to be generated; the text files are canonical because the Vulkan
// probe compiles them directly and reviewers diff them. The drift gate
// (test/shaders.test.ts) re-runs this script and asserts byte equality —
// editing a bundle by hand is a CI failure, not a silent divergence.
//
// Usage: node scripts/emit_shaders.mjs   (from the package root)

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const read = (p) => readFileSync(join(root, p), 'utf8');
const esc = (s) => s.replace(/\\/g, '\\\\').replace(/`/g, '\\`').replace(/\$\{/g, '\\${');

const WGSL = {
  oscilloDecimate: 'shaders/wgsl/oscillo_decimate.wgsl',
  oscilloRibbon: 'shaders/wgsl/oscillo_ribbon.wgsl',
  depthLadder: 'shaders/wgsl/depth_ladder.wgsl',
  candle: 'shaders/wgsl/candles.wgsl',
  pointcloud: 'shaders/wgsl/pointcloud.wgsl',
};

const GLSL = {
  oscilloDecimateVert: 'shaders/glsl/webgl2/oscillo_decimate_tf.vert',
  oscilloDecimateFrag: 'shaders/glsl/webgl2/oscillo_pass.frag',
  oscilloRibbonVert: 'shaders/glsl/webgl2/oscillo_ribbon.vert',
  oscilloRibbonFrag: 'shaders/glsl/webgl2/oscillo_ribbon.frag',
  ladderVert: 'shaders/glsl/webgl2/depth_ladder.vert',
  ladderFrag: 'shaders/glsl/webgl2/depth_ladder.frag',
  candleVert: 'shaders/glsl/webgl2/candles.vert',
  candleFrag: 'shaders/glsl/webgl2/candles.frag',
  pointcloudVert: 'shaders/glsl/webgl2/pointcloud.vert',
  pointcloudFrag: 'shaders/glsl/webgl2/pointcloud.frag',
};

const banner = (title, origin) => `// ${title}
//
// GENERATED from ${origin} by scripts/emit_shaders.mjs — DO NOT EDIT HERE.
// The drift gate (test/shaders.test.ts) re-emits and diffs; edit the
// canonical file instead.
`;

let wgslOut = banner('wgsl_sources.ts — the WGSL bundle (Tier 1).', 'shaders/wgsl/*.wgsl');
wgslOut += '\nexport const WGSL_SOURCES = {\n';
for (const [k, p] of Object.entries(WGSL)) {
  wgslOut += `  ${k}: \`${esc(read(p))}\`,\n`;
}
wgslOut += '} as const;\n';
writeFileSync(join(root, 'src/hal/webgpu/wgsl_sources.ts'), wgslOut);

let glslOut = banner('glsl_sources.ts — the GLSL 300 es bundle (Tier 2).', 'shaders/glsl/webgl2/*');
glslOut += '\nexport const GLSL_SOURCES = {\n';
for (const [k, p] of Object.entries(GLSL)) {
  glslOut += `  ${k}: \`${esc(read(p))}\`,\n`;
}
glslOut += '} as const;\n';
writeFileSync(join(root, 'src/hal/webgl2/glsl_sources.ts'), glslOut);

console.log('emitted: src/hal/webgpu/wgsl_sources.ts, src/hal/webgl2/glsl_sources.ts');
