// turbo.h — RFC 0012: Tail-Latency Eradication Layer (TLEL), C driver layer
//
// WHY EXISTS: the kernel and the RFC 0004 fan-out ring are FROZEN (Kernel
// Freeze contract — Volume I §2; weft.{h,c}/lib.rs/weft.ts byte-frozen).
// Every sub-microsecond P99 technique in this module therefore rides the
// seams the frozen layers already expose:
//   - weft_fanout_attach_writer()  -> foreign ring memory (this module maps
//     it: 2 MiB-aligned, MADV_HUGEPAGE, MAP_POPULATE, NUMA first-touch)
//   - documented advisory ctrl reads (AXIOM T; fanout.h "ring base pointer"
//     note) -> reader-side prefetch of the likely target slot
//   - writer-private field reads from the writer thread (the 02 §1 field
//     discipline ALLOWS the writer to read its own w_seq) -> writer-side
//     next-slot prefetch
// Zero bytes of weft.{h,c}, fanout.{h,c}, lib.rs, weft.ts are touched; the
// -seq A/B regime and the F-series gates re-run unchanged over this layer.
//
// CAPABILITY LADDER (Law 4 — every service degrades, never crashes, and
// every refusal is REPORTED, not hidden):
//   service          wants                        this sandbox (probed)
//   ---------------  ---------------------------  ------------------------
//   THP ring         THP=madvise|always           THP=always (kernel 5.10)
//   prefault         MAP_POPULATE + memset        available
//   core pinning     sched_setaffinity            available (2 CPUs)
//   NUMA first-touch >1 node                      1 node — ladder verified,
//                                                   cross-node deferred
//   mlock            RLIMIT_MEMLOCK >= ring       64 KiB cap — graduated:
//                                                   small rings lock, multi-MiB
//                                                   rings refused, errno reported
//   SCHED_FIFO       CAP_SYS_NICE                 EPERM -> refused, reported
//   io_uring ingest  see uring_rx.h               RING mode live (kernel 5.10:
//                                                   registration functional;
//                                                   FIXED version-gated >= 5.19)
//
// INVARIANT POSTURES (Volume I §3):
//   I1  monotonicity   — untouched; this layer never writes stamps.
//   I2  non-tearing    — prefetch is architecturally a no-op; the NT fill
//                        adds one sfence BEFORE the publish release stamp
//                        (WC stores ordered by the fence; P1/P2 preserved).
//   I3  single-writer  — all writer-side helpers are writer-thread-only.
//   I4  telescoping    — claim wrappers return the frozen claim verbatim.
//   I5  zero-alloc     — every hot-path helper is allocation-free.
//   I6  revocation     — untouched (kernel path only).
//   I7  drop accounting— untouched; stats come from the frozen reader.
//   I8  cache coherence— prefetch hints (T0) only ADD locality; alignment
//                        services guarantee 64B (and 2 MiB) boundaries.
//
// LAW 2: hot-path helpers (begin/publish/claim/fill wrappers) allocate
// nothing. Init/teardown services may (they are cold by definition).

#ifndef WEFT_TURBO_H
#define WEFT_TURBO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include "fanout.h"

#if defined(__linux__)
#define WEFT_TURBO_LINUX 1
#include <sched.h>
#else
#define WEFT_TURBO_LINUX 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability probe (cold path; probed once, cached, idempotent)
// ---------------------------------------------------------------------------

/// Transparent-hugepage kernel policy, as far as /sys reports it.
typedef enum {
    WEFT_TURBO_THP_UNKNOWN = 0,
    WEFT_TURBO_THP_ALWAYS  = 1,  ///< [always] — anon VMAs THP-eligible
    WEFT_TURBO_THP_MADVISE = 2,  ///< [madvise] — MADV_HUGEPAGE is load-bearing
    WEFT_TURBO_THP_NEVER   = 3,  ///< [never] — THP off; alignment still helps
} weft_turbo_thp_mode_t;

/// Max NUMA nodes tracked (physical hosts beyond 16 sockets are outside the
/// sandbox envelope; bind requests past this bound are refused, not guessed).
#define WEFT_TURBO_MAX_NODES 16

/// Opaque saved CPU affinity (cpu_set_t can be 128 B at 1024 CPUs).
typedef struct {
    char raw[128];
    int valid;
} weft_turbo_affinity_t;

