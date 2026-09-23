// index.js — @weft/react-tensor public surface.
import { createTensorCanvasHooks } from './core.js';

export { TensorCanvasController, createTensorCanvasHooks } from './core.js';

// React binding: peer dependency, imported lazily-free (plain ESM import).
import * as React from 'react';
export const useWeftTensorCanvas = createTensorCanvasHooks({
  useRef: React.useRef,
  useEffect: React.useEffect,
});
