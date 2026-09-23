// weft_metal_bridge.h — RFC-0017 §4: the Apple unified-memory adapter.
//
// WHY EXISTS: the lead's Pillar-2 mandate — "Directly wrap a
// weft_tensor_view_t into an IOSurfaceRef / CVPixelBufferRef without
// memory copies, enabling Apple Neural Engine (CoreML) and Metal
// Performance Shaders (MPSGraph) to read camera frames and audio
// spectrograms directly from Weft's DMA memory," plus
// newBufferWithBytesNoCopy Metal compute dispatch. On Apple Silicon the
// CPU's RAM IS the GPU's RAM (and the ANE's), so zero-copy is not an
// import dance but a WRAP dance — the exact stance Series 10's
// WeftMetalZeroCopy.swift established for ring bytes; this adapter
// generalizes it to tensor views:
//
//   ┌ weft_tensor_view_t ─────────────────────────────────────────┐
//   │  wrap_mtlbuffer     → id<MTLBuffer>  (bytesNoCopy, shared)  │
//   │                     → instant Metal compute dispatch         │
//   │  wrap_cvpixelbuffer → CVPixelBufferRef (zero-copy, release-  │
//   │                       callback keyed to the view's epoch)    │
//   │                     → CoreML / Vision / ANE model input      │
//   │  iosurface_span     → IOSurfaceRef + its OWN bytes (reverse  │
//   │                       ownership: the ring allocates FROM the │
//   │                       surface, so ANE consumes ring memory)  │
//   └──────────────────────────────────────────────────────────────┘
//
// WHAT RUNS WHERE (the honesty map — Law 4): the geometry core, the
// refusal ladder, and the probe compile and are TESTED everywhere
// (weft_metal_core.c + the AC-M gates). The device roads are ObjC++
// (weft_metal_apple.mm) compiled ONLY on __APPLE__ — the apple CI leg
// (the WeftMetalZeroCopy.swift / gpu_ring METAL precedent); on this
// x86_64 sandbox they are link-time WEAK stubs that return honest
// UNSUPPORTED refusals. Every device claim is DECLARED here, MEASURED
// on the apple CI leg.
//
// DISCRETE-GPU MACS: the same Law-4 stance the Swift bridge takes —
// wrap REFUSES (never silently stages) when the device cannot
// dereference shared storage for compute.
//
// LAW 1: the pool (pipeline states, command buffers) is created once;
//        dispatch reuses it — the hot path allocates nothing.
// LAW 2: geometry_for_view is the gate (16-byte float alignment,
//        bytesPerRow 64-aligned, LE); a refusing view routes to
//        [FALLBACK-COPY] (the SIMD road), never a misaligned wrap.
// LAW 3: modular adapter (backends/metal); core/c untouched; the
//        frozen MSL kernel mirrors the GLSL/WGSL pair byte-for-byte
//        in BEHAVIOR (the same one-multiply normalize contract).
// LAW 4: weak-stub refusals are the non-Apple truth; nothing pretends.

#ifndef WEFT_METAL_BRIDGE_H
#define WEFT_METAL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "weft/weft_tensor_view.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability probe
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_METAL_UNSUPPORTED = 0,  ///< non-Apple (the weak stub's truth)
    WEFT_METAL_UNIFIED = 1,      ///< Apple Silicon — shared storage IS
                                 ///< GPU/ANE-visible (the zero-copy road)
    WEFT_METAL_DISCRETE = 2,     ///< discrete Mac — wraps REFUSE (the
                                 ///< documented no-staging boundary)
} weft_metal_caps_t;

weft_metal_caps_t weft_metal_probe(void);
const char*       weft_metal_caps_name(weft_metal_caps_t c);

// ---------------------------------------------------------------------------
// Geometry (portable, pure, tested everywhere)
// ---------------------------------------------------------------------------

/// The wrap geometry for a view: derived, validated, and 64-aligned.
typedef struct {
    uint32_t width;             ///< pixels (dims[rank-1] / 4 for RGBA8)
    uint32_t height;            ///< rows (dims[rank-2], 1 when rank < 2)
    uint32_t bytes_per_element; ///< 4 (RGBA8 pixel) or elem_size (f32)
    uint32_t bytes_per_row;     ///< width * bytes_per_element, rounded UP
                                ///< to 64 (IOSurface/CVPixelBuffer want it)
    uint64_t span_bytes;        ///< bytes_per_row * height (the wrap extent)
    weft_tv_err_t view_err;     ///< the ladder's verdict on the view
} weft_metal_geometry_t;