/// Probed environment capabilities. Advisory (AXIOM T): a probe result is a
/// fact about the host, never a correctness assumption — every service
/// re-checks its own syscall return at use time.
typedef struct {
    int probed;                     ///< nonzero once the probe ran
    long page_size;                 ///< >= 4096 expected
    long hugepage_size;             ///< 2 MiB expected; 0 = unknown
    int node_count;                 ///< >= 1
    int node_cpu_count[WEFT_TURBO_MAX_NODES];  ///< CPUs per node (0 = absent)
    weft_turbo_thp_mode_t thp_mode; ///< /sys/kernel/mm/transparent_hugepage
    char thp_mode_str[24];          ///< raw bracket content, e.g. "[always]"
    int affinity_ok;                ///< sched_setaffinity probe succeeded
    int mlock_ok;                   ///< small mlock probe succeeded
    int mlock_errno;                ///< refusal errno when !mlock_ok (0 if ok)
    int sched_fifo_ok;              ///< SCHED_FIFO probe succeeded
    int sched_fifo_errno;           ///< refusal errno when !sched_fifo_ok
    int ncpu;                       ///< online CPU count
} weft_turbo_caps_t;

/// Probe (first call) and return the cached capability record.
const weft_turbo_caps_t* weft_turbo_caps(void);

/// One human-readable multi-line capability report (into buf; always
/// NUL-terminated). Intended for evidence logs — Law 4: the refusals are
/// the interesting lines.
size_t weft_turbo_caps_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// Tail-latency ring allocation — mmap + THP + prefault + NUMA first-touch
// ---------------------------------------------------------------------------

typedef struct {
    unsigned slot_count;   ///< ring depth M (validated by fanout geometry)
    size_t payload_bytes;  ///< per-slot payload (multiple of 4)
    int want_thp;          ///< 2 MiB-align + MADV_HUGEPAGE (default 1)
    int want_prefault;     ///< MAP_POPULATE + explicit touch (default 1)
    int want_mlock;        ///< best-effort mlock of the ring (default 1)
    int numa_node;         ///< -1 = no bind; else first-touch on that node
} weft_turbo_ring_opts_t;

/// What the allocator actually achieved (the honesty record — every field
/// is a syscall result, not a wish).
typedef struct {
    void* ring;            ///< the ring base passed to weft_fanout_attach_writer
    size_t ring_bytes;     ///< geometry-checked ring size
    void* map_base;        ///< post-trim mapping base to munmap on release
    size_t map_len;        ///< post-trim mapping length
    int thp_advised;       ///< madvise(MADV_HUGEPAGE) rc: 0 ok, else errno
    int prefaulted;        ///< 1 = MAP_POPULATE + explicit first-touch memset
    int locked;            ///< mlock rc: 0 ok, else errno (EPERM/ENOMEM ladder)
    int numa_node;         ///< node used for first-touch; -1 = default policy
    int numa_bound;        ///< 1 = the bind scope was actually applied
} weft_turbo_ring_t;

void weft_turbo_ring_opts_default(weft_turbo_ring_opts_t* o);

/// Create a fan-out broadcaster over a TLEL-optimized mapping:
///   1. mmap (PRIVATE|ANON[, MAP_POPULATE]) sized ring_bytes + alignment slack
///   2. trim to the hugepage boundary (when want_thp and the size is known)
///   3. madvise(MADV_HUGEPAGE) — best-effort, errno recorded
///   4. optional NUMA bind scope, then the ctrl-zeroing memset doubles as
///      first-touch (pages land on the bound node)
///   5. optional mlock — best-effort, errno recorded
///   6. weft_fanout_attach_writer() — the FROZEN attach path validates the
///      geometry and adopts the mapping; ctrl stays zeroed exactly like a
///      fresh weft_fanout_init ring (byte-identical initial state).
/// `f` must be an empty weft_turbo-... broadcaster storage (the attach
/// contract: zeroed struct). Returns 0 on success; -1 on geometry/alloc
/// failure (tr resources released).
int weft_turbo_fanout_create(weft_fanout_t* f, weft_turbo_ring_t* tr,
                             const weft_turbo_ring_opts_t* o);

/// Release: destroy the broadcaster (frees nothing — owns_ring == 0) and
/// munmap the mapping. Idempotent (zeroed tr is a no-op).
void weft_turbo_fanout_destroy(weft_fanout_t* f, weft_turbo_ring_t* tr);

// ---------------------------------------------------------------------------
// Prefetch wrappers (inline, hot path, zero allocation, zero semantics)
// ---------------------------------------------------------------------------

