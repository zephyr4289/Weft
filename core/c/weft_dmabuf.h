// weft_dmabuf.h — Linux DMA-BUF direct binding for Weft rings (RFC-0016 §3,
// driver layer).
//
// WHY EXISTS: RFC-0011 made ring sessions cross-PROCESS (WFSH over POSIX
// shm); RFC-0003/0013 made them cross-GPU (Vulkan HOST_VISIBLE + the fd
// bridge). The remaining seam is cross-DEVICE: a camera pipe (V4L2
// EXPBUF), a NIC running AF_XDP, a hardware codec, or another GPU all
// speak ONE currency on Linux — the dma-buf fd. This module allocates
// ring storage FROM the kernel's DMA-BUF heaps (/dev/dma_heap/system),
// so the SAME pages are (a) the CPU's mmap'd ring, (b) importable by any
// Vulkan device (weft_gpu wrap-dmabuf — RFC-0016 §2), and (c) passable
// to any device driver that accepts dma-buf — zero intermediate copies
// by construction, because there is only one allocation.
//
//     /dev/dma_heap/system ──ioctl ALLOC──> dma-buf fd
//            mmap MAP_SHARED                    │
//                │                               │
//         WFSH session + RFC-0004 ring          ├── Vulkan import (GPU compute)
//         (CPU publish/claim — fanout API)      ├── V4L2 camera (EXPBUF peer)
//                                                └── AF_XDP UMEM (xdp_rx, RFC-0016 §4)
//
// WHY THE SYSTEM HEAP ONLY: the ring's correctness rests on coherent
// shared-memory atomics (fanout.h's fenced acq/rel over MAP_SHARED).
// /dev/dma_heap/system guarantees cache-coherent CPU mappings; the
// "system-uncached" variant does NOT (it exists for GPU-private staging
// and requires explicit DMA_BUF_IOCTL_SYNC cache maintenance around every
// CPU touch — incompatible with lock-free atomics). This module probes
// for "system" (and "system-dma32" as a fallback) and REFUSES the
// uncached heaps for ring backing — an honest capability boundary, not a
// silent performance cliff. weft_dmabuf_sync() is still exported for
// FOREIGN uncached buffers (camera frames the CPU must touch).
//
// SESSION LAYERING: the WFSH header written here is byte-identical to
// shm_ring's dialect (the DB4 gate proves it field-for-field) — a
// dmabuf-backed session is the same wire object, just backed by
// device-shareable pages. Page-rounded heap allocations exceed
// 64 + ring_bytes, so shm_ring's exact-size attach_fd is NOT used here;
// this module's bind/import validate with >= semantics and hand the
// ring pointer + geometry to weft_fanout_attach_writer directly (the
// size-explicit road RFC-0004 already provides).
//
// CAPABILITY LADDER (probed once, every refusal honest — Law 4):
//   UNSUPPORTED   non-Linux, or no /dev/dma_heap/* device — ring_alloc
//                 refuses; callers use shm_ring/malloc (identical wire
//                 protocol, no device sharing)
//   HEAP_SYSTEM   a coherent system heap opened, allocation proven —
//                 ring_alloc functional
//
// LAW 2: alloc/bind/import run at session setup (syscalls allowed); the
// data path is the ordinary fanout API over the mapping (zero allocs,
// zero syscalls — unchanged).
// LAW 3: driver layer; weft.c/weft.h untouched.
// LAW 4: probe results, heap names, and every ioctl refusal are recorded
// and reported; nothing degrades silently.
//
// Honesty boundary: the heap-ioctl path requires /dev/dma_heap (kernel
// >= 5.6 + the uAPI device nodes). The x86_64 CI/sandbox hosts have no
// heap device: there the module's refusal leg is EXECUTABLE-VERIFIED and
// the mmap-aliasing substrate (two mappings, one physical allocation —
// the property dma-buf shares with memfd) is proven over memfd in the
// DB-series gates. The heap leg runs on hardware that exposes heaps
// (Android per-board, embedded Linux, CONFIG_DMABUF_HEAPS=y hosts) and
// is DECLARED for the sandbox — the exact stance uring_rx takes for the
// FIXED rung and gpu_ring for real GPUs.

#ifndef WEFT_DMABUF_H
#define WEFT_DMABUF_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability probe (once; cached; advisory — AXIOM T)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_DMABUF_UNSUPPORTED = 0,  ///< non-Linux, or no coherent system heap
    WEFT_DMABUF_HEAP_SYSTEM = 1,  ///< /dev/dma_heap/system[@...] opened + alloc-proven
} weft_dmabuf_caps_t;

typedef struct {
    int probed;                   ///< nonzero once the probe ran
    weft_dmabuf_caps_t caps;      ///< the negotiated rung
    char heap_path[64];           ///< the heap device that works ("" when unsupported)
    char heap_list[128];          ///< every heap node found, comma-separated
    long page_size;               ///< sysconf page size (allocation granularity)
} weft_dmabuf_probe_t;

/// Run (or join) the one-shot probe. Thread-safe; allocates nothing after
/// the first call. The probe OPENS the best heap candidate and performs a
/// one-page throwaway allocation, closing the fd — so "HEAP_SYSTEM" means
/// "allocation was proven", not "the node exists".
const weft_dmabuf_probe_t* weft_dmabuf_probe(void);

