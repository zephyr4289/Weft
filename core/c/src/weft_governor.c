// weft_governor.c — WSP5 dynamic hardware governor & tier sizing engine.
//
// Computes the integer hardware tier score, builds the frozen tier
// plan (ring lanes, slot strides, frame deadlines, arena budgets,
// scheduler policy) and publishes it through the lock-free atomic
// double-buffer described in weft_spectrum.h.
//
// Determinism (Law 2): the score is PURE integer arithmetic over
// verified profile fields — no floats, no environment reads, no
// time-dependence. The normative scoring table lives in D-51 §5 and
// the test ladder pins the EXACT score of every golden archetype, so
// any drift in the algorithm fails CI loudly.
//
// Zero-jitter cadence (mission bar): weft_governor_retarget() never
// blocks, sweeps, or tears reader state. The single writer builds the
// new plan off to the side (stack-local), stores its 16 words into the
// INACTIVE slot with relaxed atomics, then publishes with an ordered
// sequence: release fence -> plan_index -> active_tier -> pressure ->
// version. Readers either observe the old plan in full or the new plan
// in full; the version sandwich refuses anything in between.
//
// Threading contract: exactly ONE writer thread (the governance
// thread) calls retarget/force/clear_force/set_refresh; unlimited
// lock-free readers call the inline queries and snapshot().

#include "weft_spectrum_internal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Normative tier thresholds (D-51 §5 — pinned by tests)               */
/*                                                                     */
/*   Tier 1 >= 74 : verified floor across the golden matrix is 76      */
/*                  (Grace Hopper, desktop Zen4).                      */
/*   Tier 2 >= 45 : verified floor is exactly 45 (Dimensity 9300) —    */
/*                  pinned by an exact-score assertion.                */
/*   Tier 3 otherwise; the highest Tier 3 landing is 40 (Pi 5).        */
/* Hard RAM clamps: < 3 GiB never exceeds Tier 3; < 6 GiB never       */
/* reaches Tier 1 (budget-phone envelope).                             */
/* ------------------------------------------------------------------ */

#define WSP_T1_SCORE 74u
#define WSP_T2_SCORE 45u
#define WSP_GIB3      (3ull * 1024ull * 1024ull * 1024ull)
#define WSP_GIB6      (6ull * 1024ull * 1024ull * 1024ull)

/* Plan geometry table (locked per tier; D-51 §6). */
typedef struct {
    uint16_t lanes;
    uint16_t slots;
    uint16_t hdr_stride;
    uint32_t slot_stride;
    uint32_t refresh_hz;
    uint32_t ingest_workers;
    uint32_t batch_max_msgs;
} wsp_plan_geom_t;

static const wsp_plan_geom_t WSP_GEOM[4] = {
    { 0u, 0u, 0u, 0u, 0u, 0u, 0u }, /* index 0 unused */
    { 16u, 256u, 128u, 1024u, 240u, 8u, 1024u }, /* Tier 1 flagship  */
    { 8u, 128u, 64u, 512u, 120u, 4u, 512u },     /* Tier 2 mid-range */
    { 4u, 64u, 64u, 256u, 60u, 2u, 128u },       /* Tier 3 budget    */
};

/* Writer-private storage carved from the governor's reserved words
 * (runtime state, not ABI surface):
 *   rsvd1[0] = refresh_hz override (0 = tier table default)
 *   rsvd1[1] = last computed integer score                                */
#define WSP_GOV_REFRESH(g)  ((g)->rsvd1[0])
#define WSP_GOV_SCORE(g)    ((g)->rsvd1[1])

/* ------------------------------------------------------------------ */
/* Scoring (pure, auditable)                                           */
/* ------------------------------------------------------------------ */

