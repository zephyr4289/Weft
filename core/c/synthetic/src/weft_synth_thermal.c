// weft_synth_thermal.c — Synthetic Thermal Throttler (Pillar 8, module A).
//
// Implementation notes (the WHY lives in the header):
//   * one lumped thermal node: temp += heat(load) - cool, clamped to
//     [ambient, cap]. Full-load net heating with the defaults is
//     +550 milli-degC/tick, so 45C -> 85C in ~73 ms of sustained load and
//     idle recovery 85C -> 70C in ~43 ms — a phone-shaped curve, not a
//     datasheet curve.
//   * DVFS with hysteresis: step DOWN the moment temp crosses the throttle
//     onset, step UP only after hysteresis_ticks consecutive ticks below
//     the recovery line (a governor that flaps on the boundary would make
//     cadence validation noise, not signal).
//   * critical temperature collapses straight to the frequency floor in a
//     single tick (counted as one step_down event plus EV_CRITICAL) — this
//     is the only place stepping is coarser than freq_step_mhz.
//   * core migration runs BEFORE the stepping decision so the relief a
//     migration sheds can legitimately avoid a step-down that tick.
//   * the per-tick clock multiplier is drawn from the engine PRNG with
//     rejection-free modulo (2*pm+1 draws) — deterministic, no wall clock
//     anywhere in the model.

#include "weft_synth/weft_synth_thermal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* configuration                                                        */
/* ------------------------------------------------------------------ */

