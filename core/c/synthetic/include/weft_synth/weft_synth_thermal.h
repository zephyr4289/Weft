// weft_synth_thermal.h — Synthetic Thermal Throttler (Pillar 8, module A).
//
// WHY EXISTS: real-time pipelines are tuned on a cold bench and die on a
// hot phone. This engine emulates mobile/embedded CPU thermal throttling —
// frequency stepping from 3.2 GHz down to 800 MHz, thermal core migration,
// and dynamic clock perturbation — so weft-cadence's drop-not-queue policy
// can be validated under CPU starvation inside CI, without a thermal
// chamber. It is a lumped-capacitance behavioral model: temperature is one
// node, heat accumulates with load, dissipates per tick, and stepping
// follows hysteresis thresholds exactly like a DVFS governor's.
//
// LAWS CARRIED HERE:
//   Law 1  zero heap: the engine is a caller-allocated struct; no function
//          in this module allocates, ever (batteries interpose the
//          allocator to prove it around the hot admit path).
//   Law 2  determinism: the thermal trajectory is a pure function of
//          (config, load script) — all randomness flows from the seeded
//          engine PRNG, no wall-clock reads inside the model. Same seed,
//          same curve, bit-for-bit (state-hash verified).
//   Law 4  strict C11 portability, both architectures, -Werror -pedantic.
//   Law 5  weft_synth_thermal_* symbols only.
//
// SCALING SEMANTICS (read before wiring deadlines):
//   Let f = current effective frequency (MHz, jitter included), F = max.
//     weft_synth_thermal_work_ns(base)      = base * F / f
//          — wall-clock time the same instruction budget now takes.
//     weft_synth_thermal_deadline_ns(base)  = base * f / F
//          — the frame deadline the throttler HANDS to cadence: the
//          compute budget (in max-frequency units) still available before
//          the wall-clock frame period expires. This is the "dynamically
//          scaled frame deadline" of the directive.
//     weft_synth_thermal_quantum_ns(base)   = base * F / f
//          — worker thread quantum stretched by the same slowdown.
//   The admission probe compares in WALL time (now + work <= period),
//   which is equivalent to base_work <= scaled budget; both magnitudes are
//   reported in the verdict so tests can assert either.
//
// DROP-NOT-QUEUE CONTRACT (models weft-cadence's frozen policy — we do not
// patch the cadence engine, we hold it to its contract):
//   a frame is either ADMITTED with a completion time inside its deadline
//   or DROPPED at admission time (deadline would be missed, or the ring is
//   at capacity). Nothing ever waits in a queue for a deadline it already
//   lost; latency debt therefore cannot accumulate (backlog stays 0), and
//   queue depth never exceeds capacity. Both invariants are tracked in the
//   engine and asserted by the battery under deep throttle.

#ifndef WEFT_SYNTH__THERMAL_H_
#define WEFT_SYNTH__THERMAL_H_

#include "weft_synth_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- configuration -------------------------------------------------------- */

typedef struct weft_synth_thermal_cfg {
    uint32_t seed;                 /* engine PRNG seed (Law 2)             */
    uint32_t freq_max_mhz;         /* 3200 — nominal boost ceiling         */
    uint32_t freq_min_mhz;         /* 800  — thermal floor                 */
    uint32_t freq_step_mhz;        /* 200  — DVFS step granularity         */
    uint32_t temp_ambient_mc;      /* 45000 milli-degC idle floor          */
    uint32_t temp_step_down_mc;    /* 85000 — throttle onset               */
    uint32_t temp_step_up_mc;      /* 70000 — recovery below (hysteresis)  */
    uint32_t temp_critical_mc;     /* 95000 — hard floor frequency         */
    uint32_t temp_cap_mc;          /* 120000 — model ceiling (clamped)     */
    uint32_t heat_full_load_mc;    /* 900 milli-degC per tick at 100% load */
    uint32_t cool_mc;              /* 350 milli-degC per tick, always      */
    uint64_t tick_ns;              /* 1000000 — one model tick = 1 ms      */
    uint32_t hysteresis_ticks;     /* 3 ticks below recovery before step up*/
    uint32_t n_cores;              /* 8 — 0 disables migration             */
    uint32_t migrate_threshold_mc; /* 88000 — hotspot triggers migration   */
    uint32_t migrate_relief_mc;    /* 800 — heat shed by moving to cool core*/
    uint32_t migrate_gap_ticks;    /* 5 — rate limit between migrations    */
    uint32_t clock_jitter_pm;      /* 20 permille = +/-2% per-tick jitter  */
    uint32_t queue_capacity;       /* 8 — cadence ring depth model         */
} weft_synth_thermal_cfg_t;

/// Fill cfg with the directive defaults (3.2 GHz -> 800 MHz stepping,
/// 1 ms ticks, 8 cores). Returns 0 / -INVALID.
int weft_synth_thermal_defaults(weft_synth_thermal_cfg_t *cfg);

/* --- events reported by weft_synth_thermal_step --------------------------- */

enum {
    WEFT_SYNTH_THERMAL_EV_STEP_DOWN = 0x1u,  /* DVFS stepped down          */
    WEFT_SYNTH_THERMAL_EV_STEP_UP   = 0x2u,  /* DVFS stepped up            */
    WEFT_SYNTH_THERMAL_EV_MIGRATE   = 0x4u,  /* core migration fired       */
    WEFT_SYNTH_THERMAL_EV_CRITICAL  = 0x8u,  /* critical temp hard floor   */
};

