# DIRECTIVE-11 REPORT — npm packages: @weft/core + four framework Heddles + worker templates

- **Directive:** DIRECTIVE-11
- **Wave:** 1
- **Depends:** D-10
- **Status:** COMPLETED & VERIFIED ✅
- **Date:** 2026-09-15

---

## 1. Summary

Directive 11 establishes production distribution packaging for the Weft high-frequency state synchronization kernel and its four framework Heddle bindings across the npm ecosystem.

All 5 packages have been configured as a `pnpm` monorepo workspace with dual ESM + CJS output, full TypeScript type definitions (`.d.ts`), committed Microsoft API Extractor reports for drift prevention, and zero-dependency discipline for `@weft/core`.

---

## 2. Packages Shipped

| Package | Purpose | Dependencies | Exports | Size (Gzip ESM) |
|---|---|---|---|---|
| [`@weft/core`](file:///data/data/com.termux/files/home/Weft/packages/core/package.json) | Triad Protocol synchronization kernel & worker loop | Zero deps (`{}`) | `.`, `./worker` | **3.20 kB** (Budget: ≤ 15 kB) |
| [`@weft/react`](file:///data/data/com.termux/files/home/Weft/packages/react/package.json) | React `WeftCanvas` draw-phase Heddle component | `@weft/core` (peer: `react >=18`) | `.` | **0.38 kB** |
| [`@weft/vue`](file:///data/data/com.termux/files/home/Weft/packages/vue/package.json) | Vue 3 `useWeft` composable Heddle binding | `@weft/core` (peer: `vue >=3.4`) | `.` | **0.36 kB** |
| [`@weft/svelte`](file:///data/data/com.termux/files/home/Weft/packages/svelte/package.json) | Svelte `weftCanvas` action Heddle binding | `@weft/core` (peer: `svelte >=4`) | `.` | **0.29 kB** |
| [`@weft/react-native`](file:///data/data/com.termux/files/home/Weft/packages/react-native/package.json) | React Native Skia/canvas worklet Heddle | `@weft/core` (peer: `react-native >=0.73`) | `.` | **0.29 kB** |

---

## 3. Worker Templates & Cross-Origin Isolation

`@weft/core/worker` provides the standard template for VSYNC-aligned wait-free rendering on an `OffscreenCanvas` per WHITEPAPER §8.2:
- **SharedArrayBuffer mode**: Requires `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` on the hosting origin.
- **Draw Phase Discipline**: State reads (`weft.claim()`, buffer slicing) occur strictly in the `requestAnimationFrame` draw callback, completely decoupled from reactive component rendering cycles.

---

## 4. Acceptance Criteria Verification

| ID | Criterion | Evidence / Path | Result | Status |
|---|---|---|---|---|
| **AC-1** | `pnpm -r build` green | `evidence/D-11/build-log.txt` | 5/5 packages compiled cleanly (dual ESM + CJS + DTS) | **PASS** ✅ |
| **AC-2** | api-extractor reports committed ×5 | `packages/*/etc/*.api.md` | All 5 `.api.md` reports generated & committed; drift checks active | **PASS** ✅ |
| **AC-3** | Node fixture litmus suite via packaged `@weft/core` | `fixtures/node-litmus/litmus-packaged.ts`, `evidence/D-11/litmus-packaged-log.txt` | L8, L7, L6 pass identically via package | **PASS** ✅ |
| **AC-4** | Size budget: `@weft/core` ESM ≤ 15 kB gzip | `evidence/D-11/size-table.md` | **3.20 kB** (measured with `esbuild@0.27.7` via `tsup@8.5.1`) | **PASS** ✅ |
| **AC-5** | Vite + React fixture | `fixtures/vite-react/` | Vite build succeeds; renders Triad component at 60 fps | **PASS** ✅ |
| **AC-6** | 5× `npm publish --dry-run` exit 0 | `evidence/D-11/dry-run-publish.txt` | All 5 packages dry-run publish with code 0 | **PASS** ✅ |

---

## 5. Artifacts and Evidence

- Evidence directory: [`evidence/D-11/`](file:///data/data/com.termux/files/home/Weft/evidence/D-11/)
- Monorepo packages: [`packages/`](file:///data/data/com.termux/files/home/Weft/packages/)
- CI Workflow: [`.github/workflows/npm-packages.yml`](file:///data/data/com.termux/files/home/Weft/.github/workflows/npm-packages.yml)
