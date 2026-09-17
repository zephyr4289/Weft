import { defineConfig } from 'vitest/config';

// WHY EXISTS: @weft/react-native unit suites (weft-rn, weft-fanout-rn,
// ui-thread). Node environment with a stubbed frame-callback registrar; no
// react-native-reanimated dependency required.
//
// css.postcss inline-empty: makes the suites HERMETIC against ancestor-
// directory postcss.config.* files (the repo checked out under a webapp
// root loaded a foreign Tailwind-4 config and died on load — the same
// fix packages/core applied in the wave-2 round). No RN test imports CSS.
export default defineConfig({
  css: {
    postcss: { plugins: [] },
  },
  test: {
    environment: 'node',
    include: ['test/**/*.test.ts'],
  },
});
