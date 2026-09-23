// weft_synth_bus.h — Memory-Bus Saturation & Cache Contention Tester
// (Pillar 8, module B).
//
// WHY EXISTS: lock-free code that looks fine on an idle desktop dies under
// memory-bus saturation on real silicon. This module DELIBERATELY creates
// that physics: hammer threads issue bounded bursts of relaxed RMWs onto
// chosen words of a caller-owned arena so that adjacent 64B/128B cache
// lines coherency-bounce — a blender the rest of the lab can run while
// measuring something else. The same module carries the seqlock retry
// probe: a reference seqlock whose readers are instrumented per-attempt,
// so the directive's SLA — p99 retry-loop latency < 100 ns under MAXIMUM
// bus saturation, with bounded forward progress and zero deadlocks — is
// a measured number, not a hope.
//
// LAWS CARRIED HERE:
//   Law 1  zero heap: the blender and probe are caller-allocated structs
//          over a caller-provided arena; hammer loops and probe reads
//          never allocate (batteries interpose the allocator to prove it).
//          The one-shot A/B measurement helper allocates its own arena at
//          SETUP time and frees it — never in a hot loop (declared).
//   Law 2  determinism: word placement is config-derived; the probe's only
//          randomness (payload verification mixes) flows from its seed.
//   Law 3  bounded forward progress: hammer bursts are finite and re-check
//          the stop flag with acquire loads; the seqlock reader caps spin
//          attempts and escalates to sched_yield() — saturation stretches
//          latency, it can never wedge the loop.
//   Law 4  strict C11 on x86_64 + aarch64; the seqlock payload is accessed
//          through _Atomic relaxed loads/stores ordered by version fences
//          (the classic Linux seqlock discipline), which keeps the pattern
//          data-race-free under TSan by construction.
//   Law 5  weft_synth_bus_* / weft_synth_seqlock_* / weft_synth_probe_*
//          symbols only.
//
// HONESTY BOUNDARY: "bus saturation" here is user-space cache-line
// hammering — it saturates coherency traffic on the lines it touches and
// the memory-bus bandwidth of RMW streams, which is the interference class
// lock-free rings actually suffer; it is not a PCIe/DMA contention model.

#ifndef WEFT_SYNTH__BUS_H_
#define WEFT_SYNTH__BUS_H_

#include <pthread.h>

#include "weft_synth_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- blender configuration ----------------------------------------------- */

#define WEFT_SYNTH_BUS_MAX_THREADS 32u

typedef enum {
    WEFT_SYNTH_BUS_ISOLATED = 0,  /* each thread its own line — baseline   */
    WEFT_SYNTH_BUS_ADJACENT = 1,  /* distinct words, ONE shared line — the
                                     classic false-sharing penalty         */
    WEFT_SYNTH_BUS_FALSE_SHARE = 2, /* same word — true sharing contention */
} weft_synth_bus_mode_t;

typedef struct weft_synth_bus_cfg {
    uint32_t n_threads;        /* hammer threads, 1..MAX_THREADS           */
    uint32_t line_bytes;       /* 64 or 128 (validated)                    */
    uint32_t n_lines;          /* arena extent in lines                    */
    uint32_t burst_iters;      /* RMWs per burst before stop-flag re-check */
    uint64_t max_ops_per_thread; /* 0 = run until stop(); else exact quota */
    uint32_t seed;             /* reserved for placement jitter (Law 2)    */
    weft_synth_bus_mode_t mode;
} weft_synth_bus_cfg_t;

/// Directive defaults: 4 threads (clamped by callers to CPU count), 64B
/// lines, adjacent-line false sharing, 256-iteration bursts.
int weft_synth_bus_defaults(weft_synth_bus_cfg_t *cfg);

/// Arena size the configuration needs, in BYTES (line_bytes * n_lines).
size_t weft_synth_bus_arena_bytes(const weft_synth_bus_cfg_t *cfg);

/* --- blender (caller-allocated over a caller-owned atomic arena) --------- */

