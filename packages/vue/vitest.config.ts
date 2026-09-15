import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/vue binding unit suite (see test/useWeft.test.ts header).
// jsdom environment with a stubbed 2D context and a manually pumped
// requestAnimationFrame.
export default defineConfig({
  test: {
    environment: 'jsdom',
    include: ['test/**/*.test.ts'],
  },
});
