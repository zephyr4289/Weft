// weft_device_profile.c — Device-Tier Detection & Adaptive Geometry, C driver layer
//
// LAW 3: Mechanism, not policy. Zero kernel modification.

#include "weft_device_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

weft_device_profile_t weft_device_probe(void) {
    weft_device_profile_t prof;
    memset(&prof, 0, sizeof(prof));

    // 1. Cache line width probe
    unsigned cache_line = 64;
#if defined(_SC_LEVEL1_DCACHE_LINESIZE)
    long sc_line = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (sc_line > 0) {
        cache_line = (unsigned)sc_line;
    }
#elif defined(__APPLE__)
    size_t line_size = 0;
    size_t size_len = sizeof(line_size);
    if (sysctlbyname("hw.cachelinesize", &line_size, &size_len, NULL, 0) == 0 && line_size > 0) {
        cache_line = (unsigned)line_size;
    }
#endif
    // Fallback: If 128B Apple Silicon / Cortex-X is indicated
#if defined(__aarch64__) || defined(_M_ARM64)
    if (cache_line < 64) cache_line = 64;
#if defined(__APPLE__)
    if (cache_line == 0 || cache_line < 128) cache_line = 128;
#endif
#else
    if (cache_line == 0) cache_line = 64;
#endif
    prof.cache_line = cache_line;

    // 2. CPU Cores & Topology Probe
    long num_cores = 1;
#if defined(_SC_NPROCESSORS_ONLN)
    num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
    num_cores = sysconf(_SC_NPROCESSORS_CONF);
#endif
    if (num_cores < 1) num_cores = 1;

    unsigned perf = 0;
    unsigned eff = 0;

    // Linux / Android sysfs topology probing
#if defined(__linux__) || defined(__ANDROID__)
    for (int i = 0; i < num_cores && i < 32; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
        FILE* f = fopen(path, "r");
        if (f) {
            unsigned cap = 0;
            if (fscanf(f, "%u", &cap) == 1) {
                if (cap >= 700) {
                    perf++;
                } else {
                    eff++;
                }
            }
            fclose(f);
        }
    }
#endif

    // Fallback if sysfs topology was not available
    if (perf == 0 && eff == 0) {
        if (num_cores <= 2) {
            perf = (unsigned)num_cores;
            eff = 0;
        } else if (num_cores <= 6) {
            perf = (unsigned)(num_cores / 2);
            eff = (unsigned)(num_cores - perf);
        } else {
            // e.g. 8-core: assume 4 big + 4 little or 8 symmetric
            perf = (unsigned)(num_cores >= 8 ? 4 : (num_cores / 2));
            eff = (unsigned)(num_cores - perf);
        }
    }

    prof.perf_cores = perf;
    prof.eff_cores = eff;

    // 3. GPU Direct / Unified Memory Viability
#if defined(__APPLE__)
    prof.gpu_direct = 1; // Apple Silicon unified memory architecture
#elif defined(__ANDROID__) || defined(__linux__)
    // Mobile SoCs / ARM generally share physical RAM between CPU and GPU
    prof.gpu_direct = 1;
#else
    prof.gpu_direct = 0;
#endif

    // 4. Device Tier Assignment
    if (num_cores <= 2) {
        prof.tier = WEFT_DEVICE_TIER_LOW;
    } else if (num_cores <= 6) {
        prof.tier = WEFT_DEVICE_TIER_MID;
    } else {
        prof.tier = WEFT_DEVICE_TIER_HIGH;
    }

    // 5. Thermal Budget
    prof.thermal_budget = 1000ULL; // Baseline nominal budget

    return prof;
}

unsigned weft_device_recommended_slot_count(const weft_device_profile_t* prof) {
    if (!prof) return 4;
    return (prof->tier == WEFT_DEVICE_TIER_LOW) ? 8u : 4u;
}

unsigned weft_device_recommended_alignment(const weft_device_profile_t* prof) {
    if (!prof) return 64u;
    if (prof->tier == WEFT_DEVICE_TIER_LOW || prof->cache_line >= 128u) {
        return 128u;
    }
    return 64u;
}

unsigned weft_device_recommended_claim_cadence_divisor(const weft_device_profile_t* prof) {
    if (!prof) return 1u;
    return (prof->tier == WEFT_DEVICE_TIER_LOW) ? 2u : 1u;
}

uint32_t weft_device_recommended_governor_cooldown(const weft_device_profile_t* prof) {
    if (!prof) return 250u;
    switch (prof->tier) {
        case WEFT_DEVICE_TIER_LOW:  return 500u;
        case WEFT_DEVICE_TIER_MID:  return 250u;
        case WEFT_DEVICE_TIER_HIGH: return 150u;
        default:                    return 250u;
    }
}

unsigned weft_device_recommended_prefetch_dist(const weft_device_profile_t* prof) {
    if (!prof) return 128u;
    switch (prof->tier) {
        case WEFT_DEVICE_TIER_LOW:  return 0u;
        case WEFT_DEVICE_TIER_MID:  return 128u;
        case WEFT_DEVICE_TIER_HIGH: return 256u;
        default:                    return 128u;
    }
}
