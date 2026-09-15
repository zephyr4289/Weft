import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/svelte binding unit suite (see test/weft-action.test.ts
// header). Node environment — Svelte actions are plain functions and are
// invoked directly against fake canvas nodes; no DOM or compiler needed.
export default defineConfig({
  test: {
    environment: 'node',
    include: ['test/**/*.test.ts'],
  },
});
