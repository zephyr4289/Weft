// weft-action.ts — Svelte Heddle binding (TypeScript)
//
// WHY EXISTS: Compatibility shim. The CANONICAL Svelte Heddle lives in
// @weft/svelte (packages/svelte) — hardened by the contrib round: params
// are no longer captured forever at first run (the stale-Weft bug), an
// update() handler re-binds on parameter change, and the draw-phase read
// uses the zero-allocation rLive() view per Law 2.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP.
//
// This file exists so the structural validator (tools/port_validator.py)
// and any historical import path keep working. Do not add features here —
// implement them in packages/svelte and re-export.
//
// STATUS: SHIM — canonical implementation: @weft/svelte.

import { weftCanvas, WeftActionParams } from '@weft/svelte';
import type { Weft } from '@weft/core';

export { weftCanvas };
export type { WeftActionParams, Weft };
