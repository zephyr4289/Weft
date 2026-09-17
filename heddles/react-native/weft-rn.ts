// weft-rn.ts — React Native Heddle binding (TypeScript)
//
// WHY EXISTS: Compatibility shim. The CANONICAL React Native Heddle lives
// in @weft/react-native (packages/react-native) — hardened by the contrib
// round: the broken 'worklet' directive capturing a class instance is gone
// (it failed at runtime when workletized), both branches return a disposer
// consistently, and the draw-phase read uses the zero-allocation rLive()
// view per Law 2. Per WHITEPAPER §8.4: Reanimated is the prior art Weft's
// RN story wraps — the weakest differentiator; RN ports last in the
// roadmap.
//
// This file exists so the structural validator (tools/port_validator.py)
// and any historical import path keep working. Do not add features here —
// implement them in packages/react-native and re-export.
//
// STATUS: SHIM — canonical implementation: @weft/react-native.

import { useWeftDraw } from '@weft/react-native';
import {
  createUiThreadFrameSource,
  uiThreadClaim,
  useWeftUiThread,
} from '@weft/react-native';
import type { Weft } from '@weft/core';
import type { UiThreadFrameSource } from '@weft/react-native';

export { useWeftDraw, createUiThreadFrameSource, uiThreadClaim, useWeftUiThread };
export type { Weft, UiThreadFrameSource };
