// test/power.test.mjs — Battery/Visibility wiring: primitive-only writes,
// zero leaky listeners, UNKNOWN sentinels when APIs are absent.
import test from 'node:test';
import assert from 'node:assert/strict';
import { attachPowerSources, primeUnknown } from '../src/power.js';
import { makeProfileFlyweight, BATTERY_UNKNOWN, CHARGING_UNKNOWN, VIS_UNKNOWN, VIS_VISIBLE, VIS_HIDDEN } from '../src/wire.js';

function makeEventTarget() {
  const ls = new Map();
  return {
    _emit(type) { for (const fn of ls.get(type) || []) fn(); },
    addEventListener(type, fn) {
      if (!ls.has(type)) ls.set(type, new Set());
      ls.get(type).add(fn);
    },
    removeEventListener(type, fn) { const s = ls.get(type); if (s) s.delete(fn); },
    _count(type) { return (ls.get(type) || new Set()).size; },
  };
}

test('visibilitychange writes the primitive into the flyweight', () => {
  const st = makeProfileFlyweight();
  const doc = { hidden: false };
  Object.assign(doc, makeEventTarget());
  const h = attachPowerSources(st, undefined, doc);
  assert.equal(st.visibility, VIS_VISIBLE);
  doc.hidden = true;
  doc._emit('visibilitychange');
  assert.equal(st.visibility, VIS_HIDDEN);
  doc.hidden = false;
  doc._emit('visibilitychange');
  assert.equal(st.visibility, VIS_VISIBLE);
  h.detach();
});

test('battery listeners update permille/charging primitives (async attach)', async () => {
  const st = makeProfileFlyweight();
  const bat = Object.assign({ level: 0.42, charging: false }, makeEventTarget());
  const nav = { getBattery: () => Promise.resolve(bat) };
  const h = attachPowerSources(st, nav, undefined);
  await Promise.resolve(); await Promise.resolve(); // flush microtasks
  assert.equal(st.batteryPermille, 420);
  assert.equal(st.batteryCharging, 0);
  bat.level = 0.17; bat._emit('levelchange');
  bat.charging = true; bat._emit('chargingchange');
  assert.equal(st.batteryPermille, 170);
  assert.equal(st.batteryCharging, 1);
  assert.equal(h.liveCount(), 2);
  h.detach();
  assert.equal(h.liveCount(), 0, 'zero leaky listeners after detach');
  bat.level = 0.99; bat._emit('levelchange');
  assert.equal(st.batteryPermille, 170, 'detached handler does not fire');
});

test('absent APIs keep UNKNOWN sentinels (Law 4, no crash)', () => {
  const st = makeProfileFlyweight();
  const h = attachPowerSources(st, undefined, undefined);
  assert.equal(st.batteryPermille, BATTERY_UNKNOWN);
  assert.equal(st.batteryCharging, CHARGING_UNKNOWN);
  assert.equal(st.visibility, VIS_UNKNOWN);
  h.detach();
});

test('double attach on the same doc counts a leak (dev-surface, no hot path)', () => {
  const st = makeProfileFlyweight();
  const doc = Object.assign({ hidden: false }, makeEventTarget());
  const a = attachPowerSources(st, undefined, doc);
  const b = attachPowerSources(st, undefined, doc);
  assert.equal(b.leaks.count, 1, 'same target+type twice -> leak counter bites');
  a.detach(); b.detach();
  assert.equal(doc._count('visibilitychange'), 0);
});

test('primeUnknown fills only undefined fields', () => {
  const st = { batteryPermille: 500 };
  primeUnknown(st);
  assert.equal(st.batteryPermille, 500);
  assert.equal(st.batteryCharging, CHARGING_UNKNOWN);
  assert.equal(st.visibility, VIS_UNKNOWN);
});
