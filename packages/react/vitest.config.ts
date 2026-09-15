import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/react binding unit suite (see test/WeftCanvas.test.tsx
// header). jsdom environment with a stubbed 2D context (patched in the suite)
// and a manually pumped requestAnimationFrame.
export default defineConfig({
  test: {
    environment: 'jsdom',
    include: ['test/**/*.test.{ts,tsx}'],
  },
});
