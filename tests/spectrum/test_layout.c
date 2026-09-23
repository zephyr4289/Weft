// test_layout.c — WSP5 L-series: byte-frozen layout proofs.
//
// Verifies at RUNTIME what the header static-asserts at compile time:
// exact structure sizes, alignments and field offsets for
// weft_hw_profile_t / weft_tier_plan_t / weft_governor_t /
// weft_governor_stats_t; the complete feature-id -> (word, bit)
// mapping for ALL 265 ids (100% bitmask coverage, both set and clear
// paths); the frozen schema signatures; and the full validation
// ladder (magic / abi / schema / tail / CRC refusals).

#include "weft_spectrum_internal.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            printf("FAIL L %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static uint64_t expected_word(const uint32_t *ids, size_t n)
{
    uint64_t w = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        w |= (1ull << (ids[i] & 63u));
    }
    return w;
}

int main(void)
{
    weft_hw_profile_t prof;
    weft_tier_plan_t plan;
    weft_governor_t gov;

    /* ---- L1: sizes, alignments, offsets ------------------------- */
    CHECK(sizeof(weft_hw_profile_t) == 256, "profile size %zu", sizeof(weft_hw_profile_t));
    CHECK(_Alignof(weft_hw_profile_t) == 128, "profile align %zu", (size_t)_Alignof(weft_hw_profile_t));
    CHECK(offsetof(weft_hw_profile_t, magic) == 0x000, "magic off");
    CHECK(offsetof(weft_hw_profile_t, abi_version) == 0x004, "abi off");
    CHECK(offsetof(weft_hw_profile_t, schema_hash) == 0x008, "schema off");
    CHECK(offsetof(weft_hw_profile_t, caps) == 0x010, "caps off");
    CHECK(offsetof(weft_hw_profile_t, cache_line_size) == 0x038, "cacheline off");
    CHECK(offsetof(weft_hw_profile_t, simd_max_bits) == 0x03c, "simd off");
    CHECK(offsetof(weft_hw_profile_t, cores_total) == 0x040, "cores off");
    CHECK(offsetof(weft_hw_profile_t, cores_performance) == 0x042, "perf off");
    CHECK(offsetof(weft_hw_profile_t, cores_efficiency) == 0x044, "eff off");
    CHECK(offsetof(weft_hw_profile_t, numa_node_count) == 0x046, "numa off");
    CHECK(offsetof(weft_hw_profile_t, npu_channels) == 0x048, "npu off");
    CHECK(offsetof(weft_hw_profile_t, gpu_engines) == 0x04a, "engines off");
    CHECK(offsetof(weft_hw_profile_t, probe_sources_ok) == 0x04e, "sources off");
    CHECK(offsetof(weft_hw_profile_t, ram_total_bytes) == 0x050, "ram off");
    CHECK(offsetof(weft_hw_profile_t, ram_available_bytes) == 0x058, "ramavail off");
    CHECK(offsetof(weft_hw_profile_t, gpu_memory_bytes) == 0x060, "gpumem off");
    CHECK(offsetof(weft_hw_profile_t, thermal_limit_mw) == 0x068, "thermal off");
    CHECK(offsetof(weft_hw_profile_t, boost_headroom_mhz) == 0x06c, "boost off");
    CHECK(offsetof(weft_hw_profile_t, display_max_hz) == 0x070, "display off");
    CHECK(offsetof(weft_hw_profile_t, probe_cost_ns) == 0x074, "cost off");
    CHECK(offsetof(weft_hw_profile_t, numa_cpu_mask) == 0x078, "mask off");
    CHECK(offsetof(weft_hw_profile_t, reserved1) == 0x0b8, "rsvd off");
    CHECK(offsetof(weft_hw_profile_t, crc32c) == 0x0f8, "crc off");
    CHECK(offsetof(weft_hw_profile_t, tail_magic) == 0x0fc, "tail off");

    CHECK(sizeof(weft_tier_plan_t) == 128, "plan size %zu", sizeof(weft_tier_plan_t));
    CHECK(_Alignof(weft_tier_plan_t) == 64, "plan align");
    CHECK(offsetof(weft_tier_plan_t, tier) == 0x10, "plan tier off");
    CHECK(offsetof(weft_tier_plan_t, ring_lanes) == 0x16, "plan lanes off");
    CHECK(offsetof(weft_tier_plan_t, slot_stride) == 0x1c, "plan stride off");
    CHECK(offsetof(weft_tier_plan_t, frame_deadline_us) == 0x20, "plan deadline off");
    CHECK(offsetof(weft_tier_plan_t, arena_budget_bytes) == 0x38, "plan arena off");
    CHECK(offsetof(weft_tier_plan_t, crc32c) == 0x7c, "plan crc off");

    CHECK(sizeof(weft_governor_t) == 384, "gov size %zu", sizeof(weft_governor_t));
    CHECK(_Alignof(weft_governor_t) == 64, "gov align");
    CHECK(offsetof(weft_governor_t, version) == 0x00, "gov version off");
    CHECK(offsetof(weft_governor_t, active_tier) == 0x10, "gov tier off");
    CHECK(offsetof(weft_governor_t, plan_index) == 0x14, "gov index off");
    CHECK(offsetof(weft_governor_t, forced_tier) == 0x20, "gov forced off");
    CHECK(offsetof(weft_governor_t, profile) == 0x50, "gov profile off");
    CHECK(offsetof(weft_governor_t, plan_words) == 0x80, "gov words off");
    CHECK(sizeof(((weft_governor_t *)0)->plan_words) == 256, "gov words size");

    CHECK(sizeof(weft_governor_stats_t) == 64, "stats size %zu", sizeof(weft_governor_stats_t));

    /* ---- L2: frozen schema signatures ---------------------------- */
    CHECK(weft_fnv1a64("weft_spectrum:hw_profile:v1:le:frozen") ==
          WEFT_SPECTRUM_SCHEMA_HASH, "profile schema hash drift");
    CHECK(weft_fnv1a64("weft_spectrum:tier_plan:v1:le:frozen") ==
          WEFT_SPECTRUM_PLAN_SCHEMA_HASH, "plan schema hash drift");

    /* ---- L3: CRC-32C reference vectors (Castagnoli) -------------- */
    CHECK(weft_crc32c("123456789", 9) == 0xE3069283u, "crc32c check value");
    {
        uint32_t c0 = weft_crc32c("", 0);
        CHECK(c0 == 0x00000000u, "crc32c empty");
    }

    /* ---- L4: feature-id -> word/bit mapping, ALL 265 ids --------- */
    {
        uint32_t id;
        for (id = 0; id < (uint32_t)WEFT_FEATURE_COUNT; id++) {
            uint32_t word = id >> 6;
            uint32_t bit = id & 63u;
            memset(&prof, 0, sizeof(prof));
            prof.magic = WEFT_SPECTRUM_MAGIC;
            prof.abi_version = WEFT_SPECTRUM_ABI_VERSION;
            prof.schema_hash = WEFT_SPECTRUM_SCHEMA_HASH;
            CHECK(weft_hw_profile_promote_feature(&prof, id),
                  "promote id %u must succeed", id);
            {
                uint32_t w;
                for (w = 0; w < (uint32_t)WEFT_SPECTRUM_CAP_WORDS; w++) {
                    if (w == word) {
                        CHECK(prof.caps[w] == (1ull << bit),
                              "id %u word %u = 0x%llx (want bit %u)",
                              id, w, (unsigned long long)prof.caps[w], bit);
                    } else {
                        CHECK(prof.caps[w] == 0u,
                              "id %u leaked into word %u", id, w);
                    }
                }
            }
            CHECK(weft_hw_has_feature(&prof, id), "has_feature id %u", id);
            CHECK(weft_hw_caps_word(&prof, word) == (1ull << bit),
                  "caps_word id %u", id);
            CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_OK,
                  "sealed after promote id %u", id);
        }
        /* Out-of-range refusals (fail-closed). */
        CHECK(!weft_hw_profile_promote_feature(&prof, (uint32_t)WEFT_FEATURE_COUNT),
              "promote at COUNT must refuse");
        CHECK(!weft_hw_profile_promote_feature(&prof, 0xFFFFFFFFu),
              "promote max must refuse");
        CHECK(!weft_hw_profile_promote_feature(NULL, 0), "promote NULL");
        CHECK(!weft_hw_has_feature(&prof, (uint32_t)WEFT_FEATURE_COUNT),
              "has_feature at COUNT");
        CHECK(!weft_hw_has_feature(&prof, 0xFFFFFFFFu), "has_feature max");
        CHECK(!weft_hw_has_feature(NULL, 0), "has_feature NULL");
        CHECK(weft_hw_caps_word(&prof, 5) == 0u, "caps_word oob");
        CHECK(weft_hw_caps_word(NULL, 0) == 0u, "caps_word NULL");
    }

    /* ---- L5: grouping sanity (word boundaries) -------------------- */
    {
        const uint32_t w0[] = { 0, 13 };
        const uint32_t w1[] = { 64, 70 };
        const uint32_t w2[] = { 128, 136 };
        const uint32_t w3[] = { 192, 197 };
        const uint32_t w4[] = { 256, 264 };
        memset(&prof, 0, sizeof(prof));
        weft_hw_profile_promote_feature(&prof, 0);
        weft_hw_profile_promote_feature(&prof, 13);
        CHECK(prof.caps[0] == expected_word(w0, 2), "group w0");
        weft_hw_profile_promote_feature(&prof, 64);
        weft_hw_profile_promote_feature(&prof, 70);
        CHECK(prof.caps[1] == expected_word(w1, 2), "group w1");
        weft_hw_profile_promote_feature(&prof, 128);
        weft_hw_profile_promote_feature(&prof, 136);
        CHECK(prof.caps[2] == expected_word(w2, 2), "group w2");
        weft_hw_profile_promote_feature(&prof, 192);
        weft_hw_profile_promote_feature(&prof, 197);
        CHECK(prof.caps[3] == expected_word(w3, 2), "group w3");
        weft_hw_profile_promote_feature(&prof, 256);
        weft_hw_profile_promote_feature(&prof, 264);
        CHECK(prof.caps[4] == expected_word(w4, 2), "group w4");
    }

    /* ---- L6: validation ladder (refusals, never silent) ----------- */
    {
        weft_spectrum_status_t st;
        memset(&prof, 0, sizeof(prof));
        prof.magic = WEFT_SPECTRUM_MAGIC;
        prof.abi_version = WEFT_SPECTRUM_ABI_VERSION;
        prof.schema_hash = WEFT_SPECTRUM_SCHEMA_HASH;
        weft_hw_profile_seal(&prof);
        CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_OK, "fresh seal");
        CHECK(weft_crc32c(&prof, 0xf8) == prof.crc32c, "seal crc matches");

        st = weft_hw_profile_validate(NULL);
        CHECK(st == WEFT_SPECTRUM_EINVAL, "NULL validate -> EINVAL, got %d", st);

        prof.magic = 0xDEADBEEFu;
        CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_EABI, "bad magic");
        prof.magic = WEFT_SPECTRUM_MAGIC;
        prof.abi_version = 99u;
        CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_EABI, "bad abi");
        prof.abi_version = WEFT_SPECTRUM_ABI_VERSION;
        prof.schema_hash ^= 1u;
        CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_EABI, "bad schema");
        prof.schema_hash ^= 1u;
        prof.tail_magic = 0u;
        CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_EABI, "bad tail");
        weft_hw_profile_seal(&prof);
        CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_OK, "re-seal ok");

        /* Single-bit corruption inside the CRC window. */
        {
            uint8_t *raw = (uint8_t *)&prof;
            raw[0x40] ^= 0x01u; /* cores_total */
            CHECK(weft_hw_profile_validate(&prof) == WEFT_SPECTRUM_ECHECKSUM,
                  "corruption refused");
            raw[0x40] ^= 0x01u;
        }
        /* Corruption AFTER the CRC window is outside integrity scope. */
    }

    /* ---- L7: inline query NULL contracts ------------------------- */
    CHECK(weft_get_active_tier(NULL) == (uint32_t)WEFT_SPECTRUM_TIER_INVALID,
          "NULL governor tier");
    CHECK(weft_governor_plan_version(NULL) == 0u, "NULL governor version");

    /* ---- L8: plan/governor zero-state ----------------------------- */
    memset(&plan, 0, sizeof(plan));
    memset(&gov, 0, sizeof(gov));
    CHECK(weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_EINVAL ||
          weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_EABI ||
          weft_governor_snapshot(&gov, &plan) == WEFT_SPECTRUM_EPLAN_BUSY,
          "uninitialized governor must refuse, never fabricate");
    CHECK(weft_governor_snapshot(NULL, &plan) == WEFT_SPECTRUM_EINVAL, "NULL gov");
    CHECK(weft_governor_snapshot(&gov, NULL) == WEFT_SPECTRUM_EINVAL, "NULL out");

    printf("L-series: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
