import { defineConfig } from 'vitest/config';

// WHY EXISTS: package-level unit suite for @weft/core (see test/weft.test.ts
// header). Node environment — the kernel substrate is SharedArrayBuffer +
// Atomics, available natively in Node >= 18.
//
// css.postcss inline-empty: makes the suite HERMETIC against ancestor-
// directory postcss.config.* files (monorepo workspaces where this repo is
// checked out under a webapp root). Vite walks up from the config root and
// would otherwise load a foreign PostCSS config — e.g. a Tailwind-4 ESM
// config — and die on load. Pinning an empty plugin list keeps CSS
// processing out of a kernel test suite entirely (no test imports CSS).
export default defineConfig({
  css: {
    postcss: { plugins: [] },
  },
  test: {
    environment: 'node',
    include: ['test/**/*.test.ts'],
  },
});