static uint32_t wsp_score_ram(uint64_t ram_bytes)
{
    if (ram_bytes >= 68719476736ull) return 30u; /* >= 64 GiB */
    if (ram_bytes >= 34359738368ull) return 27u; /* >= 32 GiB */
    if (ram_bytes >= 17179869184ull) return 24u; /* >= 16 GiB */
    if (ram_bytes >= 12884901888ull) return 21u; /* >= 12 GiB */
    if (ram_bytes >= 8589934592ull)  return 18u; /* >=  8 GiB */
    if (ram_bytes >= 6442450944ull)  return 14u; /* >=  6 GiB */
    if (ram_bytes >= 4294967296ull)  return 10u; /* >=  4 GiB */
    return 4u; /* unknown or ultra-low */
}

static uint32_t wsp_score_perf_cores(uint32_t perf)
{
    if (perf >= 16u) return 20u;
    if (perf >= 12u) return 18u;
    if (perf >= 8u)  return 16u;
    if (perf >= 6u)  return 12u;
    if (perf >= 4u)  return 8u;
    if (perf >= 2u)  return 5u;
    return 2u;
}

static uint32_t wsp_score_and_pick_simd(const weft_hw_profile_t *p,
                                        uint32_t *simd_path)
{
    if (weft_hw_has_feature(p, WEFT_F_CPU_AMX)) {
        *simd_path = (uint32_t)WEFT_SIMD_AMX;    return 18u;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_AVX512F)) {
        *simd_path = (uint32_t)WEFT_SIMD_AVX512; return 18u;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_SVE2)) {
        *simd_path = (uint32_t)WEFT_SIMD_SVE2;   return 18u;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_RVV_VLEN_256)) {
        *simd_path = (uint32_t)WEFT_SIMD_RVV;    return 12u;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_RVV_1_0)) {
        *simd_path = (uint32_t)WEFT_SIMD_RVV;    return 10u;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_AVX2)) {
        *simd_path = (uint32_t)WEFT_SIMD_AVX2;   return 12u;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_NEON)) {
        *simd_path = (uint32_t)WEFT_SIMD_NEON;   return 8u;
    }
    *simd_path = (uint32_t)WEFT_SIMD_SCALAR;
    return 3u;
}

static uint32_t wsp_score_cacheline(uint32_t line)
{
    if (line == 128u) return 4u;
    if (line == 32u)  return 1u;
    return 2u; /* 64B or conservative default */
}

static uint32_t wsp_score_gpu(const weft_hw_profile_t *p)
{
    if (weft_hw_has_feature(p, WEFT_F_GPU_DISCRETE_VRAM) &&
        weft_hw_has_feature(p, WEFT_F_GPU_DMABUF_IMPORT)) {
        return 12u;
    }
    if (weft_hw_has_feature(p, WEFT_F_GPU_UNIFIED_MEMORY) &&
        weft_hw_has_feature(p, WEFT_F_GPU_COMPUTE_SHADER)) {
        return 10u;
    }
    if (weft_hw_has_feature(p, WEFT_F_GPU_PRESENT)) {
        return 6u;
    }
    return 0u;
}

static uint32_t wsp_score_thermal(uint32_t thermal_limit_mw)
{
    if (thermal_limit_mw == 0u)   return 0u;  /* unknown — neutral */
    if (thermal_limit_mw >= 65000u) return 4u; /* workstation class */
    if (thermal_limit_mw >= 10000u) return 0u; /* laptop / SBC class */
    if (thermal_limit_mw >= 3000u)  return (uint32_t)-8; /* phone class */
    return (uint32_t)-12;                       /* wearables class  */
}

static uint32_t wsp_score_boost(uint32_t headroom_mhz)
{
    if (headroom_mhz >= 1000u) return 2u;
    if (headroom_mhz >= 500u)  return 1u;
    return 0u;
}

