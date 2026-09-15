# DIRECTIVE-11 — npm packages: @weft/core + four framework Heddles + worker templates

- Wave: 1 · Depends: D-10 · Effort: ~8 h · Status: ISSUED

## 1. Context

The TS kernel (`core/ts/weft.ts`) and the four Heddle bindings (`heddles/{react,vue,svelte,react-native}/`) exist as verified source-only deliverables (WO-P4 T6, mapping tables in `docs/PORTS.md` §4). Production distribution requires real packages: `@weft/core`, `@weft/react`, `@weft/vue`, `@weft/svelte`, `@weft/react-native`. PORTS.md pins the package names — use them exactly.

## 2. Tasks

- **T11.1 Monorepo.** pnpm workspaces: `packages/core`, `packages/react`, `packages/vue`, `packages/svelte`, `packages/react-native`. Sources move from `core/ts` / `heddles/*` into packages (git mv; history preserved). Existing TS kernel is UNCHANGED (kernel freeze extends to the TS kernel).
- **T11.2 Build.** `unbuild` (or tsup) per package: dual ESM + CJS, `sideEffects: false` on core, sourcemaps. `.d.ts` rollup via api-extractor; the **API report file is committed** and drift = CI failure.
- **T11.3 Package metadata.** core: `zero-deps`, engines `>=18`. Framework packages: peerDeps (`react >=18`, `vue >=3.4`, `svelte >=4`, `react-native >=0.73`), core as dependency. `files:` allowlists; `exports` map with `types` first. License + repository fields.
- **T11.4 Worker templates.** `@weft/core/worker` export: template factory for an OffscreenCanvas loop with `SharedArrayBuffer` transport + a Transferable fallback path; document the COOP/COEP requirement (§8.2 of the whitepaper) in the template docstring and README. Template must demo `CustomPainter`-equivalent repaint discipline: draw reads happen in the draw callback, never in composition.
- **T11.5 Fixtures.** Vite + React fixture rendering one Triad at 60 fps via SAB path; Node fixture running the existing `litmus.ts` suite through the packaged core (proves the package, not the source tree).
- **T11.6 Publish gate.** `npm publish --dry-run` clean ×5. Actual publish requires staff go + credentials; until then packages are `--dry-run`-verified only, and `changesets` manages versions.

## 3. Non-goals

No kernel/TS-kernel changes. No bundler plugins. No CDN builds. No publish without staff go.

## 4. Acceptance criteria (mechanical)

1. `pnpm -r build` green on node 20 and 22 (CI matrix).
2. api-extractor API report committed ×5; `git diff --exit-code` on report files in CI.
3. Node fixture: litmus suite via packaged `@weft/core` — same cell results as source-tree run (24/24 or current canon), environment-tagged `node20`.
4. Size budget: `@weft/core` ESM ≤ 15 kB gzip (print actual, tag toolchain `esbuild@<ver>`); deviation declared if exceeded.
5. Vite fixture: 60 fps steady state at default load, `t_drop` == 0 over a 60 s soak, heap delta over 60 s within ±2 MB (browser: chromium, `linux-ci` tag).
6. 5× `npm publish --dry-run` exit 0.

## 5. Evidence to return

`evidence/D-11/`: build logs (both node versions), API reports, size table, fixture soak log with fps/alloc numbers + environment tags, dry-run outputs ×5.

## 6. Report

`reports/D-11-REPORT.md` per index §4.
