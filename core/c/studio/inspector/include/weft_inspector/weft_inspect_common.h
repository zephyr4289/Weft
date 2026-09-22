// weft_inspect_common.h — shared belt for the Weft Studio native inspector
// stack (Pillar 7): error codes, time, and the bounded memory arena that
// carries Law 2 (zero-heap steady state) through every layer.
//
// WHY EXISTS: the inspector family (shm inspector, contention profiler,
// memory-map feeder) must scrape live rings at 1000 Hz and emit visual
// frames at 120/240 FPS for hours without a single heap allocation in any
// steady-state loop. That discipline needs ONE shared vocabulary — negative
// errno-style codes (house convention), a monotonic-raw clock for
// microsecond contention resolution (Law 3), and a bump arena the CALLER
// owns so every bounded buffer in this stack has a visible, auditable
// ceiling instead of a hidden realloc.
//
// LAYERS (each is a separate module; upper layers hold a pointer to the
// inspector context and never duplicate its mappings):
//   weft_shm_inspector      attach + topology + read-only peek seam
//   weft_contention_profiler  history, torn reads, stalls, drops, fshare
//   weft_memory_stream      shadow planes + 120/240 FPS frame staging
//
// LAWS (mirrored from the D-72 directive; each module header restates the
// ones it carries):
//   Law 1  non-invasive: PROT_READ MAP_SHARED views only; no exclusive
//          locks on producer rings; no futex, no store, no RMW on any
//          watched word; total inspector CPU on a 10M msg/s stream < 0.5%.
//   Law 2  zero-heap steady state: sampling, scraping, telemetry, and
//          frame assembly run in caller-provided fixed arrays or arena
//          memory; zero malloc/calloc/realloc once the contexts are built
//          (the plain-leg batteries interpose the allocator to prove it).
//   Law 3  microsecond resolution: every contention event (torn read,
//          stall window, drop burst, false-sharing tripwire) is stamped
//          with CLOCK_MONOTONIC_RAW nanoseconds.
//   Law 4  dual-arch + container resilience: -std=c11 -Wall -Wextra
//          -Werror -pedantic clean under GCC 14.2 and Clang 21; no VLAs,
//          no anonymous struct members, no GNU statement expressions in
//          this module family; fork batteries clean up via PR_SET_PDEATHSIG
//          children and the runner's /dev/shm healing sweep.
//
// NAMESPACE: weft_inspect_* / weft_prof_* / weft_mstream_* only. The
// weft_studio_* namespace is Engineer 1's (core/c/include/weft_studio.h,
// core/c/studio/src/); no symbol here may collide with it, with
// weft_spectrum_*, or with weft_tensor_* (gate in the suite runner).

#ifndef WEFT_INSPECT__COMMON_H_
#define WEFT_INSPECT__COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- error codes (negative, errno-flavored, never silent) --------------- */

enum {
    WEFT_INSPECT_OK = 0,
    WEFT_INSPECT_ERR_SYS = -1,      /* errno carries the syscall failure   */
    WEFT_INSPECT_ERR_INVALID = -2,  /* bad arguments / wrong context state */
    WEFT_INSPECT_ERR_FULL = -3,     /* fixed capacity exhausted (honest)   */
    WEFT_INSPECT_ERR_NOENT = -4,    /* segment vanished / not yet written  */
    WEFT_INSPECT_ERR_ABI = -5,      /* magic/version/geometry mismatch:    */
                                    /* flagged, never guessed at           */
    WEFT_INSPECT_ERR_AGAIN = -6,    /* torn snapshot: bounded retry done,  */
                                    /* caller retries next scrape          */
};

/* --- time (Law 3: microsecond contention resolution) --------------------- */

/// CLOCK_MONOTONIC_RAW nanoseconds. RAW because CFS-rate-adjusted
/// monotonic would smear exactly the stall windows the backpressure
/// monitor exists to measure (a 22 ms CFS throttle must READ as 22 ms).
int64_t weft_inspect_now_ns(void);

/* --- arena (Law 2: caller-owned bounded memory) -------------------------- */

/// A bump arena over memory the caller owns (static, stack, or mmap).
/// Allocation is 8-byte aligned; exhaustion returns NULL — never a hidden
/// realloc. `used` is inspectable so tests can prove zero growth.
typedef struct weft_inspect_arena {
    uint8_t *base;
    size_t cap;
    size_t used;
} weft_inspect_arena_t;

/// Initialize an arena over [base, base+cap). Returns 0 / -INVALID.
int weft_inspect_arena_init(weft_inspect_arena_t *a, void *base, size_t cap);

/// Bump allocation. NULL when the arena is exhausted or misaligned input
/// would overflow — the caller's bounded-buffer policy decides what that
/// means (this stack: mark overflow, keep counting, never allocate).
void *weft_inspect_arena_alloc(weft_inspect_arena_t *a, size_t bytes);

/// Bytes still free.
size_t weft_inspect_arena_free(const weft_inspect_arena_t *a);

/* --- cache-line facts (Law 3 false-sharing tripwires) --------------------- */

/// The coherence granularity this profiler reasons in. Fixed at 64: every
/// production target of this tree (x86_64 since Nehalem/Conroe, aarch64
/// since ARMv8) coheres at 64 bytes, and the directive pins the tripwire
/// to the "exact same 64-byte cache line". If a future target differs,
/// this constant moves WITH the detector, not around it.
#define WEFT_INSPECT_CACHE_LINE 64u

/// The base address of the line containing p (p rounded DOWN to 64).
uintptr_t weft_inspect_line_base(const void *p);

/// Nonzero iff a and b cohabit one 64-byte line.
int weft_inspect_same_line(const void *a, const void *b);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_INSPECT__COMMON_H_ */