int weft_synth_thermal_defaults(weft_synth_thermal_cfg_t *cfg) {
    if (cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    cfg->seed = 1u;
    cfg->freq_max_mhz = 3200u;
    cfg->freq_min_mhz = 800u;
    cfg->freq_step_mhz = 200u;
    cfg->temp_ambient_mc = 45000u;
    cfg->temp_step_down_mc = 85000u;
    cfg->temp_step_up_mc = 70000u;
    cfg->temp_critical_mc = 95000u;
    cfg->temp_cap_mc = 120000u;
    cfg->heat_full_load_mc = 900u;
    cfg->cool_mc = 350u;
    cfg->tick_ns = 1000000ull;  /* 1 ms */
    cfg->hysteresis_ticks = 3u;
    cfg->n_cores = 8u;
    cfg->migrate_threshold_mc = 88000u;
    cfg->migrate_relief_mc = 800u;
    cfg->migrate_gap_ticks = 5u;
    cfg->clock_jitter_pm = 20u;
    cfg->queue_capacity = 8u;
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* engine lifecycle                                                     */
/* ------------------------------------------------------------------ */

int weft_synth_thermal_init(weft_synth_thermal_t *th,
                            const weft_synth_thermal_cfg_t *cfg) {
    if (th == NULL || cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (cfg->freq_min_mhz == 0u || cfg->freq_max_mhz < cfg->freq_min_mhz ||
        cfg->freq_step_mhz == 0u ||
        cfg->freq_step_mhz > cfg->freq_max_mhz - cfg->freq_min_mhz) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->tick_ns == 0ull || cfg->queue_capacity == 0u ||
        cfg->n_cores > 4096u || cfg->clock_jitter_pm > 200u) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (!(cfg->temp_ambient_mc <= cfg->temp_step_up_mc &&
          cfg->temp_step_up_mc < cfg->temp_step_down_mc &&
          cfg->temp_step_down_mc <= cfg->temp_critical_mc &&
          cfg->temp_critical_mc <= cfg->temp_cap_mc)) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->migrate_threshold_mc < cfg->temp_step_down_mc ||
        cfg->migrate_threshold_mc > cfg->temp_cap_mc) {
        return WEFT_SYNTH_ERR_RANGE;
    }

    memset(th, 0, sizeof(*th));
    th->cfg = *cfg;
    th->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)cfg->seed;
    (void)weft_synth_splitmix64(&th->rng);  /* warm the stream */
    th->temp_mc = cfg->temp_ambient_mc;
    th->freq_mhz = cfg->freq_max_mhz;
    th->jitter_mult_pm = 1000u;
    th->active_core = 0u;
    th->below_ticks = 0u;
    th->ticks_since_migrate = cfg->migrate_gap_ticks;
    th->queue_depth = 0u;
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* model step                                                           */
/* ------------------------------------------------------------------ */

uint32_t weft_synth_thermal_step(weft_synth_thermal_t *th,
                                 uint32_t load_permille) {
    if (th == NULL) {
        return 0u;
    }
    uint32_t events = 0u;
    const weft_synth_thermal_cfg_t *c = &th->cfg;
    if (load_permille > 1000u) {
        load_permille = 1000u;
    }

    /* thermal node: heat in, cool out, clamp to [ambient, cap] */
    uint64_t t = (uint64_t)th->temp_mc +
                 ((uint64_t)c->heat_full_load_mc * load_permille + 500u) / 1000u;
    if (t > c->cool_mc) {
        t -= c->cool_mc;
    } else {
        t = c->temp_ambient_mc;
    }
    if (t < c->temp_ambient_mc) {
        t = c->temp_ambient_mc;
    }
    if (t > c->temp_cap_mc) {
        t = c->temp_cap_mc;
    }
    th->temp_mc = (uint32_t)t;

    /* core migration BEFORE the stepping decision: the relief it sheds
     * can legitimately avoid a step-down this tick */
    if (c->n_cores > 1u && th->temp_mc >= c->migrate_threshold_mc &&
        th->ticks_since_migrate >= c->migrate_gap_ticks) {
        th->active_core =
            (uint32_t)(weft_synth_xorshift64(&th->rng) % c->n_cores);
        uint64_t relieved = (uint64_t)th->temp_mc - c->migrate_relief_mc;
        th->temp_mc = (uint32_t)(relieved < c->temp_ambient_mc
                                     ? c->temp_ambient_mc
                                     : relieved);
        th->migrations++;
        th->ticks_since_migrate = 0u;
        events |= WEFT_SYNTH_THERMAL_EV_MIGRATE;
    } else {
        th->ticks_since_migrate++;
    }

    /* DVFS decision with hysteresis */
    if (th->temp_mc >= c->temp_critical_mc) {
        events |= WEFT_SYNTH_THERMAL_EV_CRITICAL;  /* emergency state */
        if (th->freq_mhz > c->freq_min_mhz) {
            /* critical collapse: straight to the floor in one tick — the
             * only place stepping is coarser than freq_step_mhz */
            th->freq_mhz = c->freq_min_mhz;
            th->steps_down++;
            th->below_ticks = 0u;
            events |= WEFT_SYNTH_THERMAL_EV_STEP_DOWN;
        }
    } else if (th->temp_mc >= c->temp_step_down_mc) {
        th->below_ticks = 0u;
        if (th->freq_mhz > c->freq_min_mhz) {
            uint32_t next = th->freq_mhz - c->freq_step_mhz;
            th->freq_mhz = (next < c->freq_min_mhz) ? c->freq_min_mhz : next;
            th->steps_down++;
            events |= WEFT_SYNTH_THERMAL_EV_STEP_DOWN;
        }
    } else if (th->temp_mc <= c->temp_step_up_mc) {
        th->below_ticks++;
        if (th->below_ticks >= c->hysteresis_ticks &&
            th->freq_mhz < c->freq_max_mhz) {
            uint32_t next = th->freq_mhz + c->freq_step_mhz;
            th->freq_mhz = (next > c->freq_max_mhz) ? c->freq_max_mhz : next;
            th->steps_up++;
            th->below_ticks = 0u;
            events |= WEFT_SYNTH_THERMAL_EV_STEP_UP;
        }
    } else {
        th->below_ticks = 0u;  /* mid-band resets the hysteresis window */
    }

    /* dynamic clock perturbation: per-mille multiplier around nominal */
    uint32_t pm = c->clock_jitter_pm;
    if (pm > 0u) {
        uint32_t span = 2u * pm + 1u;
        uint32_t off = (uint32_t)(weft_synth_xorshift64(&th->rng) % span);
        th->jitter_mult_pm = 1000u + off - pm;
    } else {
        th->jitter_mult_pm = 1000u;
    }

    th->tick_now_ns += c->tick_ns;
    return events;
}

/* ------------------------------------------------------------------ */
/* hot-path hooks (pure arithmetic)                                     */
/* ------------------------------------------------------------------ */

static uint64_t weft_synth_thermal_eff_milli(const weft_synth_thermal_t *th) {
    /* effective frequency in milli-MHz: mhz * per-mille multiplier.
     * 3200 MHz at mult 1000 = 3,200,000 milli-MHz (units matter: the
     * per-mille factor is dimensionless, so NO extra division here). */
    return (uint64_t)th->freq_mhz * (uint64_t)th->jitter_mult_pm;
}

uint32_t weft_synth_thermal_freq_mhz(const weft_synth_thermal_t *th) {
    if (th == NULL) {
        return 0u;
    }
    return (uint32_t)((weft_synth_thermal_eff_milli(th) + 500u) / 1000u);
}

uint64_t weft_synth_thermal_deadline_ns(const weft_synth_thermal_t *th,
                                        uint64_t base_deadline_ns) {
    if (th == NULL || (th->freq_mhz == th->cfg.freq_max_mhz &&
                       th->jitter_mult_pm == 1000u)) {
        /* fast path: at nominal frequency this hook is the identity */
        return base_deadline_ns;
    }
    uint64_t eff = weft_synth_thermal_eff_milli(th);
    uint64_t fmax = (uint64_t)th->cfg.freq_max_mhz * 1000u;
    return (base_deadline_ns * eff + fmax / 2u) / fmax;
}

uint64_t weft_synth_thermal_work_ns(const weft_synth_thermal_t *th,
                                    uint64_t base_work_ns) {
    if (th == NULL || (th->freq_mhz == th->cfg.freq_max_mhz &&
                       th->jitter_mult_pm == 1000u)) {
        return base_work_ns;
    }
    uint64_t eff = weft_synth_thermal_eff_milli(th);
    uint64_t fmax = (uint64_t)th->cfg.freq_max_mhz * 1000u;
    return (base_work_ns * fmax + eff / 2u) / eff;
}

uint64_t weft_synth_thermal_quantum_ns(const weft_synth_thermal_t *th,
                                       uint64_t base_quantum_ns) {
    return weft_synth_thermal_work_ns(th, base_quantum_ns);
}

/* ------------------------------------------------------------------ */
/* cadence drop-not-queue admission probe                               */
/* ------------------------------------------------------------------ */

int weft_synth_thermal_frame_admit(weft_synth_thermal_t *th,
                                   uint64_t now_ns,
                                   uint64_t base_work_ns,
                                   uint64_t base_deadline_ns,
                                   weft_synth_thermal_verdict_t *out) {
    if (th == NULL || out == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    th->frames_evaluated++;

    out->verdict = WEFT_SYNTH_THERMAL_ADMIT;
    out->scaled_deadline_ns =
        weft_synth_thermal_deadline_ns(th, base_deadline_ns);
    out->wall_deadline_ns = base_deadline_ns;
    out->work_wall_ns = weft_synth_thermal_work_ns(th, base_work_ns);
    out->complete_by_ns = 0u;
    out->queue_depth_after = th->queue_depth;

    if (th->queue_depth >= th->cfg.queue_capacity) {
        out->verdict = WEFT_SYNTH_THERMAL_DROP_QUEUE_FULL;
        th->frames_dropped++;
        th->drops_queue_full++;
        return WEFT_SYNTH_OK;
    }
    if (now_ns + out->work_wall_ns > base_deadline_ns) {
        out->verdict = WEFT_SYNTH_THERMAL_DROP_DEADLINE;
        th->frames_dropped++;
        th->drops_deadline++;
        return WEFT_SYNTH_OK;
    }

    out->complete_by_ns = now_ns + out->work_wall_ns;
    th->queue_depth++;
    if (th->queue_depth > th->queue_depth_hiwat) {
        th->queue_depth_hiwat = th->queue_depth;
    }
    out->queue_depth_after = th->queue_depth;
    th->frames_admitted++;
    return WEFT_SYNTH_OK;
}

int weft_synth_thermal_frame_complete(weft_synth_thermal_t *th,
                                      const weft_synth_thermal_verdict_t *v) {
    if (th == NULL || v == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (v->verdict != WEFT_SYNTH_THERMAL_ADMIT) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (th->queue_depth > 0u) {
        th->queue_depth--;
    }
    /* Policy invariant: an admitted frame ALWAYS completes inside its wall
     * deadline, so the overshoot term is identically zero. The counter
     * exists so a policy regression (queue-then-miss) fails the battery
     * loudly instead of silently accruing latency debt. */
    if (v->complete_by_ns > v->wall_deadline_ns) {
        th->latency_backlog_ns +=
            (int64_t)(v->complete_by_ns - v->wall_deadline_ns);
    }
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* determinism fingerprint                                              */
/* ------------------------------------------------------------------ */

uint64_t weft_synth_thermal_state_hash(const weft_synth_thermal_t *th) {
    if (th == NULL) {
        return 0ull;
    }
    uint64_t h = 0xCBF29CE484222325ull;
    const weft_synth_thermal_cfg_t *c = &th->cfg;
    h = weft_synth_fnv1a(h, c->seed);
    h = weft_synth_fnv1a(h, c->freq_max_mhz);
    h = weft_synth_fnv1a(h, c->freq_min_mhz);
    h = weft_synth_fnv1a(h, c->freq_step_mhz);
    h = weft_synth_fnv1a(h, c->temp_ambient_mc);
    h = weft_synth_fnv1a(h, c->temp_step_down_mc);
    h = weft_synth_fnv1a(h, c->temp_step_up_mc);
    h = weft_synth_fnv1a(h, c->temp_critical_mc);
    h = weft_synth_fnv1a(h, c->temp_cap_mc);
    h = weft_synth_fnv1a(h, c->heat_full_load_mc);
    h = weft_synth_fnv1a(h, c->cool_mc);
    h = weft_synth_fnv1a(h, c->tick_ns);
    h = weft_synth_fnv1a(h, c->hysteresis_ticks);
    h = weft_synth_fnv1a(h, c->n_cores);
    h = weft_synth_fnv1a(h, c->migrate_threshold_mc);
    h = weft_synth_fnv1a(h, c->migrate_relief_mc);
    h = weft_synth_fnv1a(h, c->migrate_gap_ticks);
    h = weft_synth_fnv1a(h, c->clock_jitter_pm);
    h = weft_synth_fnv1a(h, c->queue_capacity);
    h = weft_synth_fnv1a(h, th->rng);
    h = weft_synth_fnv1a(h, th->tick_now_ns);
    h = weft_synth_fnv1a(h, th->temp_mc);
    h = weft_synth_fnv1a(h, th->freq_mhz);
    h = weft_synth_fnv1a(h, th->jitter_mult_pm);
    h = weft_synth_fnv1a(h, th->active_core);
    h = weft_synth_fnv1a(h, th->below_ticks);
    h = weft_synth_fnv1a(h, th->ticks_since_migrate);
    h = weft_synth_fnv1a(h, th->queue_depth);
    h = weft_synth_fnv1a(h, th->steps_down);
    h = weft_synth_fnv1a(h, th->steps_up);
    h = weft_synth_fnv1a(h, th->migrations);
    h = weft_synth_fnv1a(h, th->frames_evaluated);
    h = weft_synth_fnv1a(h, th->frames_admitted);
    h = weft_synth_fnv1a(h, th->frames_dropped);
    h = weft_synth_fnv1a(h, th->drops_deadline);
    h = weft_synth_fnv1a(h, th->drops_queue_full);
    h = weft_synth_fnv1a(h, (uint64_t)th->latency_backlog_ns);
    h = weft_synth_fnv1a(h, th->queue_depth_hiwat);
    return h;
}