/// Compute + validate the wrap geometry. The view must be PACKED, LE,
/// u8 with 4-byte rows (the RGBA8 camera road) or F32 (the
/// spectrogram/plane road); rows must be >= 16 bytes and the data
/// pointer 16-byte aligned (Law 2). Returns 0 with *out filled; -1 with
/// out->view_err naming the refusal (adapters route to [FALLBACK-COPY]).
int weft_metal_geometry_for_view(const weft_tensor_view_t* v,
                                 weft_metal_geometry_t* out);

// ---------------------------------------------------------------------------
// Device roads (Apple: weft_metal_apple.mm; elsewhere: weak refusals)
// ---------------------------------------------------------------------------

/// Wrap a view's bytes in a zero-copy MTLBuffer (bytesNoCopy +
/// storageModeShared). The caller owns the memory and must keep it
/// mapped for the buffer's lifetime (the Series-10 contract). Returns 0
/// and an owned handle (weft_metal_release); -1 on every refusal
/// (non-Apple, misaligned, undersized, discrete-only device).
int weft_metal_wrap_mtlbuffer(const weft_tensor_view_t* v, void** out_buffer);

/// Wrap a view in a zero-copy CVPixelBufferRef — the CoreML / Vision /
/// ANE model-input road (CVPixelBufferCreateWithBytes with a release
/// callback; the buffer never copies and never outlives the memory).
/// RGBA8 views map to kCVPixelFormatType_32RGBA; F32 4-channel views
/// map to kCVPixelFormatType_128RGBAFloat (the spectrogram road).
int weft_metal_wrap_cvpixelbuffer(const weft_tensor_view_t* v,
                                  void** out_pb);

/// REVERSE OWNERSHIP: allocate span bytes FROM an IOSurface (the ring
/// allocates its slots inside the surface's bytes — ANE/CoreML consumes
/// the ring's OWN memory through the surface handle). Returns the owned
/// surface handle + the CPU pointer of its bytes + the span size.
int weft_metal_iosurface_span(uint32_t width, uint32_t height,
                              uint32_t bytes_per_element, void** out_surface,
                              void** out_bytes, uint64_t* out_span);

/// Release any owned handle (MTLBuffer/CVPixelBuffer/IOSurface/pool).
/// NULL-safe.
void weft_metal_release(void* handle);

// ---------------------------------------------------------------------------
// Pooled compute (frozen kernel; Law 1)
// ---------------------------------------------------------------------------

/// The frozen preprocess push block — layout-identical to the GLSL/WGSL
/// pair (the one-multiply normalize contract, bit-exact by construction).
typedef struct {
    uint32_t src_word_off;
    uint32_t dst_elem_off;
    uint32_t n_pixels;
    float    scale;
} weft_metal_preprocess_push_t;

/// Build the pipeline ONCE from MSL source (compiled by device.makeLibrary
/// at pool creation; the frozen-ID check runs BEFORE compilation — Law 3).
/// Returns a pool handle; -1 on refusal (non-Apple, shader compile error,
/// frozen-ID mismatch).
int weft_metal_pool_create(void** out_pool, const char* msl_source,
                           size_t msl_len, uint64_t frozen_id_expect);

/// One dispatch: binds src/dst MTLBuffers at byte offsets, runs the
/// frozen kernel with the push block, waits for completion. Zero
/// allocations (the command buffer comes from the pool's reuse ring).
int weft_metal_pool_preprocess(void* pool, void* src_buffer,
                               uint64_t src_byte_off, void* dst_buffer,
                               uint64_t dst_byte_off,
                               const weft_metal_preprocess_push_t* push);

/// The committed MSL kernel source (shaders/weft_preprocess.metal).
const char* weft_metal_preprocess_msl(size_t* out_len);
/// FNV-1a of the committed MSL (the expected frozen ID).
uint64_t weft_metal_preprocess_frozen_id(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_METAL_BRIDGE_H
