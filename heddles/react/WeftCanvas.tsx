// WeftCanvas.tsx — React Heddle binding (TypeScript)
//
// WHY EXISTS: Compatibility shim. The CANONICAL React Heddle lives in
// @weft/react (packages/react) — hardened by the contrib round: the draw
// loop no longer tears down/restarts on every parent render (the inline
// lambda in the old effect deps here restarted rAF each render), and the
// draw-phase read uses the zero-allocation rLive() view per Law 2.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP; default web path is
// one-copy Transferable. Per 02-KERNEL §4.2: reads a Weft during the Draw
// phase only.
//
// This file exists so the structural validator (tools/port_validator.py)
// and any historical import path keep working. Do not add features here —
// implement them in packages/react and re-export.
//
// STATUS: SHIM — canonical implementation: @weft/react.

import { WeftCanvas, WeftCanvasProps } from '@weft/react';
import type { Weft } from '@weft/core';

export { WeftCanvas };
export type { WeftCanvasProps };
export type { Weft };
