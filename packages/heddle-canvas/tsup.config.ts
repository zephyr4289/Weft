// tsup.config.ts — the dist build. The rig and the shard bundle from
// SOURCE (esbuild); this config exists for `pnpm build` / publishing
// (npm-packages workflow). The shaders ship as text files (package.json
// "files") and the runtime bundles carry their strings — no fs reads.
import { defineConfig } from 'tsup';

export default defineConfig({
  entry: ['src/index.ts'],
  format: ['esm', 'cjs'],
  dts: true,
  sourcemap: true,
  clean: true,
  splitting: false,
  treeshake: true,
  external: ['node:inspector'],
});