/// One human-readable capability line for evidence logs.
size_t weft_dmabuf_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// Session geometry
// ---------------------------------------------------------------------------

/// Total session span: WFSH header (64 B, shm_ring dialect) + the RFC-0004
/// ring. The heap allocation is this rounded UP to whole pages (heap
/// granularity — map_bytes >= span_bytes always).
size_t weft_dmabuf_span_bytes(size_t payload_bytes, unsigned slot_count);

// ---------------------------------------------------------------------------
// Ring allocation over a DMA-BUF fd
// ---------------------------------------------------------------------------

/// A dmabuf-backed ring session. The CPU view is `ring` (header at `base`);
/// the DEVICE view is `fd` (import it anywhere dma-buf is accepted).
typedef struct weft_dmabuf_ring {
    int fd;              ///< owned dma-buf fd (-1 when unbound)
    uint8_t* base;       ///< mmap MAP_SHARED of the full allocation
    uint8_t* ring;       ///< base + 64 — hand to weft_fanout_attach_writer
    size_t map_bytes;    ///< the mapped (page-rounded) allocation size
    size_t span_bytes;   ///< 64 + ring_bytes (the session's logical size)
    size_t payload_bytes;
    unsigned slot_count;
    int bound_fd;        ///< 1 = fd was adopted (bind_fd), not heap-allocated
} weft_dmabuf_ring_t;

/// Allocate a ring session from the probed system heap. Writes the WFSH
/// header, zero-initializes the ring ctrl (latestSeq=0, all slots
/// invalidated — the fresh-ring invariants), and leaves `fd` ready for
/// device import. Returns 0; -1 with errno when unsupported (no heap),
/// bad geometry, or allocation/mmap refusal — every refusal recorded,
/// none silent. `out` must be zeroed storage.
int weft_dmabuf_ring_alloc(weft_dmabuf_ring_t* out, size_t payload_bytes,
                           unsigned slot_count);

/// Adopt an EXISTING dma-buf fd as ring storage (the composition road:
/// a heap fd from elsewhere, an AHardwareBuffer's native handle, a
/// V4L2-adjacent allocator). mmaps the fd (fstat size must cover the
/// span — page-rounded is fine), writes the WFSH header, zeroes the
/// ctrl. The fd is BORROWED (the caller owns it; free() never closes
/// it — the opposite of ring_alloc's ownership, so both lifetimes stay
/// explicit). Returns 0 / -1 with errno.
int weft_dmabuf_ring_bind_fd(weft_dmabuf_ring_t* out, int fd,
                             size_t payload_bytes, unsigned slot_count);

/// Unmap and release. Frees the heap fd when this handle allocated it
/// (bound_fd == 0); borrowed fds stay open. Idempotent.
void weft_dmabuf_ring_free(weft_dmabuf_ring_t* r);

// ---------------------------------------------------------------------------
// Foreign import (the camera / peer / GPU-export side)
// ---------------------------------------------------------------------------

/// A mapped foreign dma-buf. When the buffer carries a WFSH session
/// (a Weft producer on the other side), `has_session` is set and the
/// geometry is exposed; otherwise it is a raw device buffer (a camera
/// frame) — the caller binds its bytes as slot payload storage.
typedef struct weft_dmabuf_import {
    int fd;              ///< borrowed caller fd (never closed here)
    uint8_t* base;       ///< mmap MAP_SHARED of map_bytes
    size_t map_bytes;    ///< the mapped size
    int has_session;     ///< 1 = validated WFSH session at base
    uint8_t* ring;       ///< base + 64 when has_session (else NULL)
    size_t payload_bytes;
    unsigned slot_count;
} weft_dmabuf_import_t;

/// Map a foreign dma-buf fd for CPU access. `map_bytes` is a hint (the
/// fstat size is authoritative; the hint clamps it when smaller). A WFSH
/// session is detected and validated with >= size semantics (page-rounded
/// allocations exceed the exact session span — see the layering note);
/// a non-session buffer maps RAW with has_session=0 (not an error — that
/// is the camera-frame road). Returns 0 / -1 with errno.
int weft_dmabuf_import_fd(weft_dmabuf_import_t* out, int fd, size_t map_bytes);

/// Unmap. The fd stays the caller's. Idempotent.
void weft_dmabuf_import_free(weft_dmabuf_import_t* i);

// ---------------------------------------------------------------------------
// Cache maintenance for FOREIGN uncached buffers
// ---------------------------------------------------------------------------

/// DMA_BUF_IOCTL_SYNC bracket around CPU access to a FOREIGN buffer that
/// may be uncached (camera/codec frames — never this module's ring
/// storage, which is always coherent). `start`: 1 = SYNC_START, 0 =
/// SYNC_END. `read_only`: CPU will only read. Returns 0 / -1 (a refusal
/// is information: memfd and coherent dma-bufs reject the ioctl — the
/// caller learns the buffer needed no maintenance or was not a dma-buf;
/// both are recorded, neither crashes).
int weft_dmabuf_sync(int fd, int start, int read_only);

#ifdef __cplusplus
}
#endif

#endif // WEFT_DMABUF_H
