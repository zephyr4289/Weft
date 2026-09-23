// weft_vk_bridge.h — RFC-0017 §3: the Vulkan dma-buf compute bridge.
//
// WHY EXISTS: the lead's Pillar-2 mandate — "Import dma-buf file
// descriptors from Linux V4L2 / Android camera HAL directly into Vulkan
// VkDeviceMemory without CPU intervention" and dispatch preprocessing
// "entirely on GPU compute shaders directly inside the shared buffer."
// Series 10 (RFC-0016 §2) built the wrap constructors for WFSH SESSION
// memory; this bridge extends the same zero-copy stance to RAW foreign
// buffers (a camera frame, a codec output, a NIC UMEM chunk — anything
// that arrives as a dma-buf fd or a page-aligned shared mapping) and
// packages the pooled compute dispatch every accelerator road needs:
//
//   ROAD A (ring session)   weft_vk_ring_wrap_host / _fd
//     A WFSH ring session (the DMA tensor ring) becomes GPU-consumable
//     through the SUBSTRATE's alias-verified wrap constructors
//     (weft_gpu_wrap_host / weft_gpu_wrap_dmabuf, RFC-0016 §2) — this
//     bridge only adds the device view + the compute kit on top.
//
//   ROAD B (foreign buffer) weft_vk_import_fd / weft_vk_import_host
//     Raw device memory that is NOT a Weft session — the camera road.
//     Own dlopen'd bootstrap (the audited gpu_ring.c wrap pattern:
//     loader → compute-queue device with external-memory extensions →
//     import → alias canary). A device that accepts an import but backs
//     it with fresh memory (llvmpipe 25.0.7's host-road behavior — the
//     Series-10 discovery) is REFUSED by the canary, never silently
//     consumed.
//
//   THE COMPUTE KIT        weft_vk_compute_*
//     Pooled dispatch over any device handle: N storage-buffer bindings
//     (each bindable at a byte sub-range of an existing VkBuffer — a
//     ring SLOT becomes an SSBO), push constants, one pre-allocated
//     command buffer + fence (Law 1: the hot path records and submits,
//     it never creates). The frozen preprocess kernel
//     (shaders/weft_preprocess.{comp,spv}) implements the pillar's
//     normalize contract: out[i] = (f32)src[i] * scale — bit-exact
//     against the SIMD/scalar oracle by construction (one exact
//     conversion + one rounded multiply; no add to fuse into an FMA).
//
// LAW 1: ctx/import/compute_init allocate at SETUP; dispatch binds,
//        records, submits — zero creates, zero mallocs (pooled cmd
//        buffer + fence, descriptor sets allocated once).
// LAW 2: every bind path runs weft_tensor_view_gpu_ready() first; a
//        misaligned/BE view routes to [FALLBACK-COPY], never a silent
//        misaligned SSBO bind.
// LAW 3: tools layer (backends/vulkan); core/c untouched; no link-time
//        Vulkan dependency (dlopen, the kernel's no-new-library rule at
//        the DSO boundary — the gpu_ring discipline).
// LAW 4: every refusal is a distinct error code; the alias canary is a
//        LOAD-BEARING gate (a non-aliasing device is refused); probe
//        results (extensions, device name, min alignment) are reported.

