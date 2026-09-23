// golden.test.mjs — byte-exact C <-> JavaScript interop proof.
//
// The C engine (core/c/heddle/golden_dump.c) generated golden.bin +
// golden.json deterministically. This test copies the image into a
// SharedArrayBuffer, attaches the JS engine, and verifies EVERY field:
// the validation ladder inputs (magic, version, canary, CRC), every
// volatile register, every lane's stats and bounding box, and every
// cell's 16 payload bytes against the C-recorded hex. Then it exercises
// the signal plane (harvest/reset/remark/frame) through Atomics.
//
// Run: node test/golden.test.mjs   (from packages/heddle-hotplane)

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { HotPlane, MAGIC, OK, E_SEQ_TORN,
         MODE_STATE, ERR_NAMES } from '../src/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const goldenBin = join(here, '../../../core/c/heddle/golden/golden.bin');
const goldenJson = join(here, '../../../core/c/heddle/golden/golden.json');

let checks = 0;
let failures = 0;
function check(cond, name) {
  checks++;
  if (!cond) {
    failures++;
    console.error(`FAIL ${name}`);
  } else {
    console.log(`PASS ${name}`);
  }
}

const image = readFileSync(goldenBin);
const golden = JSON.parse(readFileSync(goldenJson, 'utf8'));

// The plane must live in a SharedArrayBuffer for Atomics.
const sab = new SharedArrayBuffer(image.byteLength);
new Uint8Array(sab).set(image);
const plane = new HotPlane(sab);

// ---- identity + geometry -------------------------------------------------
check(plane.mode === golden.mode && plane.mode === MODE_STATE,
      'J1 mode decoded (state)');
check(plane.laneCount === golden.lane_count, 'J1 lane_count decoded');
check(plane.laneStride === golden.lane_stride, 'J1 lane_stride decoded');
check(plane.sampleSize === golden.sample_size, 'J1 sample_size decoded');
check(plane.slotCapacity === golden.slot_capacity,
      'J1 slot_capacity decoded');
check(plane.planeSize === BigInt(golden.plane_size),
      'J1 plane_size decoded');
check(plane.createStampNs === BigInt(golden.create_stamp_ns),
      'J1 create_stamp_ns decoded');
check(golden.magic === MAGIC, 'J1 magic constant parity');

// ---- volatile registers ----------------------------------------------------
check(plane.beginSeq === BigInt(golden.begin_seq), 'J2 begin_seq');
check(plane.commitSeq === BigInt(golden.commit_seq), 'J2 commit_seq');
check(plane.dirtyMask === BigInt(golden.dirty_mask), 'J2 dirty_mask');
check(plane.dirtyTransitions === BigInt(golden.dirty_transitions),
      'J2 dirty_transitions');
check(plane.renderFrameId === BigInt(golden.render_frame_id),
      'J2 render_frame_id');
check(plane.epoch === BigInt(golden.epoch),
      'J2 epoch (derived from commit_seq)');

// ---- per-lane stats + bbox + every cell byte -------------------------------
for (const lane of golden.lanes) {
  const st = plane.laneStats(lane.lane);
  check(st.commitCount === BigInt(lane.commit_count),
        `J3 lane ${lane.lane} commit_count`);
  check(st.minRaw === BigInt(lane.min_raw) &&
        st.maxRaw === BigInt(lane.max_raw) &&
        st.currentRaw === BigInt(lane.current_raw),
        `J3 lane ${lane.lane} min/max/current raw bits`);

  const bbox = plane.bboxHarvest(lane.lane);
  check(bbox.min === lane.bbox_min && bbox.max === lane.bbox_max,
        `J3 lane ${lane.lane} bbox [${lane.bbox_min},${lane.bbox_max}]`);

  const out = new Uint8Array(plane.sampleSize);
  let cellBad = 0;
  for (const c of lane.cells) {
    const rc = plane.readCell(lane.lane, c.cell, out);
    if (rc !== OK) {
      cellBad++;
      continue;
    }
    const hex = Buffer.from(out).toString('hex');
    if (hex !== c.hex) {
      cellBad++;
    }
  }
  check(cellBad === 0, `J4 lane ${lane.lane}: all ${lane.cells.length} cells byte-exact`);
}

// ---- signal plane over Atomics ----------------------------------------------
const harvested = plane.harvestMask();
check(harvested === 0xfn, 'J5 harvest returns mask 0xf');
const again = plane.harvestMask();
check(again === 0n, 'J5 second harvest empty');
plane.remark(0b1010n);
check(plane.harvestMask() === 0b1010n, 'J5 remark visible next harvest');
const f1 = plane.frameCommit();
const f2 = plane.frameCommit();
check(f1 === 1n && f2 === 2n, 'J5 frame ids 1, 2');

// ---- role gate + torn-refusal sanity ----------------------------------------
try {
  const ro = new HotPlane(sab, { role: 1 /* producer */ });
  ro.harvestMask();
  check(false, 'J6 producer-role harvest refused');
} catch (e) {
  check(e.message.startsWith('HEDDLE_E_STATE'),
        'J6 producer-role harvest refused');
}

// Misaligned offset refusal.
try {
  new HotPlane(sab, { offset: 8 });
  check(false, 'J6 misaligned offset refused');
} catch (e) {
  check(e.message.startsWith('HEDDLE_E_ARG'), 'J6 misaligned offset refused');
}

// Corrupt magic refusal (on a scratch copy — SAB stays intact).
{
  const bad = new SharedArrayBuffer(image.byteLength);
  new Uint8Array(bad).set(image);
  Atomics.store(new Uint32Array(bad), 0, 0xdeadbeef);
  try {
    new HotPlane(bad);
    check(false, 'J6 corrupt magic refused');
  } catch (e) {
    check(e.message.startsWith('HEDDLE_E_MAGIC'), 'J6 corrupt magic refused');
  }
}

console.log(`\nJ-SERIES: ${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
