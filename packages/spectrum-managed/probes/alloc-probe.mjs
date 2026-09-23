// probes/alloc-probe.mjs — Law 1 heap probe (Pillar 5, mandate F).
//
//   node --expose-gc probes/alloc-probe.mjs telemetry   # 1,000,000 metric
//                                                        # cycles, gate <= 64 KiB
//   node --expose-gc probes/alloc-probe.mjs control     # negative control:
//                                                        # per-cycle allocation
//                                                        # MUST bite (exit 2)
//
// The telemetry loop is the exact steady-state path the SDK prescribes:
// ProfileView.validate() + primitive getters + cadenceTick() on preallocated
// flyweights. No object literals, no arrays, no strings, no closures.
// Exit code: 0 within gate, 1 over gate, 2 = control bit (expected for the
// control mode — proves the probe is sensitive).

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { decodeProfile, makeProfileFlyweight } from '../src/wire.js';
import { createCadenceState, cadenceTick } from '../src/governor.js';

const mode = process.argv[2] || 'telemetry';
const GATE_BYTES = mode === 'telemetry' ? 64 * 1024 : 1;
const CYCLES = mode === 'telemetry' ? 1_000_000 : 100_000;
const FIX = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'tests', 'spectrum', 'managed', 'fixtures');

if (typeof globalThis.gc !== 'function') {
  console.error('E_NO_GC: run with --expose-gc');
  process.exit(1);
}

const { view } = decodeProfile(readFileSync(join(FIX, 'hw-profile-flagship.bin')));
const st = makeProfileFlyweight();
view.snapshotInto(st);
const gov = createCadenceState();
const inp = {
  thermalState: 0, batteryPermille: 900, batteryCharging: 1,
  visibility: 0, heapPressure: 0, tierMaxHz: 240,
};

function cycle(i) {
  // one full telemetry cycle: integrity gate + in-place profile read +
  // cadence decision + battery/visibility primitive read
  const code = view.validate();
  if (code !== 0) throw new Error('profile corrupt at cycle ' + i);
  inp.thermalState = i % 5;
  st.thermalState = inp.thermalState;
  st.batteryPermille = 1000 - (i % 1000);
  st.visibility = (i % 600) < 540 ? 0 : 1;
  inp.batteryPermille = st.batteryPermille;
  inp.visibility = st.visibility;
  cadenceTick(gov, inp);
  return gov.capHz + st.thermalState + code; // consume, defeat DCE
}

// warmup — stablizes V8 tiers and lets bootstrap garbage age out
let sink = 0;
for (let i = 0; i < 50_000; i++) sink ^= cycle(i);
globalThis.gc();

// Control retains every per-cycle object: ~100k live objects => multi-MB
// heap growth. Escape analysis cannot eat this (the array escapes).
const controlSink = [];

const heapStart = process.memoryUsage().heapUsed;
for (let i = 0; i < CYCLES; i++) {
  sink ^= cycle(i);
  if (mode === 'control') controlSink.push({ seq: i, cap: gov.capHz }); // allocates + retained
}
globalThis.gc();
if (mode === 'control') sink ^= controlSink.length; // retain to the end
const heapEnd = process.memoryUsage().heapUsed;
const growth = heapEnd - heapStart;

const result = {
  mode,
  cycles: CYCLES,
  heapStart,
  heapEnd,
  growthBytes: growth,
  gateBytes: GATE_BYTES,
  sink: sink | 0,
  ok: growth <= GATE_BYTES,
};

console.log(JSON.stringify(result));
if (mode === 'control') {
  // control MUST bite: growth above the 1-byte gate proves probe sensitivity
  process.exit(result.ok ? 3 : 2);
}
process.exit(result.ok ? 0 : 1);
