// errors.ts — @weft/heddle-canvas Law 4 error taxonomy.
//
// WHY EXISTS: Pillar 4's Law 4 (house canon docs/PHILOSOPHY.md "Honesty is a
// feature"): every refusal a heddle-canvas engine can produce has a NAME, a
// code, and a documented boundary. No engine path throws a bare Error with a
// free-text message — callers (and the tier ladder) branch on `code`, the
// audit report lists every code that can fire, and the CI battery makes each
// refusal fire at least once so the ladder is MEASURED, not declared.
//
// The taxonomy is split in three families exactly where refusals originate:
//   PLANE_*  — the Hot-Plane view refused to open (Engineer 1's contract
//              enforcement — see src/plane/hot_plane.ts refusal ladder).
//   BACKEND_ — a render backend refused or died (capability probe, device /
//              context loss, shader compilation).
//   ENGINE_  — the frame engine refused an operation (frozen-kind misuse,
//              pool exhaustion, double-start).

export const HC_ERROR_CODES = [
  // --- plane family ---------------------------------------------------------
  'HC_E_NOT_SAB',
  'HC_E_PLANE_TOO_SMALL',
  'HC_E_PLANE_MAGIC',
  'HC_E_PLANE_VERSION',
  'HC_E_PLANE_HEADER_BYTES',
  'HC_E_PLANE_LANE_COUNT',
  'HC_E_LANE_MAGIC',
  'HC_E_LANE_KIND',
  'HC_E_LANE_STRIDE',
  'HC_E_LANE_OFFSET',
  'HC_E_LANE_CAPACITY',
  'HC_E_LANE_GRANULARITY',
  // --- backend family -------------------------------------------------------
  'HC_E_NO_BACKEND',
  'HC_E_BACKEND_REFUSED',
  'HC_E_DEVICE_LOST',
  'HC_E_CONTEXT_LOST',
  'HC_E_SHADER_COMPILE',
  'HC_E_SAB_VIEW_REFUSED',
  // --- engine family --------------------------------------------------------
  'HC_E_LANE_UNSUPPORTED_KIND',
  'HC_E_POOL_EXHAUSTED',
  'HC_E_LOOP_ALREADY_RUNNING',
  'HC_E_LOOP_NOT_RUNNING',
  // --- law family -----------------------------------------------------------
  // The Law-1 heap gate refused to run (no forced GC) — named so a CI leg
  // that forgets --expose-gc fails with the FIX in the message, not a
  // silent skip.
  'HC_E_LAW1_GATE',
] as const;

export type HeddleErrorCode = (typeof HC_ERROR_CODES)[number];

/** A named, documented refusal. `code` is the stable branch key. */
export class HeddleError extends Error {
  readonly code: HeddleErrorCode;
  constructor(code: HeddleErrorCode, message: string) {
    super(`[${code}] ${message}`);
    this.name = 'HeddleError';
    this.code = code;
  }
}

/**
 * A degradation event emitted by the tier ladder (Law 4 in motion): the
 * engine NEVER silently continues on a weaker backend — every step down is
 * an event a HUD or test can observe. Carved as a plain object because the
 * ladder allocates these OUTSIDE the frame hot path (they fire at most once
 * per degradation, never per frame — Law 1).
 */
export interface TierFallbackEvent {
  from: string;
  to: string;
  code: HeddleErrorCode;
  detail: string;
}
