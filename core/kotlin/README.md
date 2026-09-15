# Kotlin/Android Port — Weft

> **STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION**
>
> Per WHITEPAPER §8.6: this port is structurally validated but not compiled
> or benchmarked in-sandbox. No performance claims. Phase 6+ replaces with
> real-device numbers.

## Why exists

Implements the corrected Triad Protocol (RFC-0001 §4) for the JVM/Android.
The exchange maps to `AtomicReference.getAndSet` (single RMW, SC ordering —
strictly stronger than C AcqRel per WO-P4 decision 2). Per docs/PORTS.md §1
for the full memory-model mapping table.

## JVM-SC divergence note

JVM is SC-only; SC ≥ C AcqRel. The port is *stronger* than the C reference,
not weaker. No weaker-ordering heroics to "match" C. Per WHITEPAPER §7.1:
"the web port is stronger, and why that is not cheating." Same logic extends to JVM.

## Files

| File | Role |
|---|---|
| `Weft.kt` | Kernel: Triad exchange, envelope codec, I6 handshake, debug view |
| `Steward.kt` | Lifecycle manager: ViewModel-scoped, leak detection |
| `Heddle.kt` | Draw-phase binding: `Modifier.weftDraw` Compose extension |
| `TriadNative.kt` | JNI bridge to litmus-passing C kernel (panic-shielded) |

## Dependencies

- `androidx.lifecycle:lifecycle-viewmodel-ktx` (Steward scope)
- `androidx.compose.ui:ui` (Heddle draw-phase binding)
- JNI: links `libweft_core.so` (the litmus-passing C kernel from `core/c/`)

## AXIOM T

Telemetry counters are advisory. No port logic branches on a telemetry counter
(contracts v1.3). The `debugState()` return values for `tPublish`/`tClaim`/
`tDrop` carry `advisory: true` in JSON output.
