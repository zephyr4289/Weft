// @weft/react-heddle — real-React wiring of the framework-agnostic core.
// (Tests import ./core.js with a mount-semantics shim; this module is the
//  production surface consumed by applications with react >= 18 installed.)
import * as React from 'react';

export * from './core.js';
export { createWeftHud } from './hud.js';

const hooks = createHeddleHooks(React);

export const useWeftPlane = hooks.useWeftPlane;
export const useWeftSignal = hooks.useWeftSignal;
export const useWeftStats = hooks.useWeftStats;
export const useWeftBuffer = hooks.useWeftBuffer;
export const WeftPlaneProvider = hooks.WeftPlaneProvider;

export const WeftCanvas = createWeftCanvas(React, hooks);

export * from './visualizers.js';

const hud = createWeftHud(React, hooks);
export const WeftHud = hud.WeftHud;
export const mountWeftHud = hud.mountWeftHud;
export { createWeftHud };
