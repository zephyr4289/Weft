# 🏛️ Pillar 1: `weftc` Universal Zero-Serialization Compiler

> **Target Paradigm:** Obliterate Protobuf, FlatBuffers, Cap'n Proto, and gRPC.  
> **Core Metric:** **0 ns Decode Overhead** (True in-place memory projection).

---

## 1. Problem Statement & Motivation

Existing serialization frameworks impose heavy CPU, latency, and memory burdens:
- **Protobuf / gRPC:** Require serializing to variable-length wire formats (varints, tag-value pairs). Every message received requires a heap allocation per sub-object and an expensive CPU decode traversal.
- **FlatBuffers / Cap'n Proto:** Eliminate full serialization trees but introduce vtable offset tables and pointer chasing. Structs often fail to align naturally with 64-byte or 128-byte hardware cachelines and are incompatible with GPU Uniform Buffer Objects (UBO) without translation.

**The `weftc` Solution:**
A unified schema compiler that compiles declarative `.weft` definitions into **frozen, statically aligned C/Rust memory structures and direct buffer views**. A message in shared memory or network buffers is consumed via direct memory pointer cast:

$$\text{Decode Time} = 0\,\text{ns}$$

---

## 2. System Architecture

```
                  ┌────────────────────────────────────────┐
                  │          `.weft` Schema File           │
                  └──────────────────┬─────────────────────┘
                                     │
                        [ COMPILER FRONTEND (Core) ]
                                     ▼
         ┌───────────────────────┐            ┌────────────────────────┐
         │ Lexer, Parser & AST   │ ─────────► │ Deterministic Layout   │
         │ (Grammar & Validation)│            │ & Alignment Engine     │
         └───────────────────────┘            └───────────┬────────────┘
                                                          │
                                     ┌────────────────────┴───────────────────┐
                                     │ IrSchema / StructLayout Intermediate IR│
                                     │ (With 64-bit Schema Fingerprint Hash)  │
                                     └────────────────────┬───────────────────┘
                                                          │
                         ┌────────────────────────────────┴────────────────────────────────┐
                         │                                                                 │
              [ NATIVE / GPU CODEGEN ]                                          [ MANAGED / UI CODEGEN ]
                         ▼                                                                 ▼
           • C11 `<schema>.h` (zero copy)                                    • TypeScript `DataView` accessors
           • Rust `#[repr(C)]` & `bytemuck`                                  • Swift memory-bound pointers
           • WebGPU (WGSL) & GLSL structs                                    • Dart FFI & Python buffer protocol
```

---

## 3. The `.weft` Language Specification

### 3.1 Type System & Directives
```weft
// Sample telemetry frame schema
namespace weft.telemetry;

enum EngineState : u8 {
    IDLE = 0,
    IGNITION = 1,
    FULL_THRUST = 2,
    RECOVERY = 3
}

@align(64)
@packed
struct TelemetryFrame {
    u64 timestamp_ns;
    f32 velocity_vector[3];
    f32 pressure_pa;
    EngineState state;
    u8 _pad[3];               // Explicit compile-time verified padding
    f64 gps_coordinates[2];
    u8 payload_hash[32];
}
```

### 3.2 Layout Rules & Guarantees
1. **Natural Alignment:** Every scalar type of size $S$ is aligned to an address divisible by $S$.
2. **Explicit Cacheline Boundaries:** Structs marked with `@align(64)` or `@align(128)` are guaranteed to occupy dedicated cacheline blocks without false sharing.
3. **Deterministic Field Packing:** With `@optimize(packing)`, the compiler sorts fields to eliminate all internal alignment gaps.
4. **Cryptographic Schema Hashing:** A deterministic 64-bit fingerprint is embedded in headers to guarantee runtime ABI compatibility across microservices, IPC nodes, and GPUs.

---

## 4. Work Distribution (Swarm-by-Layer)

- **Senior Engineer 1 (Compiler Core & Layout Engine):**
  - CLI driver (`weftc check`, `weftc inspect`, `weftc compile`).
  - EBNF grammar, lexer, AST constructor, and semantic error reporting.
  - Deterministic alignment and memory offset calculator.
  - IR schema export (`--dump-ir` JSON / binary).

- **Senior Engineer 2 (Native Systems & GPU Codegen):**
  - C11 header generator with `static_assert(sizeof(...) == N)` guards.
  - Rust generator with `#[repr(C)]`, `#[derive(Copy, Clone, Pod, Zeroable)]`.
  - WebGPU (WGSL) and Vulkan/OpenGL (GLSL) memory-aligned struct definitions.

- **Senior Engineer 3 (Managed Runtimes & UI Bindings):**
  - TypeScript zero-GC `DataView` wrappers and ArrayBuffer readers.
  - Swift `UnsafeRawPointer` bound structs for iOS/macOS.
  - Dart FFI struct mappings for Flutter.
  - Python buffer protocol bindings with direct NumPy view integration.