#ifndef WEFT_VK_BRIDGE_H
#define WEFT_VK_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "gpu_ring.h"   // Road A: the substrate session type
#include "weft/weft_tensor_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_VK_OK = 0,
    WEFT_VK_ERR_NO_LOADER = -1,     ///< no libvulkan.so.1 / .so at runtime
    WEFT_VK_ERR_NO_DEVICE = -2,     ///< no compute-queue device w/ needed ext
    WEFT_VK_ERR_BAD_ARG = -3,       ///< NULL args / zero sizes
    WEFT_VK_ERR_ALIGN = -4,         ///< pointer below minImportedHostPointer
                                    ///< alignment (malloc'd memory — the
                                    ///< documented shm/dmabuf road)
    WEFT_VK_ERR_IMPORT = -5,        ///< vkAllocateMemory/import refused
    WEFT_VK_ERR_MEMORY_TYPE = -6,   ///< no HOST_VISIBLE|COHERENT type for
                                    ///< the imported requirements
    WEFT_VK_ERR_BUFFER = -7,        ///< buffer create/bind refused
    WEFT_VK_ERR_SHADER = -8,        ///< shader module create refused
    WEFT_VK_ERR_FROZEN_ID = -9,     ///< kernel bytes fail the frozen-ID
                                    ///< check (wrong/old shader — Law 3)
    WEFT_VK_ERR_PIPELINE = -10,     ///< pipeline/descriptor plumbing
    WEFT_VK_ERR_COMMAND = -11,      ///< command pool/buffer/submit
    WEFT_VK_ERR_ALIAS = -12,        ///< alias canary failed — the device
                                    ///< does NOT carry the producer's
                                    ///< bytes (fresh-memory backing)
    WEFT_VK_ERR_VIEW = -13,         ///< the tensor view refused the ladder
    WEFT_VK_ERR_SESSION = -14,      ///< Road A: WFSH session invalid
} weft_vk_err_t;

const char* weft_vk_err_name(weft_vk_err_t e);

// ---------------------------------------------------------------------------
// The device view (what the compute kit runs on — a ctx device OR a
// wrapped ring's device; Vulkan objects are device-scoped, so the kit
// takes the handle it will actually build pipelines on)
// ---------------------------------------------------------------------------

typedef struct weft_vk_dev {
    void* device;                        ///< VkDevice (opaque)
    void* queue;                         ///< VkQueue  (opaque)
    uint32_t queue_family;               ///< the queue's family (command
                                         ///< pools are per-family — the
                                         ///< spec's cross-family refusal)
    const void* (*proc)(void* user, const char* name);  ///< resolver
    void* proc_user;                     ///< resolver argument
} weft_vk_dev_t;

// ---------------------------------------------------------------------------
// ROAD B context — standalone device bootstrap (the camera/foreign road)
// ---------------------------------------------------------------------------

typedef struct weft_vk_ctx weft_vk_ctx_t;

/// Boot: dlopen the loader, create an instance, pick the first physical
/// device exposing a compute queue family AND the external-memory
/// extensions (VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf
/// for the fd road; VK_EXT_external_memory_host recorded when present),
/// create the logical device. May allocate (setup path). NULL out on
/// every refusal with the reason in the error code.
weft_vk_err_t weft_vk_ctx_create(weft_vk_ctx_t** out, const char* app_tag);

/// Evidence accessors (advisory — AXIOM T; every claim carries them).
const char*        weft_vk_ctx_device_name(const weft_vk_ctx_t* c);
int                weft_vk_ctx_has_dmabuf_ext(const weft_vk_ctx_t* c);
int                weft_vk_ctx_has_host_ext(const weft_vk_ctx_t* c);
uint64_t           weft_vk_ctx_min_host_align(const weft_vk_ctx_t* c);
const weft_vk_dev_t* weft_vk_ctx_dev(const weft_vk_ctx_t* c);

/// Release everything the ctx created. NULL-safe.
void weft_vk_ctx_destroy(weft_vk_ctx_t* c);

// ---------------------------------------------------------------------------
// ROAD B memory — foreign imports + output allocations
// ---------------------------------------------------------------------------

typedef struct {
    void*    buffer;        ///< VkBuffer over the imported span
    void*    memory;        ///< VkDeviceMemory (the import itself)
    void*    map;           ///< CPU mapping (HOST_VISIBLE imports only)
    uint64_t bytes;         ///< the span's byte extent
    int      alias_verified;///< 1 = canary proven (the load-bearing gate)
} weft_vk_mem_t;

/// Import a dma-buf fd (V4L2 EXPBUF / AHB handle / heap allocation /
/// another GPU's export). The fd is BORROWED — never closed here. The
/// import is HOST_VISIBLE|HOST_COHERENT (llvmpipe/integrated reality;
/// discrete-GPU device-local imports are the alloc_output road). Sets
/// alias_verified=0 — run weft_vk_alias_verify() before trusting it.
weft_vk_err_t weft_vk_import_fd(weft_vk_ctx_t* c, int fd, uint64_t bytes,
                                weft_vk_mem_t* out);

