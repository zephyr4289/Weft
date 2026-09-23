// weft_contention_profiler.h — real-time lock-free telemetry over the
// inspector's read-only views (Pillar 7).
//
// WHY EXISTS: a zero-copy mesh that is fast when healthy gives no warning
// before it is slow when contended. The three failure modes this profiler
// isolates are exactly the ones a lock-free seqlock fabric can have:
//   1. TORN-READ PRESSURE — writers flip slot versions while readers are
//      mid-bracket, forcing retries; the reader feels latency, the ring
//      counters show nothing.
//   2. BACKPRESSURE / STALLS — a saturated consumer pins the producer in
//      its spin/sleep ladder; the publisher's local timeout is invisible
//      in shared memory except as a frozen head over a full ring.
//   3. DROPS — BEST_EFFORT drops are publisher-counted (dropped_total);
//      they must be accounted per-interval without missing events.
// Plus the silent killer the directive calls out:
//   4. FALSE SHARING — two writer threads in different processes mutating
//      adjacent offsets on the same 64-byte line: the line ping-pongs
//      between cores on the coherency bus and the per-word latency
//      doubles while every individual counter looks healthy.
//
// All four are measured WITHOUT taking a single lock, storing a single
// watched word, or parking a single producer (Law 1): the profiler rides
// the inspector's PROT_READ views, diffs consecutive snapshots, and keeps
// every history in caller-owned fixed arrays (Law 2).
//
// LAW 3 (this module): every event — torn read, stall window, drop burst,
// false-sharing episode — is opened and closed with CLOCK_MONOTONIC_RAW
// nanosecond stamps; durations are exact differences, never scheduler-
// smeared. Resolution is bounded by the scrape cadence the studio chooses
// (1000 Hz default posture: event edges quantized to ~1 ms; the D-72
// bench measures the actual pass cost at ~microseconds).
//
// HONEST ATTRIBUTION: "concurrent" cross-process mutation is observed as
// co-mutation WITHIN one bounded scrape window — two words whose values
// both changed between consecutive passes, owned by different pids, on
// one 64-byte line. That is the strongest statement a non-invasive
// observer can make (a park on the writer's core would violate Law 1);
// it is labeled as observed co-mutation, and it is exactly the tripwire
// condition the directive specifies.
//
// RELIABLE timeouts are publisher-local by design (the D-62 wait ladder
// returns RMW_RET_TIMEOUT to the caller without a shared-memory trace).
// This profiler accounts their shared-memory-visible symptom precisely:
// stall episodes whose duration meets the publisher's zero-progress
// budget are counted as reliable_timeout_windows — a window count, not a
// per-message count, and declared as such everywhere it is surfaced.

#ifndef WEFT_INSPECT__CONTENTION_PROFILER_H_
#define WEFT_INSPECT__CONTENTION_PROFILER_H_

#include <stddef.h>
#include <stdint.h>

#include "weft_inspector/weft_inspect_common.h"
#include "weft_inspector/weft_shm_inspector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- capacities (fixed arrays; the context is caller-owned) -------------- */

#define WEFT_PROF_MAX_RINGS 32u
#define WEFT_PROF_MAX_REGIONS 8u
#define WEFT_PROF_MAX_WORDS 256u          /* generic-region watch words   */
#define WEFT_PROF_MAX_FSHARE_EPISODES 16u  /* simultaneously open pairs    */
#define WEFT_PROF_EVENT_RING 64u           /* per-kind event capacity      */

/* --- word ownership -------------------------------------------------------- */

typedef enum {
    WEFT_PROF_OWNER_UNKNOWN = 0,
    WEFT_PROF_OWNER_PUB = 1,    /* publisher-side hot word (head, ...)   */
    WEFT_PROF_OWNER_SUB = 2,    /* subscriber-side hot word (tail_ack)   */
    WEFT_PROF_OWNER_SYS = 3,    /* creator/system (rarely mutating)      */
} weft_prof_owner_t;

/* --- per-ring statistics (queryable any time) ------------------------------ */

