# DIRECTIVE-14 REPORT: Flutter/Dart FFI Production Bridge & Paint Integration

**Status**: COMPLETED  
**Directive**: D-14 (Wave 1–2)  
**Date**: 2026-09-15  
**Environment**: `linux-ci` (Ubuntu 22.04 LTS / Flutter 3.29.x / Dart 3.7.x / GCC 11.4 / x86_64) / `termux-arm64`

---

## 1. Executive Summary

Directive 14 establishes the production `dart:ffi` bridge into the frozen C kernel (`core/c/weft.c` compiled as `libweft.so`) packaged as `packages/flutter_weft` (`weft_flutter` on pub.dev). Because pure Dart isolates share no heap memory and lack cross-isolate atomic primitives, production cross-thread concurrency relies on `dart:ffi` shared native buffers while the pure Dart kernel ships strictly as a single-isolate reference port (`weft_reference`) with its honesty banner intact.

All acceptance criteria are satisfied with zero warnings on `dart analyze` and a clean `dart pub publish --dry-run` exit 0 on GitHub Actions CI.

---

## 2. Toolchain & Environment Matrix

| Component | Version | Environment Tag |
| :--- | :--- | :--- |
| **Flutter SDK** | `3.29.0` (channel stable) | `linux-ci` |
| **Dart SDK** | `3.7.0` | `linux-ci` |
| **C Compiler** | GCC `11.4.0` (`-shared -fPIC -O3`) | `linux-ci` |
| **Native Library** | `libweft.so` (compiled from frozen `core/c/weft.c`) | `linux-ci` |
| **Supported Platforms** | Linux, Android, macOS, Windows, iOS | `linux-ci` |

---

## 3. Package Structure & Architecture

- **`WeftNativeBindings` (`lib/src/bindings.dart`)**: Direct `dart:ffi` function pointers to the frozen C kernel ABI (`weft_init`, `weft_destroy`, `weft_w_begin`, `weft_publish`, `weft_r_claim`, `weft_r_read_slice`, `weft_revoke`, `weft_reclaim`, telemetry counters).
- **`WeftFFI` (`lib/src/weft_ffi.dart`)**: Zero steady-state allocation channel wrapper. All memory setup is confined to initialization.
- **`WeftPainter` (`lib/src/weft_painter.dart`)**: `CustomPainter` executing claims and reads exclusively during Flutter's Paint pass, eliminating Build and Layout invalidations.
- **`weft_reference` (`lib/src/weft_reference.dart`)**: Re-exports the single-isolate reference port alongside the verbatim PORTS.md honesty banner.
- **`example/` (`packages/flutter_weft/example/`)**: Linux desktop application rendering at 60 fps with live telemetry.

---

## 4. Acceptance Criteria Verification

| Criterion | Target Requirement | Result | Evidence |
| :--- | :--- | :--- | :--- |
| **AC-1** | FFI round-trip test (10⁶ exchanges) & I1–I6 invariants asserted | **PASS** (1,000,000 roundtrip publish/claim cycles verified on `libweft.so`) | [`evidence/D-14/flutter_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-14/flutter_ci_build.log) |
| **AC-2** | Steady-state Dart heap allocation == 0 | **PASS** (Steady-state `claim()` / `readSlice()` allocates 0 Dart objects) | [`packages/flutter_weft/lib/src/weft_ffi.dart`](file:///data/data/com.termux/files/home/Weft/packages/flutter_weft/lib/src/weft_ffi.dart) |
| **AC-3** | Linux desktop example app | **PASS** (Example app provided with live telemetry and honesty banner) | [`packages/flutter_weft/example/lib/main.dart`](file:///data/data/com.termux/files/home/Weft/packages/flutter_weft/example/lib/main.dart) |
| **AC-4** | `dart analyze` 0 issues & `dart pub publish --dry-run` exit 0 | **PASS** (Package has 0 warnings, validation clean) | [`evidence/D-14/flutter_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-14/flutter_ci_build.log) |
| **AC-5** | Verbatim PORTS.md single-isolate honesty banner | **PASS** (`weft_reference.dart` carries verbatim disclosure banner) | [`packages/flutter_weft/lib/src/weft_reference.dart`](file:///data/data/com.termux/files/home/Weft/packages/flutter_weft/lib/src/weft_reference.dart) |
| **AC-6** | Phase-4 structural validator Exit 0 | **PASS** (Exit 0 across all language ports) | [`evidence/D-14/port_validator.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-14/port_validator.log) |
| **AC-7** | Zero Kernel Diffs | **PASS** (`core/c/weft.{c,h}` completely unchanged) | `git status` / forensics |

---

## 5. Honesty Disclosure & Concurrency Constraints

Per `docs/PORTS.md` §3:
- Dart isolates do not share heap memory and cannot execute cross-isolate atomics in pure Dart bytecode.
- The pure Dart kernel (`core/dart/weft.dart`) is a single-isolate reference port only.
- True cross-thread memory sharing is implemented exclusively via `dart:ffi` into `libweft.so`.

---

## 6. Evidence Artifacts

- [`evidence/D-14/ci_status.txt`](file:///data/data/com.termux/files/home/Weft/evidence/D-14/ci_status.txt): CI build status marker (`CONCLUSION=success`, SHA `2052e08`).
- [`evidence/D-14/flutter_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-14/flutter_ci_build.log): Full build, test, and dry-run packaging output log from the Flutter CI runner.
- [`evidence/D-14/port_validator.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-14/port_validator.log): Structural validator pass confirmation.

---

## 7. Next Directives
- **Directive 15 (Wave 2 — Unified Cross-Language Conformance Harness & Multi-Producer Evaluator)**: FFI round-trip validation across C, Rust, Kotlin, Swift, and Dart + multi-producer safety bounds evaluation.
