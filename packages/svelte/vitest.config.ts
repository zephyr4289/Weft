import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/svelte binding unit suite (see test/weft-action.test.ts
// header). Node environment — Svelte actions are plain functions and are
// invoked directly against fake canvas nodes; no DOM or compiler needed.
export default defineConfig({
  css: {
    // Hermetic against ancestor postcss.config.* (foreign Tailwind-4 config
    // kills the suite when the repo is checked out under a webapp root).
    postcss: { plugins: [] },
  },
  test: {
    environment: 'node',
    include: ['test/**/*.test.ts'],
  },
});
