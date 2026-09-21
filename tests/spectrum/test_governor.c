// test_governor.c — WSP5 G-series: dynamic tier governor conformance.
//
// For every golden archetype: governor publication produces the locked
// tier-plan geometry table (lanes / slots / strides / deadlines /
// arena budgets / batch ceilings / worker counts / policy flags /
// SIMD dispatch path), the fast-path tier agrees with the snapshot,
// and the telemetry stats block is coherent. Then: pressure
// transitions (ELEVATED caps Tier 1 down, CRITICAL drops to Tier 3),
// operator force/clear, display-cadence override, version/generation
// monotonicity, and the fail-closed init ladder (corrupted or NULL
// profiles are refused — never governed).

#include "weft_spectrum_internal.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            printf("FAIL G %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

typedef struct {
    uint32_t id;
    uint32_t tier;
    uint32_t simd_path;
    uint32_t workers;
    uint16_t policy;
} wsp_gov_expect_t;

static const wsp_gov_expect_t GEXP[] = {
    { WEFT_ARCHETYPE_APPLE_M4_MAX,       1, WEFT_SIMD_AMX,    8, 0x5 },
    { WEFT_ARCHETYPE_APPLE_A17_PRO,      2, WEFT_SIMD_AMX,    4, 0x9 },
    { WEFT_ARCHETYPE_SNAPDRAGON_8GEN3,   2, WEFT_SIMD_NEON,   4, 0x9 },
    { WEFT_ARCHETYPE_DIMENSITY_9300,     2, WEFT_SIMD_NEON,   4, 0x9 },
    { WEFT_ARCHETYPE_HELIO_G88,          3, WEFT_SIMD_NEON,   2, 0xB },
    { WEFT_ARCHETYPE_RASPBERRY_PI_5,     3, WEFT_SIMD_NEON,   2, 0x3 },
    { WEFT_ARCHETYPE_VISIONFIVE_2,       3, WEFT_SIMD_SCALAR, 2, 0x3 },
    { WEFT_ARCHETYPE_EPYC_9654,          1, WEFT_SIMD_AVX512, 8, 0x5 },
    { WEFT_ARCHETYPE_XEON_SPR_AMX,       1, WEFT_SIMD_AMX,    8, 0x5 },
    { WEFT_ARCHETYPE_GRACE_HOPPER,       1, WEFT_SIMD_SVE2,   8, 0x5 },
    { WEFT_ARCHETYPE_DESKTOP_ZEN4,       1, WEFT_SIMD_AVX512, 6, 0x5 },
    { WEFT_ARCHETYPE_CONTAINER_FALLBACK, 3, WEFT_SIMD_SCALAR, 2, 0x3 },
};

/* Locked tier geometry (normative; D-51 §6) — lanes, slots, hdr,
 * stride, refresh, deadline, guard, batch, arena, lane_bytes, workers. */
typedef struct {
    uint16_t lanes, slots, hdr;
    uint32_t stride, refresh, deadline, guard, batch, workers;
    uint64_t arena, lane_bytes;
} wsp_geom_t;

static const wsp_geom_t GEOM[4] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 16, 256, 128, 1024, 240, 4166, 520, 1024, 8, 4196352ull, 262272ull },
    { 8, 128, 64, 512, 120, 8333, 1041, 512, 4, 524800ull, 65600ull },
    { 4, 64, 64, 256, 60, 16666, 2083, 128, 2, 65792ull, 16448ull },
};

static uint32_t expected_workers(uint32_t tier, const weft_hw_profile_t *p)
{
    uint32_t w = GEOM[tier].workers;
    if (p != NULL && p->cores_total > 0u && p->cores_total < w) {
        w = p->cores_total;
    }
    return (w != 0u) ? w : 1u;
}

