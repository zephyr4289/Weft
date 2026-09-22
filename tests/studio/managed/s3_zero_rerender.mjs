/**
 * Stage 3 — Zero-Re-Render Proof (Law 4).
 * Mounts the REAL WeftStudio component tree under the deterministic
 * mini-React shim (component recursion + per-cell hook ownership), binds
 * every canvas ref to a recording 2D context, then pumps a 10,000-frame
 * stream burst through the governed 240 Hz scheduler.
 *
 * Assertions:
 *   - every hot panel invoked EXACTLY ONCE (mount) across the whole burst
 *   - ZERO setState-initiated re-render mutations during the burst
 *   - the hot plane actually painted (fake ctx call count > frames/2)
 *   - unmount detaches every drawer
 */

import { spy, SPY } from '../../../packages/studio/src/engine/react-adapter.ts';
import { createStudioHarness, attachFakeCanvases, fakeCanvas } from './shim-react.mjs';
import { WeftStudio, CANONICAL_SCHEMA as CANONICAL_SRC } from '../../../packages/studio/src/ui/studio.tsx';
import { writeFileSync } from 'node:fs';

// browser globals shim (no DOM in the suite runtime)
globalThis.requestAnimationFrame = (fn) => 0;
globalThis.cancelAnimationFrame = () => {};

