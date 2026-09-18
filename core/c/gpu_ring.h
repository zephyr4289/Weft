// gpu_ring.h — GPU-resident zero-copy rings (RFC-0003, Triad-2 driver layer).
//
// WHY EXISTS: RFC-0003 (Triad-2 GPU-Resident Mode) is Draft / "design
// exploration accepted, pending hardware-backed spike". This module IS
// that spike, driver-layer shaped: the RFC-0004 fan-out ring allocated in
// memory a GPU can dereference DIRECTLY — Vulkan memory with
// VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT (persistently mapped: the CPU's ring
// pointer and the GPU's storage-buffer binding alias the SAME device
// allocation, so a compute shader consumes live Weft frames with NO
// staging copy and NO vkCmdCopyBuffer anywhere in the path) — plus the
// Apple-unified-memory road (compile-gated) and an honest CPU fallback.
//
// The byte layout is the SAME session protocol as shm_ring.h (WFSH header
// + RFC-0004 ring): a producer that speaks shm_ring speaks gpu_ring, and
// the probe (core/c/gpu_probe.c + probes/compute/validate_frame.comp)
// proves the zero-copy claim end-to-end — the GPU-side shader validates
// frame payload words against the 04-LITMUS mixer family by reading the
// mapped ring words directly.
//
// BACKENDS (resolved in order, reported honestly):
//   VULKAN — dlopen'd loader (NO link-time dependency; the kernel's
//            no-new-library rule kept at the DSO boundary), first
//            compute-capable queue family, HOST_VISIBLE|HOST_COHERENT
//            memory, persistent map. Requires a Vulkan ICD at runtime
//            (any: discrete, integrated, or lavapipe software — the
//            evidence log names which).
//   METAL  — Apple unified memory: the mapping IS the same RAM the GPU
//            reads (MTLResourceStorageModeShared semantics); compile-
//            guarded __APPLE__, compiled by the apple CI leg, NOT
//            executable-tested in the x86_64 sandbox — declared (the
//            sha256_hw.c ARM-CE precedent).
//   CPU    — anonymous MAP_SHARED fallback: byte-identical protocol, no
//            GPU. The module stays fully testable on GPU-less hosts; the
//            BACKEND TAG travels with every claim (Law 4).
//
// Layer discipline: driver layer. weft.c/weft.h untouched. No new link-time
// library dependencies (Vulkan resolved at runtime via dlopen/LoadLibrary;
// SPIR-V consumed through the loader's own entry points).
//
// Honesty boundary: the Vulkan path is executable-verified in this sandbox
// against the lavapipe SOFTWARE ICD (a real Vulkan driver stack — loader,
// instance/device creation, storage-buffer dispatch, mapped-memory
// visibility — executing CPU-side under Mesa; it is NOT discrete-GPU
// performance evidence, and the logs say so). Discrete-GPU and real-Metal
// numbers are hardware-deferred exactly as RFC-0003 defers them; what this
// module removes is the STRUCTURAL staging copy, proven by the shader
// reading the producer's live bytes.

#ifndef WEFT_GPU_RING_H
#define WEFT_GPU_RING_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"

/// Which backend a ring actually got (Law 4: every claim carries it).
typedef enum {
    WEFT_GPU_BACKEND_CPU = 0,      ///< anonymous MAP_SHARED fallback (no GPU)
    WEFT_GPU_BACKEND_VULKAN = 1,   ///< HOST_VISIBLE|HOST_COHERENT device memory
    WEFT_GPU_BACKEND_METAL = 2,    ///< Apple unified memory (compile-gated)
} weft_gpu_backend_t;

/// A GPU-resident ring session (opaque; see gpu_ring.c). The CPU-side ring
/// pointer is available for fanout attach; the GPU-side handles live in
/// the implementation. Destroy with weft_gpu_destroy.
typedef struct weft_gpu_ring weft_gpu_ring_t;

/// Create a session: same geometry rules as the fan-out ring, same WFSH
/// header (so attach-by-bytes validation works identically). Backends are
/// probed in the documented order; the chosen backend is reported by
/// weft_gpu_backend(). Returns 0 on success. May allocate (Law 2 applies
/// to the data path, not create).
int weft_gpu_create(weft_gpu_ring_t** out, size_t payload_bytes, unsigned slot_count);

/// Create with flags (Series 8). WEFT_GPU_CREATE_EXPORTABLE_FD chains
/// VkExportMemoryAllocateInfo into the allocation when the device offers
/// VK_KHR_external_memory_fd, so weft_gpu_export_fd() can hand the session
/// to another process/GPU with zero copies. When the extension is absent
/// the session is created WITHOUT it and export_fd reports -1 with a reason
/// — the WFSH shm session (RFC-0011) is the documented cross-process
/// fallback (transparent, never silent).
int weft_gpu_create_ex(weft_gpu_ring_t** out, size_t payload_bytes,
                       unsigned slot_count, uint32_t flags);

/// Export the session allocation as a POSIX fd (VK_KHR_external_memory_fd;
/// opaque-fd handle type, dma-buf when the ICD offers the EXT too).
/// The fd is OWNED BY THE CALLER (close(2) when done). Returns 0 on
/// success; -1 when the backend is not Vulkan, the allocation was not
/// created exportable, or the ICD refused the handle type.
int weft_gpu_export_fd(weft_gpu_ring_t* g, int* out_fd);

