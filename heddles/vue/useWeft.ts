// useWeft.ts — Vue 3 Heddle binding (TypeScript)
//
// WHY EXISTS: Compatibility shim. The CANONICAL Vue Heddle lives in
// @weft/vue (packages/vue) — hardened by the contrib round: the per-frame
// reactive write (frameCount.value++ at display rate — the exact
// anti-pattern Weft exists to prevent) is now throttled to a 1 Hz HUD,
// the 2d context is acquired once, and the draw-phase read uses the
// zero-allocation rLive() view per Law 2.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP.
//
// This file exists so the structural validator (tools/port_validator.py)
// and any historical import path keep working. Do not add features here —
// implement them in packages/vue and re-export.
//
// STATUS: SHIM — canonical implementation: @weft/vue.

import { useWeft, UseWeftOptions } from '@weft/vue';
import type { Weft } from '@weft/core';

export { useWeft };
export type { UseWeftOptions, Weft };