/// Prefetch cap: only the LEADING region of a payload is prefetched (the
/// claim's validation copy touches it first; the copy itself streams the
/// tail with the hardware prefetcher running ahead). Uncapped, a 64 KiB
/// payload would issue 1024 hint instructions per frame — measured as pure
/// overhead when the ring is already cache-resident (see the T-bench
/// TL-writer evidence: full-width prefetch at 64 KiB was a p50 regression;
/// the cap removes the instruction cost while keeping the cold-start win).
#ifndef WEFT_TURBO_PREFETCH_MAX_BYTES
#define WEFT_TURBO_PREFETCH_MAX_BYTES 4096
#endif

/// RUNTIME-CONFIGURABLE DISTANCE (issue #17-4): the compile-time macro is
/// the STARTUP DEFAULT; set_distance() changes it live (0 = hints off) and
/// tune() applies the per-vendor starting point (AMD Zen family -> 256 B,
/// Apple silicon -> 128 B, everything else -> 128 B). The TL-reader pf=
/// sweep is the per-host refinement tool and produced this build's default.
/// Hard ceiling: a runaway value would issue huge hint batches per frame.
#define WEFT_TURBO_PREFETCH_DIST_HARD_MAX (1 << 20)

/// The runtime distance (turbo.c; initialized to the compile default).
/// Relaxed: a stale read only issues a hint batch of yesterday's size —
/// architecturally a no-op either way.
extern _Atomic int _weft_turbo_pf_dist;

/// Set the prefetch distance in leading bytes. 0 disables prefetch hints
/// entirely (the wrappers stay legal — hints only). Negative refused (-1);
/// above the hard max clamps to it. Returns the effective distance set.
int weft_turbo_prefetch_set_distance(int bytes);

/// The current effective distance (leading bytes; 0 = hints off).
int weft_turbo_prefetch_get_distance(void);

/// Vendor autotune (AMD Zen >= 0x17 -> 256; Apple aarch64 -> 128; other
/// x86/aarch64/RISC-V -> 128). A documented STARTING POINT, not a verdict —
/// the TL-reader pf= sweep refines it per host. Returns the distance set.
int weft_turbo_prefetch_tune(void);

/// Prefetch the leading `min(len, runtime distance)` bytes at `p` for a
/// near-future READ (one T0 hint per 64-byte line). Architecturally a
/// no-op — issued for its latency-hiding side effect only; correctness
/// never depends on it. Distance 0 short-circuits to nothing.
static inline void weft_turbo_prefetch_read(const void* p, size_t len) {
    const char* q = (const char*)p;
    const int dist = atomic_load_explicit(&_weft_turbo_pf_dist,
                                          memory_order_relaxed);
    if (dist <= 0) return;
    if (len > (size_t)dist) {
        len = (size_t)dist;
    }
    for (size_t off = 0; off < len; off += 64) {
        __builtin_prefetch(q + off, 0 /* read */, 3 /* high temporal locality */);
    }
}

/// Fan-out payload base (the fanout.h byte-layout contract: 16 + 8*M).
static inline size_t weft_turbo_fan_payload_base(unsigned slot_count) {
    return 16 + 8 * (size_t)slot_count;
}

/// begin() + prefetch of the slot being begun. The slot's lines were last
/// touched M frames ago — typically cold at display cadence. Legal field
/// reads: w_seq/ring/payload_bytes/slot_count from the WRITER thread (02 §1
/// writer-private discipline — this helper is writer-thread-only).
static inline uint8_t* weft_turbo_begin(weft_fanout_t* f) {
    // Predict the slot begin() is about to choose: k = w_seq % M.
    const unsigned k = (unsigned)(f->w_seq % f->slot_count);
    const uint8_t* cursor = f->ring + weft_turbo_fan_payload_base(f->slot_count)
                          + (size_t)k * f->payload_bytes;
    weft_turbo_prefetch_read(cursor, f->payload_bytes);
    return weft_fanout_begin(f);
}

/// publish() + prefetch of the NEXT slot (the one the next begin() fills),
/// issued before the current frame's release stores retire. Writer thread
/// only (same field-discipline rationale as weft_turbo_begin).
static inline uint64_t weft_turbo_publish(weft_fanout_t* f) {
    const unsigned k = (unsigned)((f->w_seq + 1) % f->slot_count);
    const uint8_t* cursor = f->ring + weft_turbo_fan_payload_base(f->slot_count)
                          + (size_t)k * f->payload_bytes;
    weft_turbo_prefetch_read(cursor, f->payload_bytes);
    return weft_fanout_publish(f);
}