/// Import a page-aligned host mapping (the shm/memfd road —
/// VK_EXT_external_memory_host). BORROWED pointer; the caller keeps it
/// mapped. Alignment below the device's minimum refuses (WEFT_VK_ERR_ALIGN
/// — malloc'd rings are NOT importable, the documented boundary).
weft_vk_err_t weft_vk_import_host(weft_vk_ctx_t* c, void* ptr,
                                  uint64_t bytes, weft_vk_mem_t* out);

/// Allocate an output buffer on this device (host_visible!=0 maps it —
/// the write-back road for producers that read results CPU-side).
weft_vk_err_t weft_vk_alloc_output(weft_vk_ctx_t* c, uint64_t bytes,
                                   int host_visible, weft_vk_mem_t* out);

void* weft_vk_mem_map(weft_vk_mem_t* m);      ///< NULL when not mappable
void  weft_vk_mem_unmap(weft_vk_ctx_t* c, weft_vk_mem_t* m);
void  weft_vk_mem_free(weft_vk_ctx_t* c, weft_vk_mem_t* m);

// ---------------------------------------------------------------------------
// ROAD A — wrap a WFSH ring session (the DMA tensor ring) through the
// Series-10 substrate constructors, expose its device for the compute kit
// ---------------------------------------------------------------------------

typedef struct weft_vk_ring weft_vk_ring_t;

/// Adopt a NATIVE weft_gpu_ring session (weft_gpu_create/_ex — the
/// allocation already lives on a Vulkan device; the CPU mapping and the
/// GPU buffer alias one allocation by construction, the substrate's own
/// proven claim). The ring is BORROWED: destroy frees only the wrapper.
weft_vk_err_t weft_vk_ring_of_gpu_session(weft_vk_ring_t** out,
                                          weft_gpu_ring_t* g);

/// Host road: wrap an existing page-aligned session mapping (the ring's
/// CPU owner keeps it mapped). Alias-verified by the substrate.
weft_vk_err_t weft_vk_ring_wrap_host(weft_vk_ring_t** out,
                                     size_t payload_bytes,
                                     unsigned slot_count, void* session_mem,
                                     size_t map_bytes);

/// dma-buf road: wrap a session carrying fd (weft_dmabuf storage or a
/// peer's export). Alias-verified by the substrate.
weft_vk_err_t weft_vk_ring_wrap_fd(weft_vk_ring_t** out,
                                   size_t payload_bytes,
                                   unsigned slot_count, int fd);

/// The wrapped session's device view — pass to weft_vk_compute_init.
const weft_vk_dev_t* weft_vk_ring_dev(const weft_vk_ring_t* r);
/// The wrapped session's span buffer (VkBuffer) + its bytes — bind slots
/// out of this at byte offsets (slot k payload at
/// 64 + 16 + 8*M + k*payload_bytes... use weft_vk_ring_slot_range).
void*    weft_vk_ring_buffer(const weft_vk_ring_t* r);
uint64_t weft_vk_ring_buffer_bytes(const weft_vk_ring_t* r);
/// Byte range of slot k's PAYLOAD inside the span buffer (the RFC-0004
/// ring layout, offsets identical to the substrate's documented contract).
void weft_vk_ring_slot_range(const weft_vk_ring_t* r, unsigned slot,
                             uint64_t* off, uint64_t* len);
/// Release the wrap (the session memory/fd stays the caller's). NULL-safe.
void weft_vk_ring_destroy(weft_vk_ring_t* r);

// ---------------------------------------------------------------------------
// The pooled compute kit (any device; zero creates on the hot path)
// ---------------------------------------------------------------------------

typedef struct weft_vk_compute weft_vk_compute_t;

