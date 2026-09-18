// gpu_stream.h — zero-copy GPU streaming kit over a gpu_ring session
// (RFC-0013, Series 8).
//
// WHY EXISTS: RFC-0003's probe hand-rolled an entire Vulkan compute pipeline
// (descriptor layout, pool, pipeline, command buffer) inside a proof tool.
// Production consumers — render passes rasterizing frames, compute passes
// transforming them — need the SAME plumbing without copying it. This kit is
// that plumbing, driver-layer shaped, with the session-ring binding
// convention frozen:
//
//   binding 0 (std430 SSBO, readonly): the session span as u32 words
//   binding 1 (std430 SSBO):          result[8] u32 words (32 B, mapped)
//   binding 2 (rgba8ui image):        optional w*h storage texture
//   binding 3 (R32_UINT texel view):  optional texel buffer over the span —
//                                     the "direct texture" road: the SAME
//                                     device allocation sampled as a texture
//                                     (usamplerBuffer texelFetch), no copy
//
// WHAT A CONSUMER DOES: weft_gpu_stream_init(...) once; publish frames CPU
// side through the ordinary fan-out API; weft_gpu_stream_dispatch(...) per
// GPU pass; read weft_gpu_stream_result()[0..8) — mapped HOST_COHERENT
// memory, visible without barriers after the internal DeviceWaitIdle.
//
// ZERO-COPY CONTRACT: the ring binding is the ring's OWN VkBuffer (the CPU's
// mapped pointer and the GPU's SSBO alias one allocation — RFC-0003's claim,
// now executable). The result buffer is the only allocation this kit adds.
// The image is GPU-side state (its pixels never round-trip to the CPU in the
// streaming path; the self-verifying rasterizer proves them in-shader).
//
// FALLBACK DISCIPLINE (the guardrail): init returns
// WEFT_GPU_STREAM_ERR_NO_VULKAN on CPU/Metal backends — callers route to
// their CPU path, exactly like gpu_ring's own backend probe. Every Vulkan
// failure is a distinct error code; nothing crashes, nothing degrades
// silently. The kit is legal on any queue family the ring already chose.
//
// Layer discipline: driver layer; weft.c/weft.h untouched; Vulkan surface
// stays inside the implementation (vk_min.h internals, not this header).

#ifndef WEFT_GPU_STREAM_H
#define WEFT_GPU_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "gpu_ring.h"

typedef struct weft_gpu_stream weft_gpu_stream_t;

/// Kit error codes (distinct failures, no silent degradation).
typedef enum {
    WEFT_GPU_STREAM_OK = 0,
    WEFT_GPU_STREAM_ERR_NO_VULKAN = -1,   ///< ring backend is CPU/Metal
    WEFT_GPU_STREAM_ERR_BAD_ARG = -2,     ///< NULL args / empty SPIR-V
    WEFT_GPU_STREAM_ERR_SHADER = -3,      ///< vkCreateShaderModule failed
    WEFT_GPU_STREAM_ERR_PIPELINE = -4,    ///< pipeline create failed/NULL
    WEFT_GPU_STREAM_ERR_MEMORY = -5,      ///< result/image allocation failed
    WEFT_GPU_STREAM_ERR_DESCRIPTOR = -6,  ///< descriptor plumbing failed
    WEFT_GPU_STREAM_ERR_COMMAND = -7,     ///< command pool/buffer failed
    WEFT_GPU_STREAM_ERR_SUBMIT = -8,      ///< record/submit/wait failed
    WEFT_GPU_STREAM_ERR_IMAGE = -9,       ///< image/view creation failed
    WEFT_GPU_STREAM_ERR_TEXEL = -10,      ///< texel buffer view failed
} weft_gpu_stream_err_t;

/// Build the pipeline. spv/spv_bytes: the consumer's SPIR-V (the kit does
/// not own shaders — probes/compute/*.comp are the in-tree consumers).
/// img_w/img_h: storage-image geometry, (0,0) disables binding 2.
/// want_texel: non-zero binds the session span as an R32_UINT texel buffer
/// view at binding 3 (the ring must have been created on a backend that
/// created its buffer with texel usage — gpu_ring tries, and the kit
/// reports WEFT_GPU_STREAM_ERR_TEXEL when the ICD refuses the view).
/// May allocate (creation path; the streaming path never does).
weft_gpu_stream_err_t weft_gpu_stream_init(weft_gpu_stream_t** out,
                                           weft_gpu_ring_t* g,
                                           const void* spv, size_t spv_bytes,
                                           unsigned img_w, unsigned img_h,
                                           int want_texel);

/// One GPU pass over the live ring. push/push_bytes: push constants
/// (max 16 bytes; the in-tree shaders use 8 or 16). gx/gy/gz: workgroup
/// counts. Returns WEFT_GPU_STREAM_OK after a completed DeviceWaitIdle —
/// the mapped result is stable to read until the next dispatch.
weft_gpu_stream_err_t weft_gpu_stream_dispatch(weft_gpu_stream_t* s,
                                               const void* push,
                                               unsigned push_bytes,
                                               unsigned gx, unsigned gy,
                                               unsigned gz);

/// The mapped result words (8 u32). NULL before the first successful
/// dispatch. Points at kit-owned memory — do not free; valid until destroy.
const uint32_t* weft_gpu_stream_result(const weft_gpu_stream_t* s);

/// Release everything the kit created (the ring is NOT touched). NULL-safe.
void weft_gpu_stream_destroy(weft_gpu_stream_t* s);

#endif // WEFT_GPU_STREAM_H