static uint32_t expected_simd(const weft_hw_profile_t *p)
{
    if (weft_hw_has_feature(p, WEFT_F_CPU_AMX))       return (uint32_t)WEFT_SIMD_AMX;
    if (weft_hw_has_feature(p, WEFT_F_CPU_AVX512F))   return (uint32_t)WEFT_SIMD_AVX512;
    if (weft_hw_has_feature(p, WEFT_F_CPU_SVE2))      return (uint32_t)WEFT_SIMD_SVE2;
    if (weft_hw_has_feature(p, WEFT_F_CPU_RVV_VLEN_256)) return (uint32_t)WEFT_SIMD_RVV;
    if (weft_hw_has_feature(p, WEFT_F_CPU_RVV_1_0))   return (uint32_t)WEFT_SIMD_RVV;
    if (weft_hw_has_feature(p, WEFT_F_CPU_AVX2))      return (uint32_t)WEFT_SIMD_AVX2;
    if (weft_hw_has_feature(p, WEFT_F_CPU_NEON))      return (uint32_t)WEFT_SIMD_NEON;
    return (uint32_t)WEFT_SIMD_SCALAR;
}

static uint16_t expected_policy(uint32_t tier, const weft_hw_profile_t *p)
{
    uint16_t f = (uint16_t)WEFT_PLAN_POLICY_SEQLOCK_LANES;
    if (tier == 3u) {
        f |= (uint16_t)WEFT_PLAN_POLICY_DROP_NOT_QUEUE;
    }
    if (tier == 1u &&
        (weft_hw_has_feature(p, WEFT_F_NPU_PRESENT) ||
         (weft_hw_has_feature(p, WEFT_F_GPU_PRESENT) &&
          weft_hw_has_feature(p, WEFT_F_GPU_DMABUF_IMPORT)))) {
        f |= (uint16_t)WEFT_PLAN_POLICY_ASYNC_DMA;
    }
    if (weft_hw_has_feature(p, WEFT_F_CPU_HETERO_CORES)) {
        f |= (uint16_t)WEFT_PLAN_POLICY_HETERO_CORES;
    }
    return f;
}

static void check_plan_geometry(const weft_tier_plan_t *pl, uint32_t tier,
                                const weft_hw_profile_t *p)
{
    const wsp_geom_t *g = &GEOM[tier];
    CHECK(pl->magic == WEFT_SPECTRUM_PLAN_MAGIC, "plan magic");
    CHECK(pl->abi_version == WEFT_SPECTRUM_ABI_VERSION, "plan abi");
    CHECK(pl->schema_hash == WEFT_SPECTRUM_PLAN_SCHEMA_HASH, "plan schema");
    CHECK(pl->crc32c == weft_crc32c(pl, 0x7c), "plan crc");
    CHECK(pl->tier == tier, "plan tier %u want %u", pl->tier, tier);
    CHECK(pl->ring_lanes == g->lanes, "lanes %u", pl->ring_lanes);
    CHECK(pl->ring_slots == g->slots, "slots %u", pl->ring_slots);
    CHECK(pl->hdr_stride == g->hdr, "hdr %u", pl->hdr_stride);
    CHECK(pl->slot_stride == g->stride, "stride %u", pl->slot_stride);
    CHECK(pl->refresh_hz == g->refresh, "refresh %u", pl->refresh_hz);
    CHECK(pl->frame_deadline_us == g->deadline, "deadline %u",
          pl->frame_deadline_us);
    CHECK(pl->jitter_guard_us == g->guard, "guard %u", pl->jitter_guard_us);
    CHECK(pl->batch_max_msgs == g->batch, "batch %u", pl->batch_max_msgs);
    CHECK(pl->arena_budget_bytes == g->arena, "arena %llu",
          (unsigned long long)pl->arena_budget_bytes);
    CHECK(pl->lane_bytes == g->lane_bytes, "lane_bytes %llu",
          (unsigned long long)pl->lane_bytes);
    CHECK(pl->arena_budget_bytes ==
          (uint64_t)pl->ring_lanes * pl->lane_bytes, "arena identity");
    CHECK(pl->simd_path == expected_simd(p), "simd path %u want %u",
          pl->simd_path, expected_simd(p));
    CHECK(pl->ingest_workers == expected_workers(tier, p),
          "workers %u want %u", pl->ingest_workers,
          expected_workers(tier, p));
    CHECK(pl->ingest_workers >= 1u && pl->ingest_workers <= 8u,
          "workers bounded");
    if (p != NULL && p->cores_total > 0u) {
        CHECK(pl->ingest_workers <= p->cores_total, "workers <= cores");
    }
    CHECK(pl->policy_flags == expected_policy(tier, p),
          "policy 0x%x want 0x%x", pl->policy_flags,
          expected_policy(tier, p));
    CHECK((pl->policy_flags & WEFT_PLAN_POLICY_SEQLOCK_LANES) != 0,
          "seqlock policy invariant");
    if (tier == 3u) {
        CHECK((pl->policy_flags & WEFT_PLAN_POLICY_DROP_NOT_QUEUE) != 0,
              "T3 drop-not-queue");
        CHECK(pl->arena_budget_bytes < 4u * 1024u * 1024u,
              "T3 arena < 4 MiB");
    } else {
        CHECK((pl->policy_flags & WEFT_PLAN_POLICY_DROP_NOT_QUEUE) == 0,
              "T1/T2 never drop-not-queue");
    }
}