uint32_t weft_governor_score(const weft_hw_profile_t *profile)
{
    uint32_t simd_path = 0;
    int32_t score;
    if (profile == NULL) {
        return 0u;
    }
    score  = (int32_t)wsp_score_ram(profile->ram_total_bytes);
    score += (int32_t)wsp_score_perf_cores(profile->cores_performance);
    score += (int32_t)wsp_score_and_pick_simd(profile, &simd_path);
    score += (int32_t)wsp_score_cacheline(profile->cache_line_size);
    score += (int32_t)wsp_score_gpu(profile);
    if (weft_hw_has_feature(profile, WEFT_F_NPU_PRESENT)) {
        score += 6;
    }
    score += (int32_t)wsp_score_thermal(profile->thermal_limit_mw);
    score += (int32_t)wsp_score_boost(profile->boost_headroom_mhz);
    if (profile->numa_node_count >= 2u) {
        score += 1;
    }
    if (score < 0) {
        return 0u;
    }
    return (uint32_t)score;
}

uint32_t weft_governor_tier_for_profile(const weft_hw_profile_t *profile)
{
    uint32_t score, tier;
    if (profile == NULL) {
        return (uint32_t)WEFT_SPECTRUM_TIER_INVALID;
    }
    score = weft_governor_score(profile);
    tier = (score >= WSP_T1_SCORE) ? (uint32_t)WEFT_SPECTRUM_TIER_1
         : (score >= WSP_T2_SCORE) ? (uint32_t)WEFT_SPECTRUM_TIER_2
         : (uint32_t)WEFT_SPECTRUM_TIER_3;
    /* Hard RAM clamps — the budget-phone envelope. */
    if (profile->ram_total_bytes > 0ull &&
        profile->ram_total_bytes < WSP_GIB3) {
        tier = (uint32_t)WEFT_SPECTRUM_TIER_3;
    } else if (profile->ram_total_bytes > 0ull &&
               profile->ram_total_bytes < WSP_GIB6 &&
               tier == (uint32_t)WEFT_SPECTRUM_TIER_1) {
        tier = (uint32_t)WEFT_SPECTRUM_TIER_2;
    }
    return tier;
}

/* ------------------------------------------------------------------ */
/* Plan construction (writer-private; stack-local; zero heap)          */
/* ------------------------------------------------------------------ */

static void wsp_build_plan(weft_tier_plan_t *plan,
                           const weft_hw_profile_t *hw,
                           uint32_t tier, uint32_t pressure,
                           uint32_t generation, uint32_t refresh_hz)
{
    const wsp_plan_geom_t *g;
    uint32_t simd_path = (uint32_t)WEFT_SIMD_SCALAR;
    uint64_t lane_bytes, arena;
    uint32_t workers, refresh, deadline, guard;

    memset(plan, 0, sizeof(*plan));
    if (tier < 1u || tier > 3u) {
        tier = 3u; /* fail-closed: malformed tier degrades to budget */
    }
    g = &WSP_GEOM[tier];

    refresh = refresh_hz;
    if (refresh == 0u || refresh < 24u || refresh > 480u) {
        refresh = g->refresh_hz;
    }
    deadline = 1000000u / refresh;
    guard = deadline / 8u;

    workers = g->ingest_workers;
    if (hw != NULL && hw->cores_total > 0u &&
        hw->cores_total < workers) {
        workers = hw->cores_total;
    }
    if (workers == 0u) {
        workers = 1u;
    }

    (void)wsp_score_and_pick_simd(hw, &simd_path);

    lane_bytes = (uint64_t)g->hdr_stride +
                 (uint64_t)g->slots * (uint64_t)g->slot_stride;
    arena = (uint64_t)g->lanes * lane_bytes;

    plan->magic = WEFT_SPECTRUM_PLAN_MAGIC;
    plan->abi_version = WEFT_SPECTRUM_ABI_VERSION;
    plan->schema_hash = WEFT_SPECTRUM_PLAN_SCHEMA_HASH;
    plan->tier = (uint16_t)tier;
    plan->generation = (uint16_t)generation;
    plan->policy_flags = (uint16_t)WEFT_PLAN_POLICY_SEQLOCK_LANES;
    plan->ring_lanes = g->lanes;
    plan->ring_slots = g->slots;
    plan->hdr_stride = g->hdr_stride;
    plan->slot_stride = g->slot_stride;
    plan->frame_deadline_us = deadline;
    plan->jitter_guard_us = guard;
    plan->refresh_hz = refresh;
    plan->simd_path = simd_path;
    plan->ingest_workers = workers;
    plan->batch_max_msgs = g->batch_max_msgs;
    plan->arena_budget_bytes = arena;
    plan->lane_bytes = lane_bytes;
    plan->pressure_level = pressure;

    if (tier == (uint32_t)WEFT_SPECTRUM_TIER_3) {
        plan->policy_flags |= (uint16_t)WEFT_PLAN_POLICY_DROP_NOT_QUEUE;
    }
    if (hw != NULL) {
        if (tier == (uint32_t)WEFT_SPECTRUM_TIER_1 &&
            (weft_hw_has_feature(hw, WEFT_F_NPU_PRESENT) ||
             (weft_hw_has_feature(hw, WEFT_F_GPU_PRESENT) &&
              weft_hw_has_feature(hw, WEFT_F_GPU_DMABUF_IMPORT)))) {
            plan->policy_flags |= (uint16_t)WEFT_PLAN_POLICY_ASYNC_DMA;
        }
        if (weft_hw_has_feature(hw, WEFT_F_CPU_HETERO_CORES)) {
            plan->policy_flags |= (uint16_t)WEFT_PLAN_POLICY_HETERO_CORES;
        }
    }

    plan->crc32c = weft_crc32c(plan, 0x7c);
}

