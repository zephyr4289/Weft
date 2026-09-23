// test/hud.test.mjs — <WeftSpectrumHud />: ZERO re-renders under telemetry,
// fail-safe banner + recovery, clean unmount.
import test from 'node:test';
import assert from 'node:assert/strict';
import { makeShim, makeDoc, makeHost } from './shim.mjs';
import { createWeftSpectrumHud, buildSpectrumHudUi, paintSpectrumHud } from '../src/react.js';
import { makeProfileFlyweight, createCadenceState, cadenceTick, E_HUD_CONTEXT_LOST } from '../src/index.js';

function makeMetrics() {
  return { fps: 240, jitterP99Us: 23, jitterP999Us: 41, memBytes: 96 * 1048576 };
}

test('paint writes tier/features/thermal/cadence from the flyweights', () => {
  const st = makeProfileFlyweight();
  st.siliconTier = 1; st.thermalState = 0; st.featureFlagsLo = (1 << 7) | (1 << 0); // NEON + SIMD128
  st.batteryPermille = 870; st.visibility = 0;
  const gov = createCadenceState(); gov.capHz = 240;
  const ui = buildSpectrumHudUi(makeHost(), makeDoc());
  const rows = ui.rows;
  paintSpectrumHud(ui, { state: st, gov, metrics: makeMetrics() });
  assert.equal(rows[0].textContent, 'T1 FLAGSHIP');
  assert.ok(rows[1].textContent.includes('NEON'));
  assert.equal(rows[2].textContent, '240');
  assert.equal(rows[3].textContent, '23/41us');
  assert.equal(rows[5].textContent, 'nominal');
  assert.equal(rows[6].textContent, '240 Hz');
  assert.ok(rows[7].textContent.includes('87%'));
});

test('ZERO re-render: 10,000 telemetry updates, 0 setState, single render', () => {
  const shim = makeShim();
  const WeftSpectrumHud = createWeftSpectrumHud(shim.React);
  const st = makeProfileFlyweight();
  const gov = createCadenceState();
  const metrics = makeMetrics();
  const el = WeftSpectrumHud({ state: st, gov, metrics, doc: makeDoc() });
  assert.equal(shim.effects.length, 1, 'exactly one useEffect (single render)');
  shim.mount(); // runs effect -> builds UI + starts its own timer
  shim.unmount(); // stops the timer; paints below are driven manually

  // 10k telemetry mutations through the DIRECT micro-DOM channel —
  // the exact path the HUD timer uses in production.
  const ui = buildSpectrumHudUi(makeHost(), makeDoc());
  const props = { state: st, gov, metrics };
  paintSpectrumHud(ui, props);
  for (let i = 0; i < 10_000; i++) {
    st.thermalState = i % 5;
    metrics.fps = 240 - (i % 61);
    metrics.jitterP99Us = 10 + (i % 90);
    gov.capHz = cadenceTick(gov, {
      thermalState: st.thermalState, batteryPermille: 900, batteryCharging: 1,
      visibility: 0, heapPressure: 0,
    });
    paintSpectrumHud(ui, props);
  }
  assert.equal(shim.setStateCalls.length, 0, 'ZERO setState across 10k updates');
  assert.ok(el, 'component returned an element');
});

test('fail-safe: destroyed row -> FALLBACK banner, not a throw; heal clears it', () => {
  const st = makeProfileFlyweight(); st.siliconTier = 2;
  const ui = buildSpectrumHudUi(makeHost(), makeDoc());
  const props = { state: st, gov: null, metrics: null };
  assert.equal(paintSpectrumHud(ui, props), 0);

  // simulate context loss: status row destroyed out from under the painter
  ui.rows[7] = null;
  const code = paintSpectrumHud(ui, { state: st, gov: null, metrics: null });
  assert.equal(code, E_HUD_CONTEXT_LOST);
  assert.equal(ui.healthy, false);
  assert.ok(ui.banner.textContent.includes('FALLBACK'));
  assert.equal(ui.banner.style.display, 'block');

  // heal: healthy row restored -> banner clears (E_HUD_RECOVERED path)
  ui.rows[7] = { textContent: '', style: {} };
  assert.equal(paintSpectrumHud(ui, props), 0);
  assert.equal(ui.healthy, true);
  assert.equal(ui.banner.style.display, 'none');
});

test('paint survives a totally hostile state object (never throws into host)', () => {
  const ui = buildSpectrumHudUi(makeHost(), makeDoc());
  const hostile = {}; // missing every field -> featureFlagsLo read throws? (undefined & 3 === 0, OK)
  assert.doesNotThrow(() => paintSpectrumHud(ui, { state: hostile, gov: null, metrics: null }));
  // genuinely hostile getter
  const boom = { get siliconTier() { throw new Error('device gone'); } };
  assert.doesNotThrow(() => paintSpectrumHud(ui, { state: boom, gov: null, metrics: null }));
  assert.equal(ui.healthy, false, 'banner engaged');
});

test('mounted HUD cleans its interval on unmount (zero leaky listeners/timers)', () => {
  const shim = makeShim();
  const WeftSpectrumHud = createWeftSpectrumHud(shim.React);
  const st = makeProfileFlyweight();
  WeftSpectrumHud({ state: st, doc: makeDoc() });
  shim.mount();
  assert.equal(typeof shim.effects[0].cleanup, 'function');
  assert.doesNotThrow(() => shim.unmount());
  shim.unmount(); // double-unmount safe
});