typedef struct {
    /* torn-read tracker (frontier probes; Law 3) */
    uint64_t torn_attempts;      /* frontier probes made                 */
    uint64_t torn_events;        /* odd-version or version-flip observed */
    uint64_t writer_churn;       /* frontier version increments observed */
    /* backpressure / stall (episodes closed + current state) */
    uint64_t stalls;             /* closed stall episodes                 */
    uint64_t stall_ns_total;     /* summed stall duration (closed)        */
    uint64_t reliable_timeout_windows; /* stalls >= publisher budget      */
    uint8_t  stall_open;         /* 1 = an episode is open right now      */
    uint64_t stall_opened_ns;    /* open episode start (0 when closed)    */
    uint64_t in_flight_now;      /* head - tail_ack at last pass          */
    uint64_t capacity;           /* slot_count - 2                        */
    /* drop ledger (exact, never missing an interval) */
    uint64_t published_cum;      /* cumulative committed messages         */
    uint64_t dropped_cum;        /* cumulative BEST_EFFORT drops          */
    uint64_t drop_bursts;        /* intervals with dropped_delta > 0      */
    uint64_t last_event_ns;      /* last ledger-affecting event stamp     */
    double   drop_rate;          /* dropped_cum / published_cum (0-safe)  */
} weft_prof_ring_stats_t;

/* --- events (bounded rings; overflow is counted, never silent) ------------- */

typedef struct {
    uint64_t start_ns;           /* episode open (Law 3 stamp)            */
    uint64_t end_ns;             /* episode close                         */
    uint64_t duration_ns;
    uint64_t comutations;        /* consecutive-window co-mutation count  */
    uintptr_t line;              /* the exact 64-byte line                */
    uint32_t off_a;              /* word A offset within its region       */
    uint32_t off_b;              /* word B offset within its region       */
    uint32_t pid_a;              /* owner pid A (0 = unknown class only)  */
    uint32_t pid_b;              /* owner pid B                           */
    char label_a[16];            /* human word labels ("head","tail_ack") */
    char label_b[16];
    uint8_t region;              /* region id                             */
    uint8_t _pad[7];
} weft_prof_fshare_event_t;

typedef struct {
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t duration_ns;
    uint64_t head_at_start;
    uint64_t tail_at_start;
    uint64_t max_in_flight;      /* deepest occupancy during the episode  */
    int32_t seg_idx;
    uint32_t kind;               /* 1 = producer stall (full + frozen)    */
} weft_prof_stall_event_t;

typedef struct {
    int64_t ts_ns;               /* interval close stamp                  */
    uint64_t published_delta;    /* messages committed in the interval    */
    uint64_t dropped_delta;      /* BEST_EFFORT drops in the interval     */
    int32_t seg_idx;
    uint32_t _pad;
} weft_prof_drop_event_t;

/* --- context ---------------------------------------------------------------- */

typedef struct weft_prof_ring_watch {
    int32_t seg_idx;
    /* history: previous pass snapshot */
    uint64_t prev_head, prev_tail, prev_published, prev_dropped;
    /* this pass's mutation flags (captured before prev_* refresh) */
    uint8_t mut_head, mut_tail;
    uint16_t _pad2;
    /* torn tracker */
    uint64_t torn_attempts, torn_events, writer_churn;
    /* stall machine */
    uint8_t stall_state;         /* 0 idle, 1 full, 2 stalled             */
    uint8_t frozen_passes;
    uint16_t _pad0;
    uint64_t stall_start_ns, stall_head_at_start, stall_tail_at_start;
    uint64_t stall_max_in_flight, stall_ns_total;
    uint64_t stalls, reliable_timeout_windows;
    /* ledger */
    uint64_t led_pub_base, led_drop_base;
    uint64_t published_cum, dropped_cum, drop_bursts, last_event_ns;
    /* fshare attribution (pids known by the caller/topology) */
    uint32_t pub_pid, sub_pid;
    uint32_t _pad1;
} weft_prof_ring_watch_t;

typedef struct {
    const _Atomic uint64_t *addr;
    uint64_t last;
    uint32_t off;               /* byte offset within the region         */
    uint8_t owner;              /* weft_prof_owner_t                     */
    uint32_t owner_pid;
    char label[12];
    uint8_t region;             /* owning region id                      */
    uint8_t _pad[3];
} weft_prof_word_t;

typedef struct {
    char label[32];
    uintptr_t base;
    uint8_t word_count;
    uint8_t active;
    uint8_t _pad[2];
    uint32_t owner_default;
} weft_prof_region_t;

typedef struct {
    uint8_t active;
    uint8_t region;
    uint8_t wa, wb;             /* word indices within the region        */
    uint64_t start_ns, last_ns, comutations;
    uintptr_t line;
    uint32_t off_a, off_b, pid_a, pid_b;
} weft_prof_fshare_ep_t;

