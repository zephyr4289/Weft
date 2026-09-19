// weft_hw.h — Issue #15: thin hardware-adaptive abstraction layer ABOVE the
// core (probe-once capabilities + function-pointer dispatch).
//
// WHY EXISTS: the core is correct and portable, but payload processing and
// buffer management can exploit heterogeneous hardware (SIMD, cache/NUMA
// topology, big.LITTLE placement, GPU presence) WITHOUT touching the
// protocol — the Triad exchange itself must remain untouched (02 §2.2). This
// layer detects capabilities ONCE at startup and dispatches through direct
// function pointers: zero per-frame checks, zero vtables, zero OOP.
//
// LAYOUT MAPPING (the issue proposes include/weft_hw_caps.h +
// src/weft_hw_detect.c + include/weft_adapt.h; this repo's convention keeps
// every core-port module as a flat core/c/weft_<name>.{h,c} pair — same
// shape as weft/fanout/turbo/governor — so the three proposed files land as
// this ONE pair; the estimate's ~170 lines of surface carry over).
//
// SEPARATION FROM turbo.h (RFC 0012): turbo probes what the OS PERMITS
// (affinity, mlock, FIFO, THP); weft_hw probes what the SILICON OFFERS
// (ISA, topology, GPU presence). They compose: place with turbo, process
// with weft_hw.
//
// THE ISSUE'S OPEN QUESTIONS, ANSWERED (with precedent):
//   Q1  GPU offload in this layer or the application?
//       -> APPLICATION (the issue's own proposal; round-7 hardware-deferred
//          precedent + RFC-0013's app-layer streaming kit). weft_hw only
//          REPORTS loader presence — advisory (AXIOM T), no link dependency.
//   Q2  Thread affinity mandatory or optional?
//       -> OPTIONAL (explicit request only — weft_hw_spawn_pinned; the
//          default scheduler path is plain pthread_create).
//   Q3  Cache-line detection mandatory or fallback 64B?
//       -> FALLBACK 64B (safe on every CPU we target; detected when the
//          platform reports it).
//
// NON-GOALS (the issue's list, kept): no core/c/weft.{h,c} changes; no
// vtable/OOP; no per-frame capability checks; no dynamic resizing; no GPU
// logic in core. Fallback guarantee: worst case = current performance.
//
// LAWS: probe allocates nothing on the hot path (init/once only); every
// capability reports its REFUSAL honestly (gpu/loader absent is a state,
// not an error); AXIOM T: caps are advisory snapshots, never protocol state.

#ifndef WEFT_HW_H
#define WEFT_HW_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capabilities — probed ONCE (pthread_once), read-only forever after
// ---------------------------------------------------------------------------

/// GPU/loader presence (advisory only — offload belongs to the application).
typedef enum {
    WEFT_HW_GPU_NONE = 0,      ///< no Vulkan loader dlopen-able
    WEFT_HW_GPU_LOADER = 1,    ///< Vulkan loader present (devices unprobed — app's call)
} weft_hw_gpu_t;

/// Detected hardware capabilities (~the issue's weft_hw_caps_t). Probe is
/// idempotent, thread-safe, and cold: weft_hw_probe() runs the detection
/// once and every later call returns the same immutable record.
typedef struct {
    int probed;                ///< nonzero once the probe ran
    // ISA (CPUID / __builtin_cpu_supports; NEON compile-time)
    int has_sse42;
    int has_avx2;
    int has_avx512f;
    int has_avx512vl;
    int has_fma;
    int has_neon;              ///< compile-time (__ARM_NEON__) — 0 on x86 builds
    // Topology / memory system
    int cache_line;            ///< L1d line size in bytes (fallback 64 — Q3)
    int core_count;            ///< online cores
    int numa_nodes;            ///< NUMA node count (1 = single node)
    int big_little;            ///< heterogeneous cpu_capacity detected (ARM-style)
    weft_hw_gpu_t gpu;         ///< loader presence only (Q1: app-layer offload)
} weft_hw_caps_t;

/// Run (or join) the one-shot capability probe. Returns the immutable caps
/// record. Thread-safe; allocation-free after the first call.
const weft_hw_caps_t* weft_hw_probe(void);

