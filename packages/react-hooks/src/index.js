// index.js — @weft/react-hooks: React binding for the zero-GC core.
//
// Wires the hook implementations (src/core.js) to the real React. Only
// useRef + useEffect are used (react >= 18; StrictMode-safe).
import { useRef, useEffect } from 'react';

import {
  createHooks,
  createFrameSource,
  attachWebSocket,
  createSampleRing,
  drawFrameGraph,
} from './core.js';

const { useWeftBuffer, useWeftCanvas } = createHooks({ useRef, useEffect });

export { useWeftBuffer, useWeftCanvas, createFrameSource, attachWebSocket, createSampleRing, drawFrameGraph };