struct weft_synth_bus_blender;
typedef struct weft_synth_bus_hammer_arg {
    struct weft_synth_bus_blender *b;  /* owning blender                */
    uint32_t idx;                     /* this hammer thread's index    */
} weft_synth_bus_hammer_arg_t;

typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_bus_blender {
    weft_synth_bus_cfg_t cfg;
    _Atomic uint64_t *arena;          /* caller-owned, line_bytes aligned  */
    size_t arena_words;
    uint32_t word_map[WEFT_SYNTH_BUS_MAX_THREADS]; /* per-thread RMW word */
    _Atomic int stop_flag;
    _Atomic uint64_t totals[WEFT_SYNTH_BUS_MAX_THREADS];
    pthread_t threads[WEFT_SYNTH_BUS_MAX_THREADS];
    weft_synth_bus_hammer_arg_t
        hammer_args[WEFT_SYNTH_BUS_MAX_THREADS];  /* per-blender, no
                                                      shared statics      */
    uint64_t ops_total;               /* summed at stop()                  */
    int running;
} weft_synth_bus_blender_t;

/// Bind the blender over an arena of _Atomic uint64_t words. The arena
/// must be aligned to line_bytes and hold weft_synth_bus_arena_bytes().
/// Returns 0 / -INVALID / -RANGE (mode/threads/arena mismatch).
int weft_synth_bus_blender_init(weft_synth_bus_blender_t *b,
                                const weft_synth_bus_cfg_t *cfg,
                                _Atomic uint64_t *arena,
                                size_t arena_words);

/// Spawn the hammer threads. Returns 0 / -BUS (already running) / -SYS.
int weft_synth_bus_blender_start(weft_synth_bus_blender_t *b);

/// Signal stop, join every hammer thread, sum ops_total. Returns ops_total.
uint64_t weft_synth_bus_blender_stop(weft_synth_bus_blender_t *b);

/// Detach state (does not free anything — the caller owns all memory).
void weft_synth_bus_blender_destroy(weft_synth_bus_blender_t *b);

/* --- false-sharing A/B measurement (setup-time allocation, declared) ----- */

typedef struct weft_synth_bus_share_stat {
    double isolated_ns_per_op;  /* one thread per line (baseline)          */
    double shared_ns_per_op;    /* ADJACENT: distinct words, one line      */
    double ratio;               /* shared / isolated (> 1 = penalty)       */
    uint64_t ops_per_thread;
    uint32_t n_threads;
    uint32_t line_bytes;
} weft_synth_bus_share_stat_t;

/// One-shot experiment: run the blender in ADJACENT then ISOLATED mode for
/// ops_per_thread RMWs per thread and report ns/op for both plus the
/// penalty ratio. Allocates and frees its own arena around the runs (never
/// inside a hot loop — Law 1 declaration). Single-line penalty presence
/// requires >= 2 online CPUs to be observable; the battery gates on that.
int weft_synth_bus_measure_false_share(uint32_t n_threads,
                                       uint64_t ops_per_thread,
                                       weft_synth_bus_share_stat_t *out);

/* --- reference seqlock (the retry-loop SLA carrier) ----------------------- */

/// Seqlock with the classic Linux discipline: version odd = writing.
/// Payload words are _Atomic accessed relaxed, ordered by the version
/// acquire loads and thread fences — race-free under TSan by construction
/// while preserving seqlock semantics (reader may retry, never blocks).
typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_seqlock {
    _Atomic uint64_t version;
    _Atomic uint64_t payload[4];
} weft_synth_seqlock_t;

/// Initialize to a stable published state (version 0, zero payload).
int weft_synth_seqlock_init(weft_synth_seqlock_t *sl);

/// Bounded critical section: publish odd, fence, write 4 words, fence,
/// publish even. The section is 4 relaxed stores wide — that bound is
/// what guarantees reader forward progress (Law 3).
void weft_synth_seqlock_write(weft_synth_seqlock_t *sl,
                              const uint64_t payload[4]);