/// Human-readable caps report (cold path, diagnostics only).
size_t weft_hw_caps_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// Processor — SIMD-vs-scalar payload processing, dispatched ONCE
// ---------------------------------------------------------------------------

/// The processor shape (the issue's `void (*)(buf, len)`): an in-place
/// deterministic payload transform. Reference kernel: the lane-scramble
/// transform (positional XOR + rotate — element-wise by construction, so
/// every SIMD width produces BIT-IDENTICAL output).
typedef void (*weft_hw_process_fn)(uint8_t* buf, size_t len);

/// The dispatched transform (selected once from caps: AVX-512 -> AVX2 ->
/// SSE2 -> scalar; NEON on ARM builds). len must be a multiple of 4.
void weft_hw_xor_transform(uint8_t* buf, size_t len);

/// Which backend the dispatch selected ("avx512" | "avx2" | "sse2" |
/// "neon" | "scalar") — for tests and diagnostics.
const char* weft_hw_xor_transform_backend(void);

/// A second reference kernel with a lane-structured REDUCTION (the
/// interesting equivalence: 16 virtual u32 lanes, strided sums, one final
/// mix — every SIMD width implements the SAME lane math, so outputs are
/// bit-identical across backends by construction, not by luck).
typedef uint32_t (*weft_hw_checksum_fn)(const uint8_t* buf, size_t len);

/// The dispatched checksum (same selection ladder as the transform).
uint32_t weft_hw_checksum32(const uint8_t* buf, size_t len);

/// Which backend the checksum dispatch selected.
const char* weft_hw_checksum32_backend(void);

/// Direct access to every COMPILED variant (tests verify cross-variant
/// bit-identity; applications never need these — the dispatch owns them).
void    weft_hw_xor_transform_scalar(uint8_t* buf, size_t len);
void    weft_hw_xor_transform_sse2(uint8_t* buf, size_t len);
void    weft_hw_xor_transform_avx2(uint8_t* buf, size_t len);
void    weft_hw_xor_transform_avx512(uint8_t* buf, size_t len);
uint32_t weft_hw_checksum32_scalar(const uint8_t* buf, size_t len);
uint32_t weft_hw_checksum32_sse2(const uint8_t* buf, size_t len);
uint32_t weft_hw_checksum32_avx2(const uint8_t* buf, size_t len);
uint32_t weft_hw_checksum32_avx512(const uint8_t* buf, size_t len);

// ---------------------------------------------------------------------------
// Allocator — cache-aware aligned allocation, dispatched ONCE
// ---------------------------------------------------------------------------

/// The allocator shape (the issue's `void* (*)(size, align)`). Dispatched
/// once: alignment is clamped up to max(64, cache_line) — the detected line
/// size when the platform reports one, the safe 64B otherwise (Q3). Backed
/// by posix_memalign (the same allocator weft_init uses), so worst case is
/// exactly current behavior. Returns NULL on failure. Free with free().
typedef void* (*weft_hw_alloc_fn)(size_t size, size_t align);

/// The dispatched allocator.
void* weft_hw_alloc(size_t size, size_t align);

// ---------------------------------------------------------------------------
// Scheduler — thread placement at creation, optional (Q2)
// ---------------------------------------------------------------------------

/// The scheduler shape (the issue's `int (*)(thread_fn)`): spawn a thread.
/// DEFAULT: plain pthread_create (OS placement — zero policy). Placement is
/// OPT-IN: weft_hw_spawn_pinned requests a specific core and reports its own
/// refusals (errno-style return) honestly.
typedef int (*weft_hw_spawn_fn)(void* (*fn)(void*), void* arg);

/// The dispatched spawner (default: OS placement).
int weft_hw_spawn(void* (*fn)(void*), void* arg);

/// OPT-IN pinned spawn (big.LITTLE-aware placement is the caller's policy:
/// caps.big_little + /sys capacity data tell you WHICH core; this call
/// places ON it). Returns 0 on success, nonzero on refusal (no affinity
/// permission, bad core id) — refusals are reported, never swallowed.
int weft_hw_spawn_pinned(void* (*fn)(void*), void* arg, int core);

#ifdef __cplusplus
}
#endif

#endif // WEFT_HW_H