typedef struct weft_prof_ctx {
    weft_inspect_ctx_t *insp;

    weft_prof_ring_watch_t rings[WEFT_PROF_MAX_RINGS];
    unsigned ring_count;

    weft_prof_region_t regions[WEFT_PROF_MAX_REGIONS];
    weft_prof_word_t words[WEFT_PROF_MAX_WORDS];
    unsigned word_count;

    weft_prof_fshare_ep_t fshare_eps[WEFT_PROF_MAX_FSHARE_EPISODES];

    /* event rings (circular; overwrite-oldest, overflow counted) */
    weft_prof_fshare_event_t fshare_ev[WEFT_PROF_EVENT_RING];
    unsigned fshare_ev_head, fshare_ev_count, fshare_ev_lost;
    weft_prof_stall_event_t stall_ev[WEFT_PROF_EVENT_RING];
    unsigned stall_ev_head, stall_ev_count, stall_ev_lost;
    weft_prof_drop_event_t drop_ev[WEFT_PROF_EVENT_RING];
    unsigned drop_ev_head, drop_ev_count, drop_ev_lost;

    uint64_t passes;
    /* fshare pairs suppressed as still-open episodes are visible via
     * weft_prof_fshare_active() — episodes, not silent drops */
    uint64_t fshare_co_mutations;
} weft_prof_ctx_t;

/* --- lifecycle --------------------------------------------------------------- */

/// Bind the profiler to an inspector context. The inspector must outlive
/// the profiler. Zero allocations.
int weft_prof_init(weft_prof_ctx_t *p, weft_inspect_ctx_t *insp);

/// Watch a WFRM or WFSH ring segment (by inspector index): torn tracker,
/// stall machine, drop ledger, and the four hot ctrl words as a false-
/// sharing region (head/published/dropped = publisher; tail_ack =
/// subscriber). pub_pid/sub_pid fill event attribution (0 = class only).
int weft_prof_watch_ring(weft_prof_ctx_t *p, int seg_idx,
                         uint32_t pub_pid, uint32_t sub_pid);

/// Watch a generic region of u64 words for the false-sharing tripwire:
/// `base` is a mapped address the CALLER owns (any shared mapping), and
/// each word is (offset, owner, owner_pid, label). Offsets must yield
/// 8-aligned addresses. Returns the region id or a negative error.
int weft_prof_watch_region(weft_prof_ctx_t *p, const char *label,
                           const void *base, const uint32_t *offsets,
                           const uint8_t *owners, const uint32_t *owner_pids,
                           const char *const *labels, unsigned n);

/// Stop watching a ring (episode data drains naturally).
int weft_prof_unwatch_ring(weft_prof_ctx_t *p, int seg_idx);

/* --- one telemetry pass -------------------------------------------------------- */

/// Scrape: refreshes the inspector snapshot (one pass), then diffs every
/// watched word, advances the torn tracker (frontier probe), runs the
/// stall state machine, updates the drop ledger, and evolves false-sharing
/// episodes. Zero syscalls, zero allocations, zero stores to shared
/// memory. Returns 0 / -INVALID.
int weft_prof_scrape(weft_prof_ctx_t *p);

/* --- queries ------------------------------------------------------------------- */

/// Per-ring stats (torn/stall/ledger). Returns 0 / -INVALID.
int weft_prof_ring_stats(const weft_prof_ctx_t *p, int seg_idx,
                         weft_prof_ring_stats_t *out);

/// Currently open false-sharing episodes (contention in flight right now).
unsigned weft_prof_fshare_active(const weft_prof_ctx_t *p);

/// Drain up to max closed events of each kind into caller arrays; drained
/// events leave the ring. Returns the count written.
unsigned weft_prof_drain_fshare(weft_prof_ctx_t *p,
                                weft_prof_fshare_event_t *out, unsigned max);
unsigned weft_prof_drain_stalls(weft_prof_ctx_t *p,
                                weft_prof_stall_event_t *out, unsigned max);
unsigned weft_prof_drain_drops(weft_prof_ctx_t *p,
                               weft_prof_drop_event_t *out, unsigned max);

/// Total events lost to ring overflow since init (honesty counter).
uint64_t weft_prof_events_lost(const weft_prof_ctx_t *p);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_INSPECT__CONTENTION_PROFILER_H_ */