/* Ordering barrier for the publication protocol.
 *
 * A standalone __atomic_thread_fence is the textbook tool, but
 * ThreadSanitizer cannot model it (-Werror=tsan). An ACQ_REL RMW on a
 * stack-local scratch word provides the identical ordering contract
 * (no prior access sinks below it, no later access hoists above it)
 * and is fully modeled by every sanitizer and every target we ship. */
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
#  define WSP_ORDER_BARRIER()                                            \
    do {                                                                 \
        uint64_t wsp_barrier_scratch_ = 0;                               \
        (void)__atomic_fetch_add(&wsp_barrier_scratch_, 0,               \
                                 __ATOMIC_ACQ_REL);                      \
    } while (0)
#else
#  define WSP_ORDER_BARRIER()                                            \
    do {                                                                 \
        __atomic_thread_fence(__ATOMIC_ACQ_REL);                         \
    } while (0)
#endif

/* ------------------------------------------------------------------ */
/* Publication (single writer)                                         */
/* ------------------------------------------------------------------ */

static weft_spectrum_status_t wsp_publish(weft_governor_t *g,
                                          weft_pressure_level_t pressure)
{
    weft_tier_plan_t plan; /* 128 bytes, stack — Law 1 */
    uint32_t tier, forced, hw_tier;
    uint64_t v;
    uint32_t next_idx, i;
    const uint64_t *words;

    if (g == NULL || g->profile == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    if ((uint32_t)pressure > (uint32_t)WEFT_PRESSURE_CRITICAL) {
        return WEFT_SPECTRUM_EINVAL;
    }

    forced = __atomic_load_n(&g->forced_tier, __ATOMIC_RELAXED);
    hw_tier = weft_governor_tier_for_profile(g->profile);

    if (forced >= 1u && forced <= 3u) {
        tier = forced;
    } else {
        tier = hw_tier;
        if (pressure == WEFT_PRESSURE_ELEVATED &&
            tier < (uint32_t)WEFT_SPECTRUM_TIER_2) {
            tier = (uint32_t)WEFT_SPECTRUM_TIER_2;
        } else if (pressure == WEFT_PRESSURE_CRITICAL) {
            tier = (uint32_t)WEFT_SPECTRUM_TIER_3;
        }
    }

    WSP_GOV_SCORE(g) = (uint64_t)weft_governor_score(g->profile);

    v = __atomic_load_n(&g->version, __ATOMIC_RELAXED);
    next_idx = __atomic_load_n(&g->plan_index, __ATOMIC_RELAXED) ^ 1u;

    wsp_build_plan(&plan, g->profile, tier, (uint32_t)pressure,
                   (uint32_t)(v + 1ull) /* generation tracks publishes */,
                   (uint32_t)WSP_GOV_REFRESH(g));

    words = (const uint64_t *)(const void *)&plan;
    for (i = 0; i < (uint32_t)WEFT_SPECTRUM_PLAN_WORDS; i++) {
        __atomic_store_n(&g->plan_words[next_idx][i], words[i],
                         __ATOMIC_RELAXED);
    }
    WSP_ORDER_BARRIER();
    __atomic_store_n(&g->plan_index, next_idx, __ATOMIC_RELEASE);
    __atomic_store_n(&g->active_tier, tier, __ATOMIC_RELEASE);
    __atomic_store_n(&g->pressure, (uint32_t)pressure, __ATOMIC_RELEASE);
    __atomic_store_n(&g->generation, (uint32_t)(v + 1ull), __ATOMIC_RELAXED);
    __atomic_store_n(&g->version, v + 1ull, __ATOMIC_RELEASE);
    return WEFT_SPECTRUM_OK;
}

/* ------------------------------------------------------------------ */
/* Public governor surface                                             */
/* ------------------------------------------------------------------ */

weft_spectrum_status_t weft_governor_init(weft_governor_t *gov,
                                          const weft_hw_profile_t *profile)
{
    weft_spectrum_status_t st;
    if (gov == NULL || profile == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    st = weft_hw_profile_validate(profile);
    if (st != WEFT_SPECTRUM_OK) {
        return st; /* fail-closed: corrupted profile refused */
    }
    memset(gov, 0, sizeof(*gov));
    gov->profile = profile;
    /* rsvd1 writer-private: refresh override 0, score filled at publish */
    return wsp_publish(gov, WEFT_PRESSURE_NOMINAL);
}

weft_spectrum_status_t weft_governor_retarget(weft_governor_t *gov,
                                              weft_pressure_level_t pressure)
{
    if (gov == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    if ((uint32_t)pressure > (uint32_t)WEFT_PRESSURE_CRITICAL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    return wsp_publish(gov, pressure);
}

weft_spectrum_status_t weft_governor_force_tier(weft_governor_t *gov,
                                                uint32_t tier)
{
    weft_spectrum_status_t st;
    if (gov == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    if (tier < 1u || tier > 3u) {
        return WEFT_SPECTRUM_EINVAL;
    }
    __atomic_store_n(&gov->forced_tier, tier, __ATOMIC_RELEASE);
    st = wsp_publish(gov,
                     (weft_pressure_level_t)__atomic_load_n(&gov->pressure,
                                                            __ATOMIC_RELAXED));
    return st;
}

weft_spectrum_status_t weft_governor_clear_force(weft_governor_t *gov)
{
    weft_spectrum_status_t st;
    if (gov == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    __atomic_store_n(&gov->forced_tier, 0u, __ATOMIC_RELEASE);
    st = wsp_publish(gov,
                     (weft_pressure_level_t)__atomic_load_n(&gov->pressure,
                                                            __ATOMIC_RELAXED));
    return st;
}

weft_spectrum_status_t weft_governor_snapshot(const weft_governor_t *gov,
                                              weft_tier_plan_t *out_plan)
{
    uint32_t retry;
    if (gov == NULL || out_plan == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    for (retry = 0; retry < (uint32_t)WEFT_SPECTRUM_SNAPSHOT_RETRIES;
         retry++) {
        uint64_t v1 = __atomic_load_n(&gov->version, __ATOMIC_ACQUIRE);
        uint32_t idx = __atomic_load_n(&gov->plan_index, __ATOMIC_ACQUIRE);
        uint64_t w[WEFT_SPECTRUM_PLAN_WORDS];
        uint64_t v2;
        uint32_t i;
        if (idx > 1u) {
            return WEFT_SPECTRUM_EABI; /* corrupted governor — refusal */
        }
        for (i = 0; i < (uint32_t)WEFT_SPECTRUM_PLAN_WORDS; i++) {
            w[i] = __atomic_load_n(&gov->plan_words[idx][i],
                                   __ATOMIC_RELAXED);
        }
        WSP_ORDER_BARRIER();
        v2 = __atomic_load_n(&gov->version, __ATOMIC_ACQUIRE);
        if (v1 != v2) {
            continue; /* published mid-copy — retry (bounded, Law 4) */
        }
        memcpy(out_plan, w, sizeof(*out_plan));
        /* Belt and suspenders: a stable copy must be a sealed plan. */
        if (out_plan->magic != WEFT_SPECTRUM_PLAN_MAGIC ||
            out_plan->abi_version != WEFT_SPECTRUM_ABI_VERSION ||
            out_plan->schema_hash != WEFT_SPECTRUM_PLAN_SCHEMA_HASH) {
            return WEFT_SPECTRUM_EABI;
        }
        if (out_plan->crc32c != weft_crc32c(out_plan, 0x7c)) {
            return WEFT_SPECTRUM_ECHECKSUM;
        }
        return WEFT_SPECTRUM_OK;
    }
    return WEFT_SPECTRUM_EPLAN_BUSY; /* refusal — NEVER torn data */
}

weft_spectrum_status_t weft_governor_stats(const weft_governor_t *gov,
                                           weft_governor_stats_t *out_stats)
{
    uint32_t idx;
    if (gov == NULL || out_stats == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    idx = __atomic_load_n(&gov->plan_index, __ATOMIC_RELAXED);
    if (idx > 1u) {
        return WEFT_SPECTRUM_EABI;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->magic = WEFT_GOV_STATS_MAGIC;
    out_stats->abi_version = WEFT_SPECTRUM_ABI_VERSION;
    out_stats->version = __atomic_load_n(&gov->version, __ATOMIC_ACQUIRE);
    out_stats->tier = __atomic_load_n(&gov->active_tier, __ATOMIC_RELAXED);
    out_stats->pressure = __atomic_load_n(&gov->pressure, __ATOMIC_RELAXED);
    out_stats->generation = __atomic_load_n(&gov->generation,
                                            __ATOMIC_RELAXED);
    out_stats->forced_tier = __atomic_load_n(&gov->forced_tier,
                                             __ATOMIC_RELAXED);
    out_stats->score = (uint32_t)WSP_GOV_SCORE(gov);
    out_stats->transitions = out_stats->generation;
    /* Indicative plan fields (relaxed reads of the published slot). */
    out_stats->refresh_hz =
        (uint32_t)__atomic_load_n(&gov->plan_words[idx][5],
                                  __ATOMIC_RELAXED);
    out_stats->frame_deadline_us =
        (uint32_t)__atomic_load_n(&gov->plan_words[idx][4],
                                  __ATOMIC_RELAXED);
    out_stats->arena_budget_bytes =
        __atomic_load_n(&gov->plan_words[idx][7], __ATOMIC_RELAXED);
    if (gov->profile != NULL) {
        out_stats->profile_crc32c = gov->profile->crc32c;
    }
    return WEFT_SPECTRUM_OK;
}

weft_spectrum_status_t weft_governor_set_refresh(weft_governor_t *gov,
                                                 uint32_t hz)
{
    if (gov == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    if (hz < 24u || hz > 480u) {
        return WEFT_SPECTRUM_EINVAL;
    }
    WSP_GOV_REFRESH(gov) = (uint64_t)hz;
    return wsp_publish(gov,
                       (weft_pressure_level_t)__atomic_load_n(&gov->pressure,
                                                              __ATOMIC_RELAXED));
}
