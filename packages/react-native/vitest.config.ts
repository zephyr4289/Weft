import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/react-native binding unit suite (see test/weft-rn.test.ts
// header). Node environment with a stubbed frame-callback registrar; no
// react-native-reanimated dependency required.
export default defineConfig({
  test: {
    environment: 'node',
    include: ['test/**/*.test.ts'],
  },
});
