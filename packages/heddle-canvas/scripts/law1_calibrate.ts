// law1_calibrate.ts — the Law-1 INSTRUMENT CALIBRATION (shard leg).
//
// WHY A SEPARATE SCRIPT: the calibration drives V8's sampling heap
// profiler, whose session is PROCESS-level — inside vitest's parallel
// worker threads the sessions collide (flaky), and calibration is a
// property of the INSTRUMENT, not of the battery. The shard runs this
// once, before the bench, to prove the gate can actually fail: an
// instrument that cannot fail proves nothing (the guardian "must-bite"
// discipline).
//
// Usage: node --experimental-strip-types scripts/law1_calibrate.ts
// Exit:  0 the instrument bites (clean loop passes, planted allocator caught)

import { samplingAllocationGate } from '../src/law1/audit.ts';
import { HotPlane } from '../src/plane/hot_plane.ts';
import { NullHAL } from '../src/hal/null_device.ts';
import { HeddleEngine } from '../src/loop/frame_engine.ts';
import { createDirtyBits, takeDirtyBits } from '../src/plane/dirty_mask.ts';
import { plantedAllocator } from '../src/renderers/planted_allocator.ts';
import { HP_KIND } from '../src/plane/whp1.ts';

const plane = HotPlane.create([{ kind: HP_KIND.WAVEFORM_F32, capacity: 1024, stride: 4, granularity: 16 }]);
const engine = new HeddleEngine(plane, new NullHAL(), {
  canvasWidth: 320, canvasHeight: 240, tickHz: 240, columnCount: 320,
});
const bits = createDirtyBits();
const lane = plane.lanes[0];
const sink: unknown[] = [];

const clean = await samplingAllocationGate(500, 4000, (i) => engine.tick(i * 4166));
console.log(`calibrate-clean:   ${clean.passed ? 'PASS' : 'FAIL'} — ${clean.engineBytes} B attributed to engine modules over 4000 frames`);

const planted = await samplingAllocationGate(500, 4000, (i) => {
  takeDirtyBits(plane.i32, lane, bits); // the clean engine call
  plantedAllocator(sink); // …and the planted allocator (src/renderers url)
  engine.tick(i * 4166);
});
console.log(`calibrate-planted: ${planted.passed ? 'FAIL (instrument did NOT catch the allocator!)' : 'PASS'} — ${planted.engineBytes} B caught across ${planted.engineSamples} samples`);

const ok = clean.passed && clean.engineBytes === 0 && !planted.passed && planted.engineBytes > 0;
console.log(`law1-instrument:   ${ok ? 'CALIBRATED' : 'BROKEN'}`);
process.exit(ok ? 0 : 1);