/// Build the pipeline: n_bindings storage buffers (binding i = the i-th
/// SSBO the shader declares), push_bytes of push constants (<= 128).
/// frozen_id_expect != 0 verifies FNV-1a(spv) == expected BEFORE the
/// module is created (Law 3: the wrong/old kernel refuses, never runs).
weft_vk_err_t weft_vk_compute_init(weft_vk_compute_t** out,
                                   const weft_vk_dev_t* dev,
                                   const void* spv, uint32_t spv_words,
                                   uint64_t frozen_id_expect,
                                   uint32_t n_bindings,
                                   uint32_t push_bytes);

/// (Re)bind binding i to a byte sub-range of a VkBuffer (a weft_vk_mem_t's
/// buffer, a wrapped ring's span buffer — anything on THIS device).
/// Records nothing yet — the bind table is consumed by dispatch.
weft_vk_err_t weft_vk_compute_bind(weft_vk_compute_t* k, uint32_t binding,
                                   void* vk_buffer, uint64_t offset,
                                   uint64_t range);

/// Submit: push constants + the bind table + (gx,gy,gz) workgroups.
/// Records the pooled command buffer, submits, waits the pooled fence —
/// zero allocations, zero creates (Law 1). Returns WEFT_VK_OK when the
/// GPU is done (results stable to read).
weft_vk_err_t weft_vk_compute_dispatch(weft_vk_compute_t* k,
                                       const void* push,
                                       uint32_t gx, uint32_t gy, uint32_t gz);

void weft_vk_compute_destroy(weft_vk_compute_t* k);  ///< NULL-safe

// ---------------------------------------------------------------------------
// The alias canary (Law 4's load-bearing gate — the Series-10 lesson)
// ---------------------------------------------------------------------------

/// Prove the device carries the producer's OWN bytes: writes a 16-word
/// canary through `producer_ptr` (the CPU side of the SAME memory the
/// import bound), dispatches the frozen preprocess kernel over it, and
/// compares the GPU's output against the scalar oracle BIT-EXACTLY.
/// Returns WEFT_VK_OK + sets mem->alias_verified=1 on proof;
/// WEFT_VK_ERR_ALIAS when the device backs the import with fresh memory.
/// The kit is expected to be built from weft_preprocess.spv with the
/// canary's binding geometry (binding 0 = the import, binding 1 = a
/// 256-byte mappable output allocated by the canary itself).
weft_vk_err_t weft_vk_alias_verify(weft_vk_ctx_t* c, weft_vk_mem_t* mem,
                                   weft_vk_compute_t* k);

// ---------------------------------------------------------------------------
// The frozen preprocess kernel (embedded, committed byte-identical — CI
// rebuilds and diffs the .spv; the runtime frozen-ID check guards caller
// kernels; together: kernel freeze as a gate, not a promise)
// ---------------------------------------------------------------------------

/// Push layout for weft_preprocess.comp (16 bytes — keep in sync with the
/// shader; the shader is the frozen artifact).
typedef struct {
    uint32_t src_word_off;  ///< u32 word offset of the RGBA8 frame in binding 0
    uint32_t dst_elem_off;  ///< f32 element offset in binding 1
    uint32_t n_pixels;      ///< pixels to convert (4 f32 out each)
    float    scale;         ///< the normalize contract's single multiply
} weft_vk_preprocess_push_t;

/// The committed SPIR-V (weft_preprocess.spv) as a word array.
const uint32_t* weft_vk_preprocess_spv(uint32_t* out_words);
/// FNV-1a of the committed SPIR-V bytes (the expected frozen ID).
uint64_t weft_vk_preprocess_frozen_id(void);

/// One-call preprocess over bound memories: validates the view (Law 2),
/// computes workgroup counts, fills the push block, dispatches.
/// `src` binds at binding 0 (its buffer + byte offset), `dst` at
/// binding 1. dst must be f32[4*n_pixels]-spaced.
weft_vk_err_t weft_vk_preprocess_dispatch(weft_vk_compute_t* k,
                                          void* src_buffer,
                                          uint64_t src_byte_off,
                                          void* dst_buffer,
                                          uint64_t dst_byte_off,
                                          const weft_tensor_view_t* src_rgba8,
                                          float scale);

#ifdef __cplusplus
}
#endif

#endif // WEFT_VK_BRIDGE_H
