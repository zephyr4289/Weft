// src/errors.js — HPL1 Law-4 error taxonomy (docs/heddle2/HPL1-LAYOUT-V1.md §6).
//
// Constructing an Error allocates: therefore Hpl1Error is ONLY thrown on caller
// bugs / boundary violations, never in steady-state hot paths. Recoverable
// conditions (tears, epoch change, underrun) are returned as codes, not thrown.

export const HPL1 = {
  OK: 0,
  BAD_MAGIC: 1,
  BAD_VERSION: 2,
  NOT_LITTLE_ENDIAN: 3,
  CAPACITY_MISMATCH: 4,
  LANE_OUT_OF_RANGE: 5,
  TORN_SEQLOCK: 6,
  EPOCH_CHANGED: 7,
  PLANE_DETACHED: 8,
  CONTEXT_LOST: 9,
  TAB_HIDDEN: 10,
  WORKER_CRASH: 11,
  BAD_RENDER_ENGINE: 12,
  INVALID_SAMPLE: 13,
  RING_UNDERRUN: 14,
};

export const HPL1_NAME = [
  'HPL1_OK',
  'HPL1_BAD_MAGIC',
  'HPL1_BAD_VERSION',
  'HPL1_NOT_LITTLE_ENDIAN',
  'HPL1_CAPACITY_MISMATCH',
  'HPL1_LANE_OUT_OF_RANGE',
  'HPL1_TORN_SEQLOCK',
  'HPL1_EPOCH_CHANGED',
  'HPL1_PLANE_DETACHED',
  'HPL1_CONTEXT_LOST',
  'HPL1_TAB_HIDDEN',
  'HPL1_WORKER_CRASH',
  'HPL1_BAD_RENDER_ENGINE',
  'HPL1_INVALID_SAMPLE',
  'HPL1_RING_UNDERRUN',
];

export class Hpl1Error extends Error {
  constructor(code, message) {
    super(`${HPL1_NAME[code] || 'HPL1_UNKNOWN'}: ${message}`);
    this.name = 'Hpl1Error';
    this.code = code; // stable numeric code (wire/log friendly)
    this.hpl1 = HPL1_NAME[code] || 'HPL1_UNKNOWN';
  }
}
