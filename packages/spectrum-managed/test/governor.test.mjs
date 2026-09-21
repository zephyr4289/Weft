// test/governor.test.mjs — cadence governor vs the FROZEN 130-tick vector
// (cross-language parity source of truth) + tier staging + edge matrix.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createCadenceState, cadenceTick, tierTick, effectiveBudgetBytes,
  CADENCE_LADDER, SUSTAINED_TICKS, RECOVERY_TICKS, BACKGROUND_CAP,
  LOW_BATTERY_PERMILLE, MAX_TIER_STAGES,
} from '../src/governor.js';
import { E_TIER_EXHAUSTED, E_HEAP_PRESSURE } from '../src/wire.js';

const FIX = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'tests', 'spectrum', 'managed', 'fixtures');
const vec = JSON.parse(readFileSync(join(FIX, 'governor_vector.json'), 'utf8'));
const tierVec = JSON.parse(readFileSync(join(FIX, 'tier_vector.json'), 'utf8'));

test('constants match the normative table (§4.1)', () => {
  assert.deepEqual([...CADENCE_LADDER], vec.constants.CADENCE_LADDER);
  assert.equal(SUSTAINED_TICKS, vec.constants.SUSTAINED_TICKS);
  assert.equal(RECOVERY_TICKS, vec.constants.RECOVERY_TICKS);
  assert.equal(BACKGROUND_CAP, vec.constants.BACKGROUND_CAP);
  assert.equal(LOW_BATTERY_PERMILLE, vec.constants.LOW_BATTERY_PERMILLE);
  assert.equal(MAX_TIER_STAGES, vec.constants.MAX_TIER_STAGES);
});

test('FROZEN 130-tick vector: identical cap/rung/tierStage sequence', () => {
  const st = createCadenceState();
  const inp = {
    thermalState: 0, batteryPermille: 900, batteryCharging: 1,
    visibility: 0, heapPressure: 0, tierMaxHz: vec.profileMaxHz,
  };
  for (let i = 0; i < vec.ticks.length; i++) {
    const t = vec.ticks[i];
    inp.thermalState = t.thermal;
    inp.batteryPermille = t.batteryPermille;
    inp.batteryCharging = t.batteryCharging;
    inp.visibility = t.visibility;
    inp.heapPressure = t.heapPressure;
    cadenceTick(st, inp);
    const e = vec.expected[i];
    assert.equal(st.capHz, e.cap, `tick ${i} cap`);
    assert.equal(st.rung, e.rung, `tick ${i} rung`);
    assert.equal(st.tierStage, e.tierStage, `tick ${i} tierStage`);
  }
  // transcript-level: transitions happen exactly where the vector froze them
  const seen = [vec.expected[0].cap];
  for (let i = 1; i < vec.expected.length; i++) {
    if (vec.expected[i].cap !== vec.expected[i - 1].cap) seen.push(vec.expected[i].cap);
  }
  assert.deepEqual(seen, vec.expectedCapTransitions);
});

test('cadenceTick returns a primitive and mutates the flyweight in place', () => {
  const st = createCadenceState();
  const ret = cadenceTick(st, {
    thermalState: 0, batteryPermille: 900, batteryCharging: 1,
    visibility: 0, heapPressure: 0,
  });
  assert.equal(ret, 240);
  assert.equal(st.capHz, 240);
});

test('ladder clamps at the bottom rung under endless severe heat', () => {
  const st = createCadenceState();
  const inp = { thermalState: 3, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0 };
  let last = 0;
  for (let i = 0; i < 200; i++) last = cadenceTick(st, inp);
  assert.equal(last, 30);
  assert.equal(st.rung, CADENCE_LADDER.length - 1);
});

