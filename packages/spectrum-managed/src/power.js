// src/power.js — Battery Status + Page Visibility integration (mandate A).
//
// Every listener writes PRIMITIVES into the caller-owned SHP1 flyweight —
// no state objects, no React state, no GC churn (Law 1 + Zero Re-Render Rule).
// detach() removes every added listener: zero leaky listeners is a tested
// contract, not a hope (Quality Bar).
//
// E_PROBE_UNAVAILABLE handling (Law 4): absent battery/visibility APIs leave
// the flyweight's UNKNOWN sentinels in place — the governor reads the
// sentinels and does nothing drastic.

import {
  BATTERY_UNKNOWN, CHARGING_UNKNOWN, VIS_UNKNOWN, VIS_VISIBLE, VIS_HIDDEN,
} from './wire.js';

// Cross-attach duplicate listener index (init-time bookkeeping — NOT the hot
// path). Keyed by state identity so two attachPowerSources() calls on the
// same flyweight detect their shared targets (E_LISTENER_LEAK guard).
const ATTACH_INDEX = new WeakMap(); // state -> Set("targetId|type")
let POWER_TARGET_SEQ = 0;
const targetId = (t) => {
  if (!t.__weftPowerId) t.__weftPowerId = ++POWER_TARGET_SEQ;
  return t.__weftPowerId;
};

export function attachPowerSources(state, navigatorLike = globalThis.navigator, doc = globalThis.document) {
  const listeners = [];   // { target, type, fn } — bookkeeping only (init path)
  let detached = false;
  const leaks = { count: 0 };

  const add = (target, type, fn) => {
    if (!target || typeof target.addEventListener !== 'function') return;
    // E_LISTENER_LEAK guard: same target+type already registered by ANY attach
    // on this state -> count once (dev surface, never the hot path)
    let key = null;
    const tid = targetId(target);
    key = tid + '|' + type;
    let set = ATTACH_INDEX.get(state);
    if (!set) { set = new Set(); ATTACH_INDEX.set(state, set); }
    if (set.has(key)) leaks.count++;
    set.add(key);
    target.addEventListener(type, fn, { passive: true });
    listeners.push({ target, type, fn, key });
  };

  // --- Battery Status API (Chrome/Edge/Android WebViews; optional everywhere)
  // getBattery() is a promise; attach is INIT-time, handler writes are O(1)
  // primitive stores into the flyweight.
  try {
    if (navigatorLike && typeof navigatorLike.getBattery === 'function') {
      Promise.resolve(navigatorLike.getBattery()).then((bat) => {
        if (detached || !bat) return;
        const onLevel = () => { state.batteryPermille = Math.round(bat.level * 1000); };
        const onCharging = () => { state.batteryCharging = bat.charging ? 1 : 0; };
        add(bat, 'levelchange', onLevel);
        add(bat, 'chargingchange', onCharging);
        onLevel();
        onCharging();
      }).catch(() => { /* battery API refused — sentinels stay (Law 4) */ });
    }
  } catch { /* no battery API — sentinels stay */ }

  // --- Page Visibility API (dynamic background power throttling)
  if (doc && typeof doc.hidden === 'boolean') {
    const onVis = () => { state.visibility = doc.hidden ? VIS_HIDDEN : VIS_VISIBLE; };
    add(doc, 'visibilitychange', onVis);
    onVis();
  }

  return {
    state,
    leaks,
    // Number of REAL listeners currently tracked (0 after detach — the zero
    // leaky listeners contract; tests assert this).
    liveCount() {
      let n = 0;
      for (let i = 0; i < listeners.length; i++) {
        const l = listeners[i];
        if (l.target !== null && l.target !== undefined) n++;
      }
      return n;
    },
    detach() {
      if (detached) return;
      detached = true;
      const set = ATTACH_INDEX.get(state);
      for (let i = 0; i < listeners.length; i++) {
        const l = listeners[i];
        if (l.target === null || l.target === undefined) continue;
        try { l.target.removeEventListener(l.type, l.fn); } catch { /* target gone */ }
        if (set && l.key) set.delete(l.key);
      }
      listeners.length = 0;
    },
  };
}

// Sentinel primer used by harnesses without DOM: fills UNKNOWN fields.
export function primeUnknown(state) {
  if (state.batteryPermille === undefined) state.batteryPermille = BATTERY_UNKNOWN;
  if (state.batteryCharging === undefined) state.batteryCharging = CHARGING_UNKNOWN;
  if (state.visibility === undefined) state.visibility = VIS_UNKNOWN;
  return state;
}

export { VIS_VISIBLE, VIS_HIDDEN };
