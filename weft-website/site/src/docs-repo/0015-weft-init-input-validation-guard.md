---
RFC: 0015
Title: weft_init Input Validation Wall and Construction Geometry Bounds
Status: Standard (Implemented in Tier 4, Issue #19)
Authors: Weft Core Team
Created: 2026-09-19
Supersedes / Superseded-by: None
---

# RFC 0015 — weft_init Input Validation Wall and Construction Geometry Bounds

## Summary

Defines the normative construction-time validation requirements for `weft_init`, `weft_fanout_init`, and all cross-language port constructors (C, Rust, TypeScript, Kotlin, Dart, Swift).

Establishes the architectural rule: **Kernel pre-conditions on geometry must fail closed at initialization time, preventing integer wraparounds, zero allocations, and out-of-bounds access before runtime buffers are allocated.**

## Motivation

The Weft core protocol (Theorems T1–T5) operates on pre-allocated triad ring buffers. Arithmetic on `payload_max` during construction:
```c
raw = 16 + payload_max + 8;
buf_size = (raw + 63) & ~63;
```
can wrap if passed `payload_max > SIZE_MAX - 64` or huge integers, leading to allocation of undersized buffers followed by heap out-of-bounds writes.

While runtime hot paths (`weft_publish`, `weft_r_claim`) remain zero-overhead and lock-free, construction is cold-path. Hardening `weft_init` to reject invalid geometry before any allocation or pointer arithmetic permanently eliminates the entire class of construction-time memory vulnerabilities without taxing hot-path throughput.

## Specification

### 1. Payload Limit Constant

Every port defines `WEFT_PAYLOAD_MAX_LIMIT`:
- Default: `1048576` (1 MiB = $2^{20}$ bytes).
- Overridable via `#ifndef WEFT_PAYLOAD_MAX_LIMIT` in C/C++ or configuration parameters in high-level bindings for large triad payloads.

### 2. Validation Guard at `weft_init`

Before performing any buffer size calculation or memory allocation:
```c
if (!w || payload_max == 0 || payload_max > WEFT_PAYLOAD_MAX_LIMIT) {
    return -1;
}
```

### 3. Cross-Language Parity
- **C (`weft.c`)**: Returns `-1` on validation failure; no memory allocated.
- **Rust (`lib.rs`)**: Returns `None` or `Err(WeftError::InvalidGeometry)` on invalid geometry.
- **TypeScript (`weft.ts`)**: Throws `TypeError` / `RangeError` on invalid payload sizes.
- **Kotlin / Dart / Swift**: Throws `IllegalArgumentException` / `ArgumentError` / `fatalError` at initialization.

### 4. Architectural Principle

> *"The kernel proves the protocol. The validation wall proves the input. The threat model proves the boundary. Nothing else is a kernel bug."*