test('recovery never exceeds rung 0 and re-accumulates the cool window', () => {
  const st = createCadenceState();
  const hot = { thermalState: 3, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0 };
  const cool = { thermalState: 0, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0 };
  for (let i = 0; i < 100; i++) cadenceTick(st, hot); // pinned at 30
  for (let i = 0; i < RECOVERY_TICKS; i++) cadenceTick(st, cool); // one step up
  assert.equal(st.rung, 2);
  for (let i = 0; i < RECOVERY_TICKS; i++) cadenceTick(st, cool);
  assert.equal(st.rung, 1);
  for (let i = 0; i < RECOVERY_TICKS; i++) cadenceTick(st, cool);
  assert.equal(st.rung, 0);
  for (let i = 0; i < 500; i++) cadenceTick(st, cool); // cannot go above 240
  assert.equal(st.capHz, 240);
});

test('hidden visibility freezes streaks (no ladder movement while backgrounded)', () => {
  const st = createCadenceState();
  const hidden = { thermalState: 3, batteryPermille: 100, batteryCharging: 0, visibility: 1, heapPressure: 0 };
  for (let i = 0; i < 50; i++) cadenceTick(st, hidden);
  assert.equal(st.capHz, BACKGROUND_CAP); // rule 1 wins over battery + thermal
  assert.equal(st.rung, 0, 'no rung movement while hidden');
  // severe streak must NOT have accumulated: 10 visible severe ticks step down
  const visibleSevere = { thermalState: 3, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0 };
  for (let i = 0; i < SUSTAINED_TICKS - 1; i++) cadenceTick(st, visibleSevere);
  assert.equal(st.rung, 0, 'streaks were reset by hidden ticks');
  cadenceTick(st, visibleSevere);
  assert.equal(st.rung, 1, 'full visible severe window steps down');
});

test('tier ceiling: a down-tiered T3 device never runs above its profile max', () => {
  const st = createCadenceState();
  const inp = { thermalState: 0, batteryPermille: 900, batteryCharging: 1, visibility: 0, heapPressure: 0, tierMaxHz: 60 };
  cadenceTick(st, inp);
  assert.equal(st.capHz, 60);
});

test('tier staging vector (rule 5): 3 pressures -> stage 2, budget halves, hold', () => {
  const st = createCadenceState();
  const inp = { heapPressure: 0, profileBudgetBytes: 8589934592 };
  // pressure while already at MAX stages -> E_TIER_EXHAUSTED inside the loop
  // (the vector's expected[2] holds stage 2); fresh pressure at stage<max -> E_HEAP_PRESSURE
  for (let i = 0; i < tierVec.ticks.length; i++) {
    inp.heapPressure = tierVec.ticks[i].heapPressure;
    const prevStage = st.tierStage;
    const code = tierTick(st, inp);
    const e = tierVec.expected[i];
    assert.equal(st.tierStage, e.tierStage, `tick ${i} stage`);
    assert.equal(st.effTier, e.effectiveTier, `tick ${i} effTier`);
    assert.equal(st.budgetBytes, e.budgetBytes, `tick ${i} budget`);
    if (tierVec.ticks[i].heapPressure === 1) {
      assert.equal(code, prevStage >= MAX_TIER_STAGES ? E_TIER_EXHAUSTED : E_HEAP_PRESSURE, `tick ${i} code`);
    } else {
      assert.equal(code, 0, `tick ${i} idle code`);
    }
  }
  // pressure while exhausted -> E_TIER_EXHAUSTED, stage holds (Law 4: keep serving)
  inp.heapPressure = 1;
  assert.equal(tierTick(st, inp), E_TIER_EXHAUSTED);
  assert.equal(st.tierStage, MAX_TIER_STAGES);
});

test('effectiveBudgetBytes pure integer halving', () => {
  assert.equal(effectiveBudgetBytes(1024, 0), 1024);
  assert.equal(effectiveBudgetBytes(1024, 1), 512);
  assert.equal(effectiveBudgetBytes(1024, 2), 256);
  assert.equal(effectiveBudgetBytes(1023, 1), 511);
});
