// weft_device_profile.h — Device-Tier Detection & Adaptive Geometry, C driver layer
//
// WHY EXISTS: nano/doc-007.md §1. The verification axes prove correctness;
// the adaptation layer ensures the same protocol runs at 120 Hz on flagship
// silicon and at 30 Hz on low-end hardware without tearing or GC pressure.
//
// LAW 3: Mechanism, not policy. The kernel (weft.h/weft.c) remains strictly
// byte-frozen; device profiling and geometry adaptation reside purely in the
// driver/consumer boundary.

#ifndef WEFT_DEVICE_PROFILE_H
#define WEFT_DEVICE_PROFILE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_DEVICE_TIER_LOW  = 0,  ///< Low-end (e.g. 1-2 cores, Android Go, low L1 cache)
    WEFT_DEVICE_TIER_MID  = 1,  ///< Mid-tier (e.g. 4-6 cores, standard mobile/desktop)
    WEFT_DEVICE_TIER_HIGH = 2,  ///< High-tier (e.g. 8+ cores, Apple Silicon, Cortex-X)
} weft_device_tier_t;

typedef struct {
    int      tier;            ///< 0=low, 1=mid, 2=high (weft_device_tier_t)
    unsigned cache_line;      ///< 64 or 128 (Apple M-series, some Cortex-X)
    unsigned perf_cores;      ///< big cores in big.LITTLE / heterogeneous topology
    unsigned eff_cores;       ///< little cores
    int      gpu_direct;      ///< 1 if GPU-resident ring / unified memory is viable
    uint64_t thermal_budget;  ///< platform-specific throttle headroom threshold
} weft_device_profile_t;

/// Probe the hardware device profile once at initialization (cold-path).
/// Safe fallback on any platform without dynamic allocation.
weft_device_profile_t weft_device_probe(void);

/// Adaptive geometry & policy recommendations by tier:
///
/// | Parameter                    | Low-end | Mid    | High   |
/// |------------------------------|---------|--------|--------|
/// | slot_count                   | 8       | 4      | 4      |
/// | payload_bytes alignment      | 128B    | 64B    | 64B    |
/// | claim cadence divisor        | 2       | 1      | 1      |
/// | governor reseed cooldown ms  | 500     | 250    | 150    |
/// | turbo prefetch distance (B)  | 0       | 128    | 256    |

unsigned weft_device_recommended_slot_count(const weft_device_profile_t* prof);
unsigned weft_device_recommended_alignment(const weft_device_profile_t* prof);
unsigned weft_device_recommended_claim_cadence_divisor(const weft_device_profile_t* prof);
uint32_t weft_device_recommended_governor_cooldown(const weft_device_profile_t* prof);
unsigned weft_device_recommended_prefetch_dist(const weft_device_profile_t* prof);

#ifdef __cplusplus
}
#endif

#endif // WEFT_DEVICE_PROFILE_H