/// Prefetch the LIKELY target slot from the ADVISORY latestSeq (Relaxed,
/// AXIOM T — the ctrl word documented readable in fanout.h). Issued as
/// early as the application knows a claim is coming (e.g. before the frame's
/// render work), so the memory latency overlaps compute instead of stacking
/// in front of the claim. A stale guess is a wasted hint, never a wrong
/// result. Callable from any reader thread; allocation-free.
static inline void weft_turbo_prefetch_next(weft_fanout_reader_t* r) {
    // ctrl[0] == latestSeq (fanout.h layout contract). Advisory only.
    const uint64_t L = atomic_load_explicit(r->ctrl, memory_order_relaxed);
    if (L != 0 && L != r->last_seq) {
        const unsigned k = (unsigned)((L - 1) % r->slot_count);
        weft_turbo_prefetch_read(r->slot_words[k], r->payload_bytes);
    }
}

/// claim() + prefetch of the likely target slot (the late-hint variant of
/// weft_turbo_prefetch_next — the frozen claim's own acquire load decides
/// truth; the advisory read may be stale, which only wastes a hint).
/// Callable from any reader thread.
static inline const weft_fanout_claim_t* weft_turbo_claim(weft_fanout_reader_t* r) {
    weft_turbo_prefetch_next(r);
    return weft_fanout_claim(r);
}

// ---------------------------------------------------------------------------
// Non-temporal streaming fill (x86-64 AVX2; large payloads)
// ---------------------------------------------------------------------------

/// Fill the begun slot via 256-bit non-temporal stores + a trailing SFENCE.
///
/// ORDERING CONTRACT (why the sfence is load-bearing, per fanout.h P1/P2):
/// NT stores are weakly ordered (WC) — a Release stamp alone does NOT order
/// them on x86. fill() therefore issues `_mm_sfence()` BEFORE returning, so
/// the subsequent publish() release stamp retires after every payload word.
/// The begin() invalidate (SeqCst store + fence) precedes the fill in
/// program order and an NT store cannot become visible before an already-
/// fenced store — the bracket is preserved end-to-end.
///
/// Alignment discipline: the streaming loop requires a 32-byte-aligned
/// destination; odd geometries whose payload cursor is not 32B-aligned (or
/// non-x86-64 builds) fall back to the frozen weft_fanout_fill() — the
/// fallback is byte-identical, only slower (honest equivalence, T5).
/// `len` must be a multiple of 4 and <= payload_bytes (same contract as
/// weft_fanout_fill). Returns words written, or -1 on bad len / no begin().
int weft_turbo_fill(weft_fanout_t* f, const void* src, size_t len);

// ---------------------------------------------------------------------------
// Thread services (cold-ish paths; every refusal is a return code)
// ---------------------------------------------------------------------------

/// Snapshot the calling thread's affinity mask for later restore.
int weft_turbo_save_affinity(weft_turbo_affinity_t* out);

/// Restore a saved affinity snapshot (no-op if !valid).
int weft_turbo_restore_affinity(const weft_turbo_affinity_t* saved);

/// Pin the calling thread to one CPU. 0 on success, -errno on refusal
/// (unprivileged affinity is normally permitted; cgroup cpusets can refuse).
int weft_turbo_pin_cpu(int cpu);

/// Attempt SCHED_FIFO at `prio`. 0 on success, -errno on refusal — the
/// EPERM-without-CAP_SYS_NICE case is EXPECTED in containers and sandboxes;
/// deterministic jitter then rests on pinning alone (documented trade).
int weft_turbo_rt(int prio);

/// Attempt mlock(MCL_CURRENT|MCL_FUTURE). 0 on success, -errno on refusal
/// (RLIMIT_MEMLOCK is commonly 64 KiB in containers — the ring-allocator's
/// per-ring mlock hits the same ladder).
int weft_turbo_mlockall(void);

/// Bind the calling thread to node `node`'s CPUs (saves the prior mask into
/// `saved` when non-NULL). 0 on success, -errno on refusal. On single-node
/// hosts this is a no-op bind that still verifies the node exists — the
/// cross-node measurement story is hardware-gated and honestly labeled.
int weft_turbo_numa_bind(int node, weft_turbo_affinity_t* saved);

#ifdef __cplusplus
}
#endif

#endif // WEFT_TURBO_H