const results = { stage: 3, checks: [], ok: false };
let failures = 0;
function check(name, ok, detail = '') {
  results.checks.push({ name, ok, detail: String(detail).slice(0, 200) });
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

const harness = createStudioHarness();

// ---- mount sequence (shell → mounted → engine-attached) --------------------
let view = harness.renderRoot(WeftStudio);
harness.runEffects();
for (let i = 0; i < 8 && harness.rerenderPending(); i++) {
  view = harness.rerenderRoot(WeftStudio);
}

const rootCell = view.cell;
// useState slot layout in WeftStudio (declaration order):
//   0 mounted · 1 engine · 2 src · 3 tab · 4 running · 5 rate ...
const engine = rootCell.state[1] ?? null;

const rootCountAfterMount = spy.counts[SPY.STUDIO_ROOT];
check('root mounted and engine attached', rootCountAfterMount >= 2 && rootCountAfterMount <= 5,
  `root renders = ${rootCountAfterMount} (mount lifecycle: shell → mounted → engine)`);

// bind fake canvases so drawers can actually paint
const canvases = [];
const bound = attachFakeCanvases(view.tree, () => {
  const c = fakeCanvas();
  canvases.push(c);
  return c;
});
check('canvas refs bound to fake surfaces', bound >= 3, `bound = ${bound} (schema tab mounts 3: spy, status, telemetry)`);
check('engine attached through mount lifecycle', !!engine && typeof engine.runVirtual === 'function',
  engine ? `rate=${engine.sim.stats.nominalMsgPerSec}` : 'engine missing');

// ---- measurement window ----------------------------------------------------
const hotIds = [SPY.SCHEMA_DESIGNER, SPY.CACHE_MAPPER, SPY.RING_MONITOR, SPY.TIME_TRAVEL, SPY.TELEMETRY_HUD, SPY.RENDER_SPY_HUD, SPY.STATUS_BAR];
const mountedPanels = hotIds.filter((id) => spy.counts[id] > 0);
const baseline = hotIds.map((id) => spy.counts[id]);
const mutationsBefore = spy.mutations;

const FRAMES = 10_000;
const PERIOD = 1e9 / 240;

if (engine) {
  engine.sim.setRate(200_000); // keep the 10k-frame burst fast; frames are the unit under test
  const t0 = performance.now();
  const ran = engine.runVirtual(FRAMES * PERIOD);
  const wallMs = performance.now() - t0;

  const mutationsDuring = spy.mutations - mutationsBefore;
  const grown = hotIds.filter((id) => spy.counts[id] !== baseline[hotIds.indexOf(id)]);
  const paintCalls = canvases.reduce((a, c) => a + (c.__rec?.calls || 0), 0);

  check(`all ${FRAMES} frames executed`, ran === FRAMES, `ran = ${ran}`);
  check(`${mountedPanels.length} hot panels mounted under the tree`, mountedPanels.length >= 4,
    `mounted: ${mountedPanels.join(',')}`);
  check('hot components invoked exactly once', grown.length === 0,
    grown.length ? 'RE-RENDERED: ' + grown.join(',') : `counts stable at [${baseline.join(',')}]`);
  check('zero setState mutations during burst', mutationsDuring === 0, `mutations = ${mutationsDuring}`);
  check('hot plane painted', paintCalls > FRAMES / 2, `ctx calls = ${paintCalls}`);
  check('flight log recorded the burst', engine.flight.recordCount > 0, `${engine.flight.recordCount} records`);
  check('burst completed in reasonable wall time', wallMs < 60_000, `${wallMs.toFixed(0)} ms`);

  // unmount: every drawer must detach
  const drawersBefore = Object.values(engine.drawers).reduce((a, arr) => a + arr.length, 0);
  harness.unmount();
  const drawersAfter = Object.values(engine.drawers).reduce((a, arr) => a + arr.length, 0);
  check('unmount detaches all drawers', drawersBefore > 0 && drawersAfter === 0,
    `${drawersBefore} → ${drawersAfter}`);

  // ---- tab panels: mount Ring/Cache/TimeTravel as roots, burst again ----
  const { RingMonitorPanel } = await import('../../../packages/studio/src/ui/panels/RingMonitorPanel.tsx');
  const { CacheMapperPanel } = await import('../../../packages/studio/src/ui/panels/CacheMapperPanel.tsx');
  const { TimeTravelPanel } = await import('../../../packages/studio/src/ui/panels/TimeTravelPanel.tsx');
  const { parseSchema } = await import('../../../packages/studio/src/engine/schema.ts');
  const { computeLayout } = await import('../../../packages/studio/src/engine/layout.ts');
  const parsed = parseSchema(CANONICAL_SRC);
  const layout = computeLayout(parsed.doc ?? { structs: [], hash: '' });

  const tabViews = [
    harness.renderRoot(RingMonitorPanel, { engine }),
    harness.renderRoot(CacheMapperPanel, { layout, engine, lineSize: 64 }),
    harness.renderRoot(TimeTravelPanel, { engine }),
  ];
  const tabCanvases = [];
  for (const v of tabViews) {
    attachFakeCanvases(v.tree, () => {
      const c = fakeCanvas();
      tabCanvases.push(c);
      return c;
    });
  }
  harness.runEffects();

  const tabBaseline = [SPY.RING_MONITOR, SPY.CACHE_MAPPER, SPY.TIME_TRAVEL].map((id) => spy.counts[id]);
  const mutBefore2 = spy.mutations;
  engine.runVirtual(2_000 * PERIOD);
  const tabGrown = [SPY.RING_MONITOR, SPY.CACHE_MAPPER, SPY.TIME_TRAVEL].filter((id) => spy.counts[id] !== tabBaseline[[SPY.RING_MONITOR, SPY.CACHE_MAPPER, SPY.TIME_TRAVEL].indexOf(id)]);
  const tabPaint = tabCanvases.reduce((a, c) => a + (c.__rec?.calls || 0), 0);
  check('tab panels (Ring/Cache/TimeTravel) invoked exactly once over 2,000 more frames', tabGrown.length === 0 && tabCanvases.length >= 4,
    `canvases=${tabCanvases.length} grown=${tabGrown.length ? tabGrown.join(',') : 'none'} paints=${tabPaint}`);
  check('zero mutations in tab-panel burst', spy.mutations - mutBefore2 === 0, `${spy.mutations - mutBefore2}`);
}

results.ok = failures === 0;
writeFileSync(new URL('../../../evidence/pillar7/stage-3-zero-rerender.json', import.meta.url), JSON.stringify(results, null, 2));
console.log(results.ok ? 'STAGE 3: PASS' : `STAGE 3: FAIL (${failures})`);
process.exit(results.ok ? 0 : 2);