/* --- instrumented reader probe -------------------------------------------- */

#define WEFT_SYNTH_PROBE_MAX_ATTEMPTS 1024u  /* total attempts before hard   */
                                             /* escalation (yield + restart) */
#define WEFT_SYNTH_PROBE_YIELD_AFTER 256u    /* spin attempts per yield phase */

typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_bus_probe {
    uint64_t rng;             /* verification-mix stream (Law 2)            */
    uint64_t *lat;            /* caller scratch: per-ATTEMPT latency (ns) —
                                one retry-loop iteration each; this is the
                                population of the < 100 ns p99 SLA        */
    uint32_t lat_cap;
    uint32_t lat_len;
    uint64_t reads;
    uint64_t retries;         /* failed attempts (odd version / torn)       */
    uint64_t yields;          /* sched_yield escalations                    */
    uint32_t max_attempts;    /* worst read seen                            */
    uint64_t merged_dropped;  /* samples that did not fit on merge (honest) */
} weft_synth_bus_probe_t;

/// Bind a probe over caller scratch (lat_cap samples; more reads are
/// counted, not stored). Call weft_synth_cycle_calibrate() first — probe
/// latencies are cycle-measured and converted with the calibrated rate.
int weft_synth_bus_probe_init(weft_synth_bus_probe_t *p, uint32_t seed,
                              uint64_t *latency_scratch,
                              uint32_t scratch_cap);

/// One instrumented seqlock read: first-attempt to success, measured in
/// cycles and converted to ns. Retries are counted per failed attempt;
/// after WEFT_SYNTH_PROBE_YIELD_AFTER consecutive failed spins the probe
/// yields the CPU once (counted), resetting the spin phase — so the loop
/// is bounded and cooperative under maximum saturation. Returns the total
/// attempt count (>= 1; the read ALWAYS completes — forward progress is
/// structural, the writer section is 4 stores wide). out_payload may be
/// NULL.
int weft_synth_bus_probe_read(weft_synth_bus_probe_t *p,
                              weft_synth_seqlock_t *sl,
                              uint64_t out_payload[4]);

/// Percentile stats over the recorded PER-ATTEMPT latencies (one sample
/// per retry-loop iteration — the p99 < 100 ns SLA population; p99.9 and
/// max carry the scheduler-preemption tail honestly, ungated). NOTE:
/// sorts the probe's scratch in place (documented; the probe is not
/// const). With no samples, percentiles are 0 and counters still report.
typedef struct weft_synth_probe_stat {
    uint64_t reads;
    uint64_t retries;
    uint64_t yields;
    uint32_t max_attempts;
    double avg_retries_per_read;
    double p50_ns;
    double p99_ns;
    double p999_ns;
    double max_ns;
} weft_synth_probe_stat_t;

void weft_synth_bus_probe_stats(weft_synth_bus_probe_t *p,
                                weft_synth_probe_stat_t *out);

/// Fold src's samples and counters into dst (scratch capacity permitting;
/// overflow is counted in dst->merged_dropped, never silently dropped).
void weft_synth_bus_probe_merge(weft_synth_bus_probe_t *dst,
                                const weft_synth_bus_probe_t *src);

/* --- seqlock writer thread (contention source for the probe) -------------- */

typedef struct weft_synth_seqlock_writer {
    weft_synth_seqlock_t *sl;
    _Atomic int *stop;
    uint64_t rng;             /* payload stream (Law 2)                     */
    uint64_t writes;
    uint32_t period_ns;       /* busy-spaced writes; 0 = flat-out maximum   */
} weft_synth_seqlock_writer_t;

/// Writer loop: publish a fresh 4-word payload, optionally busy-wait
/// period_ns (cycle-calibrated), check stop with acquire loads. Use as a
/// pthread entry with a weft_synth_seqlock_writer_t* argument.
void *weft_synth_seqlock_writer_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_SYNTH__BUS_H_ */