int main(void)
{
    size_t i;
    size_t n = sizeof(GEXP) / sizeof(GEXP[0]);

    CHECK(n == (size_t)WEFT_ARCHETYPE_COUNT, "gov expectation coverage");

    for (i = 0; i < n; i++) {
        const wsp_gov_expect_t *ex = &GEXP[i];
        weft_hw_profile_t p;
        weft_governor_t gov;
        weft_tier_plan_t plan;
        weft_governor_stats_t stats;
        const char *name = weft_mock_archetype_name(ex->id);

        CHECK(weft_mock_archetype_select(ex->id) == WEFT_SPECTRUM_OK,
              "select %s", name);
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe %s", name);
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK,
              "init %s", name);

        CHECK(weft_get_active_tier(&gov) == ex->tier,
              "%s active tier %u want %u", name,
              weft_get_active_tier(&gov), ex->tier);
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "snapshot %s", name);
        check_plan_geometry(&plan, ex->tier, &p);
        /* Per-archetype pins (independent of the computed expectations). */
        CHECK(plan.simd_path == ex->simd_path, "%s simd pin %u",
              name, plan.simd_path);
        CHECK(plan.ingest_workers == ex->workers, "%s workers pin %u",
              name, plan.ingest_workers);
        CHECK(plan.policy_flags == ex->policy, "%s policy pin 0x%x",
              name, plan.policy_flags);
        CHECK(plan.generation == 1u, "%s generation %u", name,
              plan.generation);
        CHECK(weft_governor_plan_version(&gov) == 1ull, "%s version", name);
        CHECK(plan.tier == weft_get_active_tier(&gov),
              "%s plan/fast tier agree", name);

        CHECK(weft_governor_stats(&gov, &stats) == WEFT_SPECTRUM_OK,
              "stats %s", name);
        CHECK(stats.magic == WEFT_GOV_STATS_MAGIC, "stats magic");
        CHECK(stats.tier == ex->tier, "%s stats tier", name);
        CHECK(stats.generation == 1u, "%s stats generation", name);
        CHECK(stats.transitions == 1u, "%s stats transitions", name);
        CHECK(stats.forced_tier == 0u, "%s stats unforced", name);
        CHECK(stats.score == weft_governor_score(&p),
              "%s stats score", name);
        CHECK(stats.arena_budget_bytes ==
              (uint64_t)GEOM[ex->tier].arena, "%s stats arena", name);
        CHECK(stats.refresh_hz == GEOM[ex->tier].refresh,
              "%s stats refresh", name);
        CHECK(stats.frame_deadline_us == GEOM[ex->tier].deadline,
              "%s stats deadline", name);
        CHECK(stats.profile_crc32c == p.crc32c, "%s stats profile crc",
              name);
        CHECK(stats.pressure == (uint32_t)WEFT_PRESSURE_NOMINAL,
              "%s stats pressure", name);
    }

    /* ---- pressure ladder on a Tier-1 machine (M4 Max) ------------ */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        weft_tier_plan_t plan;

        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_APPLE_M4_MAX) ==
              WEFT_SPECTRUM_OK, "select m4");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe m4");
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK, "init m4");
        CHECK(weft_get_active_tier(&gov) == 1u, "m4 starts T1");

        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_ELEVATED) ==
              WEFT_SPECTRUM_OK, "elevated");
        CHECK(weft_get_active_tier(&gov) == 2u, "elevated caps to T2");
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "snapshot elevated");
        check_plan_geometry(&plan, 2u, &p);
        CHECK(plan.pressure_level == (uint32_t)WEFT_PRESSURE_ELEVATED,
              "plan pressure");

        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_CRITICAL) ==
              WEFT_SPECTRUM_OK, "critical");
        CHECK(weft_get_active_tier(&gov) == 3u, "critical drops to T3");
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "snapshot critical");
        check_plan_geometry(&plan, 3u, &p);
        CHECK((plan.policy_flags & WEFT_PLAN_POLICY_DROP_NOT_QUEUE) != 0,
              "critical engages drop-not-queue");
        CHECK(plan.generation == 3u, "generation tracks publishes");
        CHECK(weft_governor_plan_version(&gov) == 3ull, "version tracks");

        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_NOMINAL) ==
              WEFT_SPECTRUM_OK, "recovery");
        CHECK(weft_get_active_tier(&gov) == 1u, "nominal recovers T1");
        CHECK(weft_governor_retarget(&gov, (weft_pressure_level_t)3) ==
              WEFT_SPECTRUM_EINVAL, "bad pressure refused");
        CHECK(weft_governor_retarget(NULL, WEFT_PRESSURE_NOMINAL) ==
              WEFT_SPECTRUM_EINVAL, "NULL governor refused");
    }

    /* ---- pressure never upgrades a budget machine ---------------- */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_HELIO_G88) ==
              WEFT_SPECTRUM_OK, "select g88");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe g88");
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK, "init g88");
        CHECK(weft_get_active_tier(&gov) == 3u, "g88 T3");
        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_ELEVATED) ==
              WEFT_SPECTRUM_OK, "g88 elevated");
        CHECK(weft_get_active_tier(&gov) == 3u, "g88 stays T3");
        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_NOMINAL) ==
              WEFT_SPECTRUM_OK, "g88 nominal");
        CHECK(weft_get_active_tier(&gov) == 3u, "g88 still T3");
    }

    /* ---- Tier-2 machine is stable under ELEVATED ------------------ */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_SNAPDRAGON_8GEN3) ==
              WEFT_SPECTRUM_OK, "select sd");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe sd");
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK, "init sd");
        CHECK(weft_get_active_tier(&gov) == 2u, "sd T2");
        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_ELEVATED) ==
              WEFT_SPECTRUM_OK, "sd elevated");
        CHECK(weft_get_active_tier(&gov) == 2u, "sd stays T2 under cap");
    }

    /* ---- operator force / clear ---------------------------------- */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        weft_tier_plan_t plan;
        weft_governor_stats_t stats;

        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_EPYC_9654) ==
              WEFT_SPECTRUM_OK, "select epyc");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe epyc");
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK, "init epyc");
        CHECK(weft_get_active_tier(&gov) == 1u, "epyc T1");

        CHECK(weft_governor_force_tier(&gov, 3u) == WEFT_SPECTRUM_OK,
              "force 3");
        CHECK(weft_get_active_tier(&gov) == 3u, "forced T3");
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "snapshot forced");
        check_plan_geometry(&plan, 3u, &p);
        /* Pressure transitions must respect the operator force. */
        CHECK(weft_governor_retarget(&gov, WEFT_PRESSURE_NOMINAL) ==
              WEFT_SPECTRUM_OK, "retarget under force");
        CHECK(weft_get_active_tier(&gov) == 3u, "force wins over nominal");
        CHECK(weft_governor_stats(&gov, &stats) == WEFT_SPECTRUM_OK,
              "stats forced");
        CHECK(stats.forced_tier == 3u, "stats forced_tier");

        CHECK(weft_governor_clear_force(&gov) == WEFT_SPECTRUM_OK,
              "clear force");
        CHECK(weft_get_active_tier(&gov) == 1u, "force cleared -> T1");

        CHECK(weft_governor_force_tier(&gov, 0u) == WEFT_SPECTRUM_EINVAL,
              "force 0 refused");
        CHECK(weft_governor_force_tier(&gov, 4u) == WEFT_SPECTRUM_EINVAL,
              "force 4 refused");
        CHECK(weft_governor_force_tier(NULL, 1u) == WEFT_SPECTRUM_EINVAL,
              "force NULL refused");
        CHECK(weft_governor_clear_force(NULL) == WEFT_SPECTRUM_EINVAL,
              "clear NULL refused");
    }

    /* ---- display cadence override -------------------------------- */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        weft_tier_plan_t plan;

        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_APPLE_M4_MAX) ==
              WEFT_SPECTRUM_OK, "select m4 refresh");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe m4 refresh");
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK,
              "init m4 refresh");
        CHECK(weft_governor_set_refresh(&gov, 120u) == WEFT_SPECTRUM_OK,
              "set 120");
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "snapshot 120");
        CHECK(plan.refresh_hz == 120u, "refresh 120");
        CHECK(plan.frame_deadline_us == 8333u, "deadline 8333");
        CHECK(plan.jitter_guard_us == 1041u, "guard 1041");
        CHECK(plan.tier == 1u, "tier unchanged by cadence");
        CHECK(plan.ring_lanes == 16u, "geometry unchanged by cadence");
        CHECK(weft_governor_set_refresh(&gov, 144u) == WEFT_SPECTRUM_OK,
              "set 144");
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "snapshot 144");
        CHECK(plan.frame_deadline_us == 6944u, "deadline 6944");
        CHECK(weft_governor_set_refresh(&gov, 23u) == WEFT_SPECTRUM_EINVAL,
              "23 Hz refused");
        CHECK(weft_governor_set_refresh(&gov, 481u) == WEFT_SPECTRUM_EINVAL,
              "481 Hz refused");
        CHECK(weft_governor_set_refresh(NULL, 60u) == WEFT_SPECTRUM_EINVAL,
              "NULL refresh refused");
    }

    /* ---- fail-closed init ladder ---------------------------------- */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_GRACE_HOPPER) ==
              WEFT_SPECTRUM_OK, "select gh");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe gh");

        CHECK(weft_governor_init(NULL, &p) == WEFT_SPECTRUM_EINVAL,
              "NULL governor");
        CHECK(weft_governor_init(&gov, NULL) == WEFT_SPECTRUM_EINVAL,
              "NULL profile");
        {
            uint8_t *raw = (uint8_t *)&p;
            raw[0x050] ^= 0x01u; /* tear the RAM field */
            CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_ECHECKSUM,
                  "corrupted profile refused");
            raw[0x050] ^= 0x01u;
        }
        {
            weft_hw_profile_t bad;
            memset(&bad, 0, sizeof(bad));
            bad.magic = WEFT_SPECTRUM_MAGIC;
            bad.abi_version = 7u;
            bad.schema_hash = WEFT_SPECTRUM_SCHEMA_HASH;
            weft_hw_profile_seal(&bad);
            CHECK(weft_governor_init(&gov, &bad) == WEFT_SPECTRUM_EABI,
                  "wrong ABI refused");
        }
    }

    /* ---- pure-function contracts ---------------------------------- */
    CHECK(weft_governor_score(NULL) == 0u, "NULL score");
    CHECK(weft_governor_tier_for_profile(NULL) ==
          (uint32_t)WEFT_SPECTRUM_TIER_INVALID, "NULL tier");
    CHECK(weft_governor_snapshot(NULL, NULL) == WEFT_SPECTRUM_EINVAL,
          "snapshot NULLs");
    CHECK(weft_governor_stats(NULL, NULL) == WEFT_SPECTRUM_EINVAL,
          "stats NULLs");

    /* ---- version monotonicity under a transition storm ----------- */
    {
        weft_hw_profile_t p;
        weft_governor_t gov;
        weft_tier_plan_t plan;
        uint64_t v;
        uint32_t k;
        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_APPLE_M4_MAX) ==
              WEFT_SPECTRUM_OK, "select m4 storm");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe m4 storm");
        CHECK(weft_governor_init(&gov, &p) == WEFT_SPECTRUM_OK,
              "init m4 storm");
        v = weft_governor_plan_version(&gov);
        for (k = 0; k < 1000u; k++) {
            CHECK(weft_governor_retarget(
                      &gov,
                      (k % 3u == 0u) ? WEFT_PRESSURE_NOMINAL
                      : (k % 3u == 1u) ? WEFT_PRESSURE_ELEVATED
                                       : WEFT_PRESSURE_CRITICAL) ==
                  WEFT_SPECTRUM_OK, "storm retarget %u", k);
        }
        CHECK(weft_governor_plan_version(&gov) == v + 1000ull,
              "version +1000");
        CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_OK,
              "storm snapshot");
        CHECK(plan.generation == (uint16_t)(v + 1000ull),
              "storm generation");
    }

    printf("G-series: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
