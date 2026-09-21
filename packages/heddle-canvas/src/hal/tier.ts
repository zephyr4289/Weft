// tier.ts — @weft/heddle-canvas the render tier ladder (Law 4 in motion).
//
// WHY EXISTS: the Pillar 4 mandate fixes a three-tier hardware matrix —
// Tier 1 WebGPU (WGSL), Tier 2 WebGL2 (GLSL 300 es + instanced arrays),
// Tier 3 Canvas2D (pre-allocated ImageData blitting) — plus the honest
// NullHAL the CI sandbox runs when no browser exists at all. The ladder's
// job is NOT "try WebGPU, else fail": every step down is a NAMED refusal
// surfaced as a TierFallbackEvent, because a trading UI that silently
// dropped from GPU instancing to CPU blitting without telling anyone is a
// Law 4 violation even if it keeps drawing (docs/PHILOSOPHY.md: "every
// capability claim ships with its boundary").
//
// Probes are INJECTED (ProbeEnv) so the node battery can fake any
// capability matrix deterministically — including the failure modes
// (device lost at init, context lost mid-frame, shader compile refused)
// that are rare on real hardware and must not stay untested because of it.

import { HeddleError, type TierFallbackEvent } from '../errors.ts';

export const RENDER_TIERS = {
  /** Tier 1 — WebGPU: WGSL compute decimation + storage buffers. */
  WEBGPU: 0,
  /** Tier 2 — WebGL2: GLSL 300 es, transform-feedback decimation, instancing. */
  WEBGL2: 1,
  /** Tier 3 — Canvas2D: pre-allocated ImageData direct pixel blitting. */
  CANVAS2D: 2,
  /** Tier 0-honest — NullHAL: full pipeline bookkeeping, GPU submit elided. */
  NULL: 3,
} as const;
export type RenderTierId = (typeof RENDER_TIERS)[keyof typeof RENDER_TIERS];

export function tierName(t: number): string {
  switch (t) {
    case RENDER_TIERS.WEBGPU: return 'webgpu';
    case RENDER_TIERS.WEBGL2: return 'webgl2';
    case RENDER_TIERS.CANVAS2D: return 'canvas2d';
    case RENDER_TIERS.NULL: return 'null';
    default: return 'unknown';
  }
}

/**
 * Counters the HUD and the audit report read — deliberately the same shape
 * as core/c weft_render_bridge_t (bind_count / present_count /
 * is_direct_mapped) so a heddle-canvas engine and the C render bridge can
 * sit side by side in one ops dashboard without a translation layer.
 */
export interface HalStats {
  /** Successful lane->GPU binds (sub-range uploads issued). */
  bindCount: number;
  /** Frames presented. */
  presentCount: number;
  /** Lanes whose draw call AND upload were skipped (clean dirty word). */
  skippedCleanLanes: number;
  /** Bytes moved lane->GPU (the number the zero-copy claim is audited on). */
  uploadedBytes: number;
  /** Upload calls issued (sub-range granularity — != bindCount when one
   *  bind covers a contiguous multi-bit range: it does, by design). */
  uploadCalls: number;
  drawCalls: number;
  scissorClips: number;
  degradations: number;
}

export function createHalStats(): HalStats {
  return {
    bindCount: 0, presentCount: 0, skippedCleanLanes: 0,
    uploadedBytes: 0, uploadCalls: 0, drawCalls: 0,
    scissorClips: 0, degradations: 0,
  };
}

/** Engine-wide configuration, fixed at initialize() (Law 1: immutable). */
export interface EngineConfig {
  readonly canvasWidth: number;
  readonly canvasHeight: number;
  /** Target presentation rate — the 240 Hz frame budget clock. */
  readonly tickHz: number;
  /** Oscilloscope decimation columns (one min/max pair per column). */
  readonly columnCount: number;
}

/**
 * The injected capability environment. In the browser rig every probe is
 * the real platform call; in the node battery each probe is a fake with a
 * scripted outcome. `sabViewAccepted` is probed ONCE at init with a
 * canary upload (see webgl2_binder / webgpu_binder): browsers that refuse
 * SharedArrayBuffer views on the upload path force the ONE pre-allocated
 * staging ArrayBuffer per lane — still zero per-frame JS allocation, one
 * extra copy, labeled in stats (Law 4: the copy is counted, never hidden).
 */
export interface ProbeEnv {
  /** Returns an adapter-ish object, or null (WebGPU absent/refused). */
  readonly probeWebGPU: () => Promise<object | null>;
  /** Returns a WebGL2 context-ish object, or null. */
  readonly probeWebGL2: () => object | null;
  /** Returns a 2D context-ish object, or null. */
  readonly probeCanvas2D: () => object | null;
}

export interface LadderResult {
  readonly tiers: readonly RenderTierId[];
  readonly refusals: readonly TierFallbackEvent[];
}

/**
 * Walk the mandated ladder top-down. Returns the ordered list of tiers
 * worth attempting (the first that initializes wins) plus the refusal
 * trail — the trail is the artifact, not an error to swallow.
 */
export async function walkTierLadder(env: ProbeEnv): Promise<LadderResult> {
  const tiers: RenderTierId[] = [];
  const refusals: TierFallbackEvent[] = [];

  const gpu = await env.probeWebGPU();
  if (gpu !== null) {
    tiers.push(RENDER_TIERS.WEBGPU);
  } else {
    refusals.push({
      from: 'webgpu', to: 'webgl2', code: 'HC_E_BACKEND_REFUSED',
      detail: 'navigator.gpu absent or requestAdapter() returned null',
    });
  }

  const gl = env.probeWebGL2();
  if (gl !== null) {
    tiers.push(RENDER_TIERS.WEBGL2);
  } else {
    refusals.push({
      from: 'webgl2', to: 'canvas2d', code: 'HC_E_BACKEND_REFUSED',
      detail: 'getContext("webgl2") returned null',
    });
  }

  const ctx2d = env.probeCanvas2D();
  if (ctx2d !== null) {
    tiers.push(RENDER_TIERS.CANVAS2D);
  } else {
    refusals.push({
      from: 'canvas2d', to: 'null', code: 'HC_E_BACKEND_REFUSED',
      detail: 'getContext("2d") returned null',
    });
  }

  // The NullHAL is always available by definition — it is the honest floor.
  tiers.push(RENDER_TIERS.NULL);
  return { tiers, refusals };
}

/**
 * Assert a tier id is one of the mandated four — guards against probe
 * matrices returning garbage (node battery hammers this).
 */
export function assertTierId(t: number): void {
  if (t !== RENDER_TIERS.WEBGPU && t !== RENDER_TIERS.WEBGL2
    && t !== RENDER_TIERS.CANVAS2D && t !== RENDER_TIERS.NULL) {
    throw new HeddleError('HC_E_BACKEND_REFUSED', `tier id ${t} outside the matrix`);
  }
}