/* --- engine (caller-allocated; inspectable) ------------------------------- */

typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_thermal {
    weft_synth_thermal_cfg_t cfg;
    uint64_t rng;              /* engine PRNG stream (Law 2)              */
    uint64_t tick_now_ns;      /* simulated now (ticks * tick_ns)         */
    uint32_t temp_mc;          /* milli-degC, ambient..cap                */
    uint32_t freq_mhz;         /* current DVFS level                      */
    uint32_t jitter_mult_pm;    /* per-mille clock multiplier (1000=nom) */
    uint32_t active_core;      /* 0..n_cores-1                            */
    uint32_t below_ticks;      /* hysteresis accumulator                  */
    uint32_t ticks_since_migrate;
    /* cadence drop-not-queue model */
    uint32_t queue_depth;      /* admitted-but-not-yet-completed frames   */
    /* stats ledger */
    uint64_t steps_down;
    uint64_t steps_up;
    uint64_t migrations;
    uint64_t frames_evaluated;
    uint64_t frames_admitted;
    uint64_t frames_dropped;         /* deadline + queue-full drops        */
    uint64_t drops_deadline;         /* would miss: dropped at admit       */
    uint64_t drops_queue_full;       /* ring at capacity: dropped at admit */
    int64_t  latency_backlog_ns;     /* OVERSHOOT debt — invariant: 0      */
    uint32_t queue_depth_hiwat;      /* max queue depth ever observed      */
} weft_synth_thermal_t;

/// Bind an engine over a config. Frequency starts at max, temperature at
/// ambient. Returns 0 / -INVALID (bad cfg: min>max, step 0, tick 0...).
int weft_synth_thermal_init(weft_synth_thermal_t *th,
                            const weft_synth_thermal_cfg_t *cfg);

/// Advance ONE model tick under load_permille in [0, 1000]. Updates
/// temperature (heat in, cooling out, clamped), DVFS level with
/// hysteresis, effective clock jitter, and core migration. Returns the
/// event bitmask of this tick. Pure model step — zero syscalls, zero heap.
uint32_t weft_synth_thermal_step(weft_synth_thermal_t *th,
                                 uint32_t load_permille);

/* --- hot-path hooks (pure arithmetic; Law 1) ------------------------------ */

/// Current effective frequency in MHz (DVFS level +/- this tick's jitter).
uint32_t weft_synth_thermal_freq_mhz(const weft_synth_thermal_t *th);

/// Scaled frame deadline (compute budget) — see SCALING SEMANTICS above.
uint64_t weft_synth_thermal_deadline_ns(const weft_synth_thermal_t *th,
                                        uint64_t base_deadline_ns);

/// Wall-clock inflation of a work budget at the current frequency.
uint64_t weft_synth_thermal_work_ns(const weft_synth_thermal_t *th,
                                    uint64_t base_work_ns);

/// Worker quantum stretched by the current slowdown.
uint64_t weft_synth_thermal_quantum_ns(const weft_synth_thermal_t *th,
                                       uint64_t base_quantum_ns);

/* --- cadence drop-not-queue admission probe -------------------------------- */

enum {
    WEFT_SYNTH_THERMAL_ADMIT = 0,        /* queued, completion inside dl   */
    WEFT_SYNTH_THERMAL_DROP_DEADLINE = 1,/* would miss -> dropped now      */
    WEFT_SYNTH_THERMAL_DROP_QUEUE_FULL = 2,/* ring at capacity -> dropped  */
};

typedef struct weft_synth_thermal_verdict {
    int      verdict;          /* WEFT_SYNTH_THERMAL_* code above         */
    uint64_t scaled_deadline_ns; /* throttler-handed budget (f/F scaled)   */
    uint64_t wall_deadline_ns;  /* the fixed wall-clock frame period       */
    uint64_t work_wall_ns;     /* base work inflated to wall time         */
    uint64_t complete_by_ns;   /* admitted: now + work_wall               */
    uint32_t queue_depth_after;
} weft_synth_thermal_verdict_t;

/// One admission decision (hot path — zero heap, zero syscalls). now_ns is
/// the caller's simulated clock. The frame is admitted iff the ring has
/// room AND now + wall work <= base_deadline_ns (the frame period is
/// wall-clock fixed). Updates the engine's queue model and stats.
int weft_synth_thermal_frame_admit(weft_synth_thermal_t *th,
                                   uint64_t now_ns,
                                   uint64_t base_work_ns,
                                   uint64_t base_deadline_ns,
                                   weft_synth_thermal_verdict_t *out);

/// Mark an admitted frame complete at complete_by_ns. Adds any deadline
/// overshoot to latency_backlog_ns (the invariant says it stays 0: the
/// probe never admits a frame that would overshoot) and releases the
/// queue slot. Returns 0 / -INVALID.
int weft_synth_thermal_frame_complete(weft_synth_thermal_t *th,
                                      const weft_synth_thermal_verdict_t *v);

/// Determinism fingerprint over the full model state (config, PRNG,
/// thermal state, stats). Two engines fed the same script and seed must
/// produce identical hashes at every checkpoint.
uint64_t weft_synth_thermal_state_hash(const weft_synth_thermal_t *th);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_SYNTH__THERMAL_H_ */
