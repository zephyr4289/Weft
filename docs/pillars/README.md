# 🏛️ The Four Architectural Pillars of Weft
## Universal High-Performance Compute, Streaming & Zero-Copy Fabric

> **"Obliterate the serialization, staging, and latency taxes of modern computing."**

---

## 🌟 Executive Overview

Weft establishes a new paradigm in systems engineering by eliminating the CPU cache misses, heap allocations, serialization overhead, and memory copies that plague modern distributed and interactive software.

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                      THE WEFT UNIVERSAL COMPUTE & STREAMING FABRIC                       │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ 1. THE "ZERO-SERIALIZATION" COMPILER (`weftc`)                                          │
│    Obliterates: Protobuf, FlatBuffers, gRPC, Cap'n Proto                                │
│    Direct in-place field reads across 7 languages directly on L1/L2 cache lines         │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ 2. THE ZERO-COPY EDGE AI & TENSOR PIPELINE (`weft-tensor`)                              │
│    Obliterates: Python/PyTorch/ONNX memory copy staging                                  │
│    Camera/Mic DMA → Neural Engine/LLM (ONNX/Llama.cpp) → GPU VRAM → 120 FPS UI (<1ms)   │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ 3. SUB-MICROSECOND DISTRIBUTED RDMA / io_uring / eBPF CLUSTER RING (`weft-cluster`)     │
│    Obliterates: Kafka, NATS, ZeroMQ, WebSockets in ultra-low-latency distributed systems│
│    Hardware RDMA (RoCEv2) one-sided writes: Server A Memory → Server B Memory in <500ns │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ 4. THE 240 FPS "UNIFIED HOT-PLANE" FOR MODERN UIs (`heddle-2.0`)                        │
│    Obliterates: Redux, MobX, Zustand, Riverpod for high-frequency realtime graphics     │
│    Zero-GC, zero-re-render VSYNC data-binding for Trading, Robotics, DAWs, and Games    │
└─────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 🧭 The Four Pillars At A Glance

| Pillar | Focus Area | Technologies Replaced | Primary Breakthrough | Latency / Throughput Target |
|---|---|---|---|---|
| **[Pillar 1: `weftc`](./01-WEFTC-ZERO-SERIALIZATION-COMPILER.md)** | Zero-Serialization IDL Compiler | Protobuf, FlatBuffers, Cap'n Proto, gRPC | 0ns decode overhead; bit-exact L1 cache-line aligned struct projection across 7 languages | **0 ns decode** (Direct pointer cast) |
| **[Pillar 2: `weft-tensor`](./02-ZERO-COPY-EDGE-AI-TENSOR-FABRIC.md)** | Realtime Edge AI & Sensor DMA | Python/C++ PyTorch staging, ONNX runtime copies | Direct DMA ring from camera/mic → NPU/GPU shared tensor arena → 120 FPS UI | **< 1 ms end-to-end** inference loop |
| **[Pillar 3: `weft-cluster`](./03-DISTRIBUTED-RDMA-EBPF-CLUSTER-RING.md)** | Hardware RDMA & eBPF Cluster Fabric | Kafka, NATS, ZeroMQ, RabbitMQ | One-sided RDMA (RoCEv2) and Linux `io_uring`/eBPF kernel-bypass cross-node memory sync | **< 500 ns** cross-server sync |
| **[Pillar 4: `heddle-2.0`](./04-HEDDLE2-UNIFIED-HOT-PLANE-UI.md)** | 240 FPS Zero-GC UI Hot-Plane | Redux, Zustand, MobX, Flutter setState | In-memory shared float/int arrays bound directly to Canvas/WebGL/Metal at display refresh | **0 GC cycles**, 240 FPS lock |

---

## ⚡ The Engineering Execution Model: Swarm-by-Layer

Rather than assigning one engineer per siloed pillar (which causes architectural drift and integration bottlenecks), the team executes using the **Swarm-by-Layer** pattern: **all 3 Senior Engineers swarm on ONE pillar at a time**, divided cleanly across horizontal architectural layers.

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│ ENGINEER 3: HIGH-LEVEL RUNTIMES & REACTIVE UI BINDINGS                                  │
│ • TypeScript (DataView / ArrayBuffer) · Swift struct views · Dart FFI                   │
│ • Python buffer protocol · React/Flutter/SwiftUI auto-generated zero-GC hooks           │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ ENGINEER 2: NATIVE SYSTEM RUNTIMES & GPU COMPUTE CODEGEN                                │
│ • C11 memory-mapped struct headers · Rust zero-copy structs (`#[repr(C)]`)              │
│ • WebGPU (WGSL) & Vulkan (GLSL) memory-aligned uniform buffer shaders                   │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ ENGINEER 1: COMPILER CORE, GRAMMAR & STATIC LAYOUT ARITHMETIC ENGINE                    │
│ • `.weft` Schema Parser & Grammar · AST Semantic Validator · Field Offset Engine        │
│ • 64B/128B Cache-line alignment verifier · Cryptographic Schema Version Hashing        │
└─────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 📂 Pillar Documentation Index

### Foundational Pillars (1 – 4)
1. **[01-WEFTC-ZERO-SERIALIZATION-COMPILER.md](./01-WEFTC-ZERO-SERIALIZATION-COMPILER.md)** — Compiler architecture, AST format, memory alignment rules, and multi-language codegen specs.
2. **[02-ZERO-COPY-EDGE-AI-TENSOR-FABRIC.md](./02-ZERO-COPY-EDGE-AI-TENSOR-FABRIC.md)** — Hardware DMA capture, zero-copy tensor ring buffers, onnxruntime/Llama.cpp integration, and GPU VRAM dispatch.
3. **[03-DISTRIBUTED-RDMA-EBPF-CLUSTER-RING.md](./03-DISTRIBUTED-RDMA-EBPF-CLUSTER-RING.md)** — Sub-microsecond distributed cluster ring, RoCEv2 one-sided writes, eBPF packet routing, and split-brain recovery.
4. **[04-HEDDLE2-UNIFIED-HOT-PLANE-UI.md](./04-HEDDLE2-UNIFIED-HOT-PLANE-UI.md)** — High-frequency UI streaming, zero-re-render DOM/Metal bindings, and tear-free lockless multi-producer HUD displays.

### Next-Generation Pillars (5 – 8)
5. **[NEXT-GEN-PILLARS-5-8.md](./NEXT-GEN-PILLARS-5-8.md)** — Comprehensive master plan for:
   - **Pillar 5: `weft-spectrum`** (Universal Hardware-Adaptive Matrix for Qualcomm, MediaTek, Apple, Nvidia, Intel, AMD, ARM, RISC-V)
   - **Pillar 6: `weft-adapters`** (Plug-and-play drop-in enterprise boosters for FinTech SBE/ITCH, Robotics ROS2/DDS `rmw_weft`, Edge Vision DMA)
   - **Pillar 7: `weft-studio`** (Lightweight dedicated IDE, Language Server, Live Cache-Line & Seqlock Visualizer, Time-Travel Debugger)
   - **Pillar 8: `weft-verify`** (Static zero-allocation compiler linter `weftc --lint-alloc`, TLA+ cluster proofs, Synthetic Silicon & Jitter Lab)

