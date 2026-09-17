import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/vue binding unit suite (see test/useWeft.test.ts header).
// jsdom environment with a stubbed 2D context and a manually pumped
// requestAnimationFrame.
export default defineConfig({
  css: {
    // Hermetic against ancestor postcss.config.* (foreign Tailwind-4 config
    // kills the suite when the repo is checked out under a webapp root).
    postcss: { plugins: [] },
  },
  test: {
    environment: 'jsdom',
    include: ['test/**/*.test.ts'],
  },
});