/// Import a session exported by another process (the fd bridge's far side).
/// The allocation is imported (VkImportMemoryFdInfoKHR — the fd is consumed
/// on success), a session-span buffer is bound over it, the WFSH header is
/// validated (geometry must match), and the ring is mapped for CPU access.
/// Returns 0 on success; the WFSH fallback (shm_ring attach) is the
/// documented alternative when the extension is absent.
int weft_gpu_import_fd(weft_gpu_ring_t** out, size_t payload_bytes,
                       unsigned slot_count, int fd);

/// Advisory: which external-memory handle types this ring's device offered
/// at create time ("opaque-fd", "opaque-fd+dmabuf", or "" when not
/// exportable). Logging/evidence only — never a correctness input (AXIOM T).
const char* weft_gpu_external_info(const weft_gpu_ring_t* g);

/// Creation flags (weft_gpu_create_ex, Series 8 — RFC-0013).
typedef enum {
    WEFT_GPU_CREATE_DEFAULT = 0,       ///< plain HOST_VISIBLE session
    WEFT_GPU_CREATE_EXPORTABLE_FD = 1, ///< allocation exports an fd
                                       ///  (VK_KHR_external_memory_fd;
                                       ///   weft_gpu_export_fd below)
} weft_gpu_create_flags_t;

/// The chosen backend (for logging/evidence — never a correctness input).
weft_gpu_backend_t weft_gpu_backend(const weft_gpu_ring_t* g);

/// Backend name ("vulkan" / "metal" / "cpu") for logs and gates.
const char* weft_gpu_backend_name(const weft_gpu_ring_t* g);

/// Human-readable device name the backend resolved (e.g. the Vulkan
/// physical-device name; "anonymous mapping" for CPU). Advisory (AXIOM T).
const char* weft_gpu_device_name(const weft_gpu_ring_t* g);

/// CPU-side ring pointer — pass to weft_fanout_attach_writer /
/// weft_fanout_reader_init exactly like any foreign ring (the layout is
/// the shm_ring session: 64-byte header, then the RFC-0004 ring).
uint8_t* weft_gpu_ring_bytes(weft_gpu_ring_t* g);
size_t weft_gpu_ring_span(weft_gpu_ring_t* g);  ///< header + ring bytes
size_t weft_gpu_payload_bytes(const weft_gpu_ring_t* g);
unsigned weft_gpu_slot_count(const weft_gpu_ring_t* g);

/// GPU-side export (Vulkan backend): the VkBuffer the ring lives in and
/// its size — bind as a storage buffer (the probe does exactly this).
/// Returns NULL/0 on non-Vulkan backends (the Metal road binds the
/// pointer; the CPU road has no GPU consumer).
const void* weft_gpu_vk_buffer(const weft_gpu_ring_t* g);
size_t weft_gpu_vk_buffer_bytes(const weft_gpu_ring_t* g);

/// GPU-side export (Vulkan backend): the VkDevice and the compute queue
/// family index — for building pipelines that consume the ring. NULL on
/// non-Vulkan backends.
const void* weft_gpu_vk_device(const weft_gpu_ring_t* g);
uint32_t weft_gpu_vk_queue_family(const weft_gpu_ring_t* g);

/// GPU-side export (Vulkan backend, Series 8): the VkInstance and the
/// VkPhysicalDevice the session's device was built from — consumers that
/// need instance-level queries (memory properties, extension enumeration
/// for the RFC-0013 fd bridge) use these instead of re-creating instances.
const void* weft_gpu_vk_instance(const weft_gpu_ring_t* g);
const void* weft_gpu_vk_physical_device(const weft_gpu_ring_t* g);

/// Resolve an INSTANCE-level Vulkan entry point by name (vkGetInstanceProcAddr
/// on the ring's instance — the loader discipline mirror of weft_gpu_vk_proc).
/// NULL on non-Vulkan backends or unknown name.
const void* weft_gpu_vk_instance_proc(const weft_gpu_ring_t* g, const char* name);

/// Resolve a device-level Vulkan entry point by name on the ring's device
/// (vkGetDeviceProcAddr under the hood). NULL on non-Vulkan backends or
/// unknown name. This is how the probe builds its pipeline WITHOUT any
/// link-time Vulkan dependency of its own.
const void* weft_gpu_vk_proc(const weft_gpu_ring_t* g, const char* name);

/// Release (destroys GPU objects, unmaps, frees). NULL-safe.
void weft_gpu_destroy(weft_gpu_ring_t* g);

// ---------------------------------------------------------------------------
// GPU-side consumer contract (what a shader sees)
// ---------------------------------------------------------------------------
// The buffer bound at descriptor set 0, binding 0 is the SESSION SPAN
// (header + ring) as u32 words, std430. Word indices (little-endian):
//   session header: words[0..16)          (WFSH magic at words[0..1))
//   latestSeq:      words[16..18)         (u64 LE)
//   publishes:      words[18..20)
//   slotSeq[k]:     words[20 + 2k .. 20 + 2k + 2)   (u64 LE, 0 = invalid)
//   payload word i of slot k:
//                    words[20 + 2*M + k*W + i]      (W = payload_bytes/4)
// The reference consumer (probes/compute/validate_frame.comp) validates
// slot (latestSeq-1) mod M against the mixer family entirely GPU-side.
// Series 8 adds the streaming consumers: probes/compute/stream_frames.comp
// (every resident slot, window advancing across dispatches — RFC-0013) and
// probes/compute/rasterize_frame.comp (latest frame -> rgba8ui storage
// image, self-verifying in-shader). The bind kit that wires any of them is
// core/c/gpu_stream.{h,c} (bindings 0=ring SSBO, 1=result, 2=image,
// 3=texel view).

#endif // WEFT_GPU_RING_H
