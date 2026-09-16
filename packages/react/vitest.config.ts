import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/react binding unit suite (see test/WeftCanvas.test.tsx
// header). jsdom environment with a stubbed 2D context (patched in the suite)
// and a manually pumped requestAnimationFrame.
export default defineConfig({
  css: {
    // Hermetic against ancestor postcss.config.* (foreign Tailwind-4 config
    // kills the suite when the repo is checked out under a webapp root).
    postcss: { plugins: [] },
  },
  test: {
    environment: 'jsdom',
    include: ['test/**/*.test.{ts,tsx}'],
  },
});
