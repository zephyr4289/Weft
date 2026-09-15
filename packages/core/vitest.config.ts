import { defineConfig } from 'vitest/config';

// WHY EXISTS: package-level unit suite for @weft/core (see test/weft.test.ts
// header). Node environment — the kernel substrate is SharedArrayBuffer +
// Atomics, available natively in Node >= 18.
export default defineConfig({
  test: {
    environment: 'node',
    include: ['test/**/*.test.ts'],
  },
});
