# Directive 16 — Inspector Read-Only Safety Audit Note

## 1. Architectural Guarantee
- **Read-Only Surface**: The Inspector interacts with Weft via the sole sanctioned debug-view kernel accessor (`weft_debug_view(&w, &view)` in C, `weft_debug_view()` in Rust/TS/WASM).
- **Zero Protocol Mutations**: The Inspector never executes `weft_publish`, `weft_claim`, `weft_revoke`, or `weft_reclaim`.
- **Zero Write Syscalls**: Inspector UI components and headless workers strictly consume read-only snapshots and JSON streams.
- **AXIOM T Compliance**: All counters are marked `advisory: true` and never drive protocol state or branching.

## 2. Code-Path Verification
- `tools/weft-probe/weft_probe.c`: Quiesced and live probes only call `weft_debug_view()` and standard `printf`.
- `tools/weft-playback/weft_play.c`: Files are opened with `"rb"` (read-only binary mode).
- `demos/web/src/components/Inspector.tsx`: UI telemetry consumers operate on read-only event copies.

Status: AUDIT VERIFIED READ-ONLY PASS.
