# DIRECTIVE-12 REPORT: Android/Kotlin Packaging & JNI Integration

**Status**: COMPLETED  
**Directive**: D-12 (Wave 1)  
**Date**: 2026-09-15  
**Environment**: `linux-ci` (Ubuntu 22.04 LTS / JDK 17 / NDK r27 / CMake 3.22.1 / AGP 8.7.3 / Kotlin 2.1.0) / `termux-arm64`

---

## 1. Executive Summary

Directive 12 transitions the verified Kotlin port into canonical Maven artifacts (`dev.weft:*`) and JNI native libraries compiled directly from the frozen C kernel (`core/c/weft.c`) without any kernel modifications (zero kernel diffs). The build and verification pipeline executes in GitHub Actions CI with full artifact retention and passes all acceptance criteria:

1. **Gradle Structure (`android/`)**:
   - `dev.weft:weft-core:0.1.0` (Core Kotlin API + JNI bindings + native `.so` for 3 ABIs: `arm64-v8a`, `armeabi-v7a`, `x86_64`).
   - `dev.weft:weft-compose:0.1.0` (Jetpack Compose draw-phase deferred read modifiers + ViewModel-scoped `rememberWeftHeddle`).
   - `dev.weft:weft-bom:0.1.0` (Bill of Materials).
   - `dev.weft:sample-app` (Compose application exercising consumer ProGuard/R8 rules).
2. **JNI Packaging**:
   - CMake 3.22.1 + NDK r27 (`27.0.12077973`) compiling `core/c/weft.c` and `weft_jni.c` into `libweft_core.so` across `arm64-v8a`, `armeabi-v7a`, and `x86_64`.
   - Complete panic shielding across all JNI entrypoints (`TriadNative.kt`).
3. **Compose Integration**:
   - `rememberWeftHeddle()` surviving recomposition and bound to `ViewModel`/`DisposableEffect`.
   - `Modifier.weftDraw()` and `Modifier.weftGraphicsLayer()` implementing draw-phase deferred reads (avoiding composition invalidations).
   - `ReattachPolicy` seam integrated per RFC Q5.
4. **Consumer R8 Rules & Verification**:
   - Scoped `-keep` rules preserving JNI entrypoints and native method names in `consumer-rules.pro`.
   - Sample app `assembleRelease` with `isMinifyEnabled = true` compiling cleanly.
5. **Phase-4 Structural Validation**:
   - `python3 tools/port_validator.py --target all` returns Exit 0.

---

## 2. Toolchain & Environment Matrix

| Component | Version | Environment Tag |
| :--- | :--- | :--- |
| **Android Gradle Plugin (AGP)** | `8.7.3` | `linux-ci` |
| **Kotlin Gradle Plugin** | `2.1.0` | `linux-ci` / `termux-arm64` |
| **Gradle** | `8.11.1` | `linux-ci` |
| **Android NDK** | `27.0.12077973` (r27) | `linux-ci` |
| **CMake** | `3.22.1` | `linux-ci` |
| **Compile SDK / Target SDK** | `35` / `35` | `linux-ci` |
| **Min SDK** | `24` (Android 7.0+) | `linux-ci` |
| **JDK** | `17.0.14` (Eclipse Temurin) | `linux-ci` |
| **Target ABIs** | `arm64-v8a`, `armeabi-v7a`, `x86_64` | `linux-ci` |

---

## 3. Acceptance Criteria Verification

| Criterion | Target Requirement | Result | Evidence |
| :--- | :--- | :--- | :--- |
| **AC-1** | `./gradlew assembleRelease` green across 3 ABIs | **PASS** (132 tasks executed/up-to-date, exit 0) | [`evidence/D-12/gradle_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-12/gradle_ci_build.log) |
| **AC-2** | Phase-4 structural validator Exit 0 | **PASS** (Exit 0 across Kotlin, Swift, Dart, TS) | [`evidence/D-12/port_validator.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-12/port_validator.log) |
| **AC-3** | Panic-shielded JNI bindings compiled | **PASS** (CMake compiled `libweft_core.so` for 3 ABIs) | [`android/weft-core/src/main/cpp/weft_jni.c`](file:///data/data/com.termux/files/home/Weft/android/weft-core/src/main/cpp/weft_jni.c) |
| **AC-4** | Steward lifecycle & recomposition survival | **PASS** (100 recompositions simulated, 0 leaks on clear) | [`android/weft-core/src/test/kotlin/dev/weft/StewardLifecycleTest.kt`](file:///data/data/com.termux/files/home/Weft/android/weft-core/src/test/kotlin/dev/weft/StewardLifecycleTest.kt) |
| **AC-5** | R8 minification verification | **PASS** (`:sample-app:minifyReleaseWithR8` succeeded) | [`android/sample-app/build.gradle.kts`](file:///data/data/com.termux/files/home/Weft/android/sample-app/build.gradle.kts) |
| **AC-6** | Maven Local Publication | **PASS** (POMs generated for core, compose, bom) | [`android/weft-bom/build.gradle.kts`](file:///data/data/com.termux/files/home/Weft/android/weft-bom/build.gradle.kts) |
| **AC-7** | Zero Kernel Diffs | **PASS** (`core/c/weft.{c,h}` completely unchanged) | `git status` / forensics |

---

## 4. Evidence Artifacts

- [`evidence/D-12/ci_status.txt`](file:///data/data/com.termux/files/home/Weft/evidence/D-12/ci_status.txt): CI build status marker (`CONCLUSION=success`, SHA `a27fb75`).
- [`evidence/D-12/gradle_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-12/gradle_ci_build.log): Full Gradle build and test log from CI execution.
- [`evidence/D-12/port_validator.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-12/port_validator.log): Structural validator pass confirmation.

---

## 5. Next Directives
- **Directive 13 (Wave 1 — Apple/Swift Package Manager)**: SPM package `Weft` supporting iOS/macOS/visionOS/watchOS/tvOS with Swift 6 strict concurrency, `@MainActor` SwiftUI Heddle, and Metal deferred read hooks.
