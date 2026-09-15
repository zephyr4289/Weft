# DIRECTIVE-12 — Android/Kotlin packaging: dev.weft AAR + JNI libweft.so (3 ABIs) + Compose library + R8

- Wave: 1 (longest pole — start immediately) · Depends: D-10 · Effort: ~12 h · Status: ISSUED

## 1. Context

The Kotlin port (`core/kotlin/{Steward,Heddle,TriadNative,Weft}.kt`) is verified source-only (WO-P4 T3, mapping table gate). Production distribution = `dev.weft:*` on Maven Central. Per the owner pivot, there are **no device tasks**: verification is build + unit/Robolectric + x86_64 native smoke on the sandbox. Nothing here claims device numbers.

## 2. Tasks

- **T12.1 Gradle structure.** Composite build `android/`: `weft-core` (Kotlin kernel port), `weft-compose` (Compose integration), `weft-bom`. Namespace `dev.weft`. Kotlin 2.x, AGP 8.x, compileSdk 35, minSdk 24. Group ids: `dev.weft`.
- **T12.2 JNI packaging.** CMake + NDK r27: compile the FROZEN `core/c/weft.c` unchanged into `libweft.so` for `arm64-v8a`, `armeabi-v7a`, `x86_64` (+`x86` optional, declared if skipped). Prefab package exposes `weft.h` to native consumers. `TriadNative.kt` binds via JNI: extern-C shield, panic shielding per the Phase-4 structural validator (validator must pass with exit 0 — it is committed-before-ports canon).
- **T12.3 Compose integration library.** `weft-compose` provides:
  - `rememberWeftHeddle()` bound to `ViewModel`-scoped `Steward` (survives recomposition; disposal ordering via `DisposableEffect` — the composition-disposal bug class is the design target);
  - `Modifier.graphicsLayer { }` deferred draw-phase read pattern (state read in draw phase, not composition) with a documented AGSL/`RenderEffect` hook for W2;
  - lifecycle policy stub calling into the RFC Q5 `ReattachPolicy` seam (design-only — D-17).
- **T12.4 Consumer R8/ProGuard rules.** `consumer-rules.pro`: keep `dev.weft.**` native method names; keep atomic field accessors used reflectively; `-keepclasseswithmembernames class dev.weft.** { native <methods>; }` scoped — no global keeps.
- **T12.5 Runtime verification (sandbox-honest).**
  - x86_64 `libweft.so`: `dlopen` on the sandbox + run the kernel self-test/litmus subset through JNI-equivalent C harness — proves the packaged .so, not the source tree.
  - `weft-core` JVM unit tests + Robolectric for Steward lifecycle (no emulator, no device).
  - R8-minified sample app assembles (`assembleRelease` ×3 ABIs) — runtime on emulator is out of scope; build-level verification only, stated plainly.
- **T12.6 Maven path.** `publishToMavenLocal` for all modules; Vanniktech/Sonatype Central Portal config present but **publish gated on credentials + staff go**. Semver `0.1.0`.

## 3. Non-goals

No kernel diffs. No instrumented tests on devices/emulators. No Compose Multiplatform (D-17 Q3 evaluates it). No perf claims of any kind on Android.

## 4. Acceptance criteria (mechanical)

1. `./gradlew assembleRelease` green ×3 ABIs (CI log, `linux-ci` + NDK r27 tags).
2. Phase-4 structural validator: exit 0 against the new JNI bindings.
3. x86_64 smoke: dlopen + self-test PASS (log + binary hash of the .so).
4. Robolectric suite green; composition-disposal test: heddle survives recomposition ×100, disposed exactly once on ViewModel clear.
5. Sample app: `assembleRelease` with `minifyEnabled true` + consumer rules — R8 build succeeds; mapping file retained as evidence.
6. `publishToMavenLocal` → POMs valid (`dev.weft:weft-core:0.1.0` resolvable from a scratch consumer project).
7. LOC/LOC-delta vs source-only port declared (deviations field).

## 5. Evidence to return

`evidence/D-12/`: build logs ×3 ABIs, validator output, dlopen smoke log, Robolectric report, R8 mapping, POM files, .so sha256 ×3 ABIs.

## 6. Report

`reports/D-12-REPORT.md` per index §4. Toolchain table mandatory (AGP/Kotlin/NDK/CMake versions) — every number tagged.
