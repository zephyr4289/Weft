# probes/metal — RFC-0013 Metal road (Apple unified memory)

`stream_frames.metal` is the MSL reference consumer for the RFC-0013
session-ring word map (the mirror of `probes/compute/stream_frames.comp`).

## The zero-copy wrap (Apple-side glue)

The Weft session span (`weft_gpu_ring_bytes(g) - 64` is the ring; the FULL
span including the 64-byte WFSH header is what the GPU consumes) is
anonymous `MAP_SHARED` memory under the `METAL` backend of
`core/c/gpu_ring.c`. On Apple silicon, wrap it with **no copy**:

```swift
let g = try weft_gpu_create(payload: 256, slots: 4)   // METAL backend on Apple
let span = weft_gpu_ring_span(g)
let base = weft_gpu_ring_bytes(g) - 64                // span base (header start)

let ring = device.makeBuffer(bytesNoCopy: UnsafeRawPointer(base)!,
                             length: span,
                             options: .storageModeShared)!
// ring and the CPU's fan-out pointer alias ONE allocation — zero copy.
```

`newBufferWithBytesNoCopy` requires page-aligned, page-multiple lengths —
the session span is `posix_memalign`-friendly by construction (round the
wrap up to the page boundary; the tail is never read by the shader).

Dispatch:

```swift
let pipe = try device.makeComputePipelineState(
    function: lib.makeFunction(name: "stream_frames")!)
let enc = cmdBuf.computeCommandEncoder()
enc.setBuffer(ring, offset: 0, index: 0)
enc.setBuffer(result, offset: 0, index: 1)
enc.setBytes(&geo, length: MemoryLayout<uint2>.size, index: 2)
enc.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                         threadsPerThreadgroup: MTLSize(width: slots, height: 1, depth: 1))
enc.endEncoding()
cmdBuf.commit(); cmdBuf.waitUntilCompleted()
```

## Honesty boundary (Law 4)

- The MSL kernel is the **reference contract**; it is not compile-tested in
  the x86_64 sandbox (no Metal toolchain exists off Apple hardware).
- The apple CI leg can gate compilation with:
  `xcrun metal -c probes/metal/stream_frames.metal -o /dev/null`
  (proposed as an optional apple-packages step; adding it to that workflow
  is a reviewer call — it is declared here rather than silently assumed).
- No Apple-silicon performance numbers are claimed anywhere. The claim is
  structural: unified memory + `newBufferWithBytesNoCopy` = the same
  zero-copy aliasing the Vulkan road proves executably
  (`litmus/evidence/gpu-ring/series8-zero-copy-streaming.log`).

## simdgroup-reduction variant

The committed kernel writes the result from thread 0 (the review-symmetric
shape of the GLSL reference). For wide slot counts, the Metal-idiomatic
reduction is `simd_reduce_add` / `simd_reduce_xor` over the per-thread
`(count, xor)` pairs — offered as an exercise for the integration, not
claimed here.
