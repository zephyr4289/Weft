# DIRECTIVE-14 — Flutter/Dart: dart:ffi production bridge into libweft.so

- Wave: 1–2 · Depends: D-12 (libweft.so artifacts), D-10 · Effort: ~8 h · Status: ISSUED

## 1. Context

PORTS.md:37–49 is the honesty load-bearing wall: pure Dart isolates share no heap and have no atomics, so the pure-Dart kernel is a **single-isolate reference implementation**. Production Flutter usage goes through `dart:ffi` into the C kernel — **specified, not implemented** (WO-P4 decision 4). This directive implements that forward interface as package `weft_flutter`, paired with `CustomPainter(repaint: Listenable)`.

## 2. Tasks

- **T14.1 FFI bindings.** `lib/src/bindings.dart`: `dart:ffi` signatures over the frozen C ABI (`weft.h`) — exchange, refresh, revoke, epoch ACK, telemetry counters, debug-view accessor. Zero allocations in steady state (`Arena`/`calloc` confined to setup; steady-state path allocation-free — assert via `malloc` counter hook in tests).
- **T14.2 Writer thread model.** Writer runs on a native C thread (spawned via FFI) — NOT a Dart isolate (isolates cannot share the kernel's heap). Setup/teardown from Dart; the render side stays on the UI isolate and receives notifications through a `Listenable` woken by the kernel's `latest` epoch (poll-on-frame is the fallback; document both).
- **T14.3 Painter integration.** `WeftPainter(repaint: heddle.notifications)`: draw callback reads slot state only in paint phase (Compose `graphicsLayer` discipline, Canvas equivalent); stale-frame path draws last-good frame and bumps `t_drop` display.
- **T14.4 Package shape.** `weft_flutter` on pub.dev conventions: `dart analyze` 0 issues, `pana` score ≥ 110 (or deviation declared), platforms: linux/windows/macos/android (ios build-only). Binary loading: bundled `libweft.so` per platform from D-12 build outputs (android) + desktop CMake fallback; `DynamicLibrary.open('libweft.so')` with documented load failure error.
- **T14.5 Example app.** W2-class workload on **linux desktop** (sandbox-runnable): 60 fps target, live `w_work/r_work/latest` + `t_drop` overlay; the single-isolate pure-Dart reference kernel ships alongside as `weft_reference` export with the PORTS.md honesty banner verbatim.
- **T14.6 Publish gate.** `dart pub publish --dry-run` clean; actual publish gated on credentials + staff go.

## 3. Non-goals

No kernel diffs. No isolate-heap tricks (no `SharedArrayBuffer`-style claims — Dart has none; that's the wall PORTS.md builds). No device/emulator runs. No Impeller/Skia-specific tuning claims.

## 4. Acceptance criteria (mechanical)

1. FFI round-trip test: 10⁶ exchanges through the real x86_64 `libweft.so` (D-12 artifact) with I1–I6 invariants asserted between phases; environment tag `linux-sandbox`.
2. Steady-state allocation: 60 s run, Dart-side allocations in the paint/exchange path == 0 (tracked-class assertion) — scope: named classes, threshold exact zero, stated in report.
3. Example app on linux desktop: 60 fps steady at default load, `t_drop` == 0 over 60 s (flame/graph log + environment tag).
4. `dart analyze` 0 issues; `pana` ≥ 110 or deviation declared; `dart pub publish --dry-run` exit 0.
5. Honesty banner: `weft_reference` export carries the PORTS.md single-isolate banner verbatim (string diff = empty).

## 5. Evidence to return

`evidence/D-14/`: FFI test logs, allocation counter output, desktop soak log, analyze/pana output, dry-run output, .so hashes consumed.

## 6. Report

`reports/D-14-REPORT.md` per index §4.
