# 🏛️ Pillar 2: `weft-tensor` Realtime Edge AI & Zero-Copy Tensor Fabric

> **Target Paradigm:** Obliterate Python/C++ PyTorch staging, ONNX staging copies, and multi-hop GPU transfers.  
> **Core Metric:** **< 1.0 ms End-to-End Latency** (Sensor DMA $\to$ NPU/LLM $\to$ GPU VRAM $\to$ 120 FPS UI).

---

## 1. Problem Statement & Motivation

Edge AI applications (robotics, AR glasses, camera vision, local LLM voice assistants) are severely bottlenecked by data-transfer hops:
1. Camera sensor writes image bytes into kernel V4L2 / AVFoundation buffer.
2. User-space application copies image into heap memory.
3. Python / C++ runtime copies and formats data into a PyTorch/ONNX Tensor.
4. Tensor data is copied over PCIe to GPU / NPU memory.
5. Inference results are copied back to host memory, parsed into UI models, and re-rendered.

**The `weft-tensor` Solution:**
A unified hardware-accelerated zero-copy pipeline that connects camera/mic DMA rings directly into unified memory tensor arenas accessible by Apple Neural Engine, NVIDIA TensorRT, Qualcomm NPU, and WebGPU compute shaders without a single intermediate `memcpy`.

---

## 2. Pipeline Architecture

```
┌─────────────────┐       DMA Lock-Free Ring Buffer
│ Camera / Sensor │ ──────────────────────────────────────┐
│ (Hardware DMA)  │                                       │
└─────────────────┘                                       ▼
                                             ┌───────────────────────────┐
┌─────────────────┐   Shared Memory Arenas   │   WEFT TENSOR FABRIC      │
│ Audio / Sensors │ ───────────────────────► │ (Unified Memory Page Map) │
└─────────────────┘                          └─────────────┬─────────────┘
                                                           │
                      ┌────────────────────────────────────┼────────────────────────────────────┐
                      ▼                                    ▼                                    ▼
       ┌─────────────────────────────┐      ┌─────────────────────────────┐      ┌─────────────────────────────┐
       │ Apple NPU / CoreML Engine   │      │ ONNX Runtime / Llama.cpp    │      │ GPU VRAM Compute Pipeline   │
       │ (Zero-copy CVPixelBuffer)   │      │ (Direct tensor pointer)     │      │ (Direct Vulkan/Metal UBO)   │
       └──────────────┬──────────────┘      └──────────────┬──────────────┘      └──────────────┬──────────────┘
                      │                                    │                                    │
                      └────────────────────────────────────┼────────────────────────────────────┘
                                                           ▼
                                            ┌─────────────────────────────┐
                                            │ 120/240 FPS Tear-Free UI    │
                                            │ (Heddle Reactive Plane)     │
                                            └─────────────────────────────┘
```

---

## 3. Key Technological Innovations

1. **Lock-Free DMA Tensor Ring:** Camera/Audio hardware writes frames directly into Weft ring buffers mapped with `PROT_READ | PROT_WRITE` across processes.
2. **Unified Strided Tensor Descriptors:** A `weft_tensor_view_t` struct describing dimensions, strides, datatype (`f16`, `bf16`, `int8`, `f32`), and physical memory address.
3. **Hardware Engine Adapters:**
   - **Apple Silicon:** Direct wrapping into `IOSurface` and `CVPixelBuffer` for 0-copy ANE/Metal acceleration.
   - **Linux / Android:** Direct `dma-buf` export/import between V4L2 and Vulkan/OpenCL.
   - **Embedded / RISC-V:** Direct fixed-address SRAM buffers for micro-NPUs.
4. **Sub-Millisecond Execution:** Streaming audio and video tokens are fed into ONNX / Llama.cpp inference loops with zero preprocessing overhead.

---

## 4. Work Distribution (Swarm-by-Layer)

- **Senior Engineer 1 (Core DMA & Tensor Ring Architecture):**
  - Lock-free circular DMA buffer management and page-aligned tensor allocators.
  - Zero-copy strided tensor slice and transpose algorithms.
- **Senior Engineer 2 (Hardware Accelerators & NPU/GPU Kernels):**
  - Apple Metal / IOSurface and Linux `dma-buf` bindings.
  - ONNX Runtime and Llama.cpp zero-copy execution hooks.
- **Senior Engineer 3 (Realtime Application & UI Streaming):**
  - WebRTC / Audio streaming ingestion interfaces.
  - Realtime streaming visualizer for React, Flutter, and Swift.
