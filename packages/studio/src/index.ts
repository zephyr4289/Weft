/**
 * Weft Studio — managed package entry (Pillar 7).
 * Binds the host React, then re-exports the engine + panels.
 * Zero runtime dependencies beyond react.
 */

import * as React from 'react';
import { bindReact } from './engine/react-adapter';

bindReact(React as never);

export * from './engine/types';
export * from './engine/schema';
export * from './engine/layout';
export * from './engine/codegen';
export * from './engine/codegen-internals';
export * from './engine/ring';
export * from './engine/ingest';
export * from './engine/sim';
export * from './engine/scheduler';
export * from './engine/flightrec';
export * from './engine/react-adapter';
export * from './engine/studio-engine';
export * from './ui/studio';
export { STUDIO_THEME } from './ui/theme';
