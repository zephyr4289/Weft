// weft_synth_common.h — shared belt for the Weft Synthetic Silicon Lab
// (Pillar 8): error codes, clocks, the deterministic PRNG stream, and the
// cache-line vocabulary every injection engine reasons in.
//
// WHY EXISTS: the synthetic lab (thermal throttler, bus blender, network
// chaos fabric) must reproduce real-silicon failure physics inside CI
// without a hardware testbed — and it must do so DETERMINISTICALLY, so a
// chaos run is a replayable experiment, not a lottery. That needs ONE
// shared vocabulary: negative errno-style codes (house convention), a
// monotonic-raw clock for honest latency evidence, a cycle counter with
// explicit calibration for sub-100 ns retry budgeting (the seqlock probe
// SLA cannot be resolved by CLOCK_MONOTONIC alone on this hardware), a
// seeded splitmix64/xorshift64* PRNG stream shared by all engines, and the
// 64/128-byte coherence granularity the blender hammers.
//
// LAYERS (each is a separate module over this belt):
//   weft_synth_thermal   thermal throttling, freq stepping, core migration,
//                        frame-deadline scaling, cadence drop-not-queue
//   weft_synth_bus       cache-line contention blender + seqlock retry probe
//   weft_synth_net       virtual RDMA/UDP chaos fabric + WCR1-style lease
//                        consensus reference (monotonic epochs, one primary)
//
// LAWS (mirrored from the D-82 directive; each module header restates the
// ones it carries):
//   Law 1  zero heap in hot paths: ring manipulation and chaos interceptors
//          execute with 0 B of malloc/calloc/realloc once their contexts
//          are built (plain-leg batteries interpose the allocator to prove
//          it; the module code never calls the allocator at all).
//   Law 2  determinism: every injection decision is drawn from a seeded
//          PRNG in a fixed order — same seed, same trace (verified by
//          trace-hash equality in the battery and the bench gate).
//   Law 3  bounded forward progress: no spin without an exit condition;
//          seqlock retries carry an attempt cap with cooperative yield
//          escalation, so saturation can stretch latency but never wedge.
//   Law 4  strict portability: -std=c11 -Wall -Wextra -Werror -pedantic
//          clean under GCC 14.2 and Clang 21 on x86_64 and aarch64; no
//          VLAs, no anonymous struct members, no GNU statement expressions.
//   Law 5  namespace: weft_synth_* only. weft.c / shm_ring.c / rmw_* /
//          weft_spectrum_* / weft_tensor_* / weft_studio_* are other
//          engineers' frozen namespaces and are never defined here (the
//          suite runner's namespace gate proves it per object file).
//
// HONESTY BOUNDARY: this lab is a behavioral emulator, not a cycle-accurate
// silicon model. Thermal curves are lumped-capacitance approximations;
// "bus saturation" is user-space cache-line hammering; the network fabric
// is a virtual time-stepped wire. Each module header declares exactly what
// its emulation does and does not claim.

#ifndef WEFT_SYNTH__COMMON_H_
#define WEFT_SYNTH__COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- error codes (negative, errno-flavored, never silent) --------------- */

enum {
    WEFT_SYNTH_OK = 0,
    WEFT_SYNTH_ERR_SYS = -1,      /* errno carries the syscall failure     */
    WEFT_SYNTH_ERR_INVALID = -2,  /* bad args / wrong context state        */
    WEFT_SYNTH_ERR_FULL = -3,     /* fixed capacity exhausted (honest)     */
    WEFT_SYNTH_ERR_RANGE = -4,    /* config outside validated bounds       */
    WEFT_SYNTH_ERR_BUSY = -5,     /* engine already running / not running  */
};

/* --- cache-line vocabulary (the blender's hammering granularity) --------- */

/// Coherence granularity fixed at 64 bytes: every production target of this
/// tree (x86_64 since Nehalem, aarch64 since ARMv8) coheres at 64B. The
/// 128-byte experiment is exposed as the double line for Adjacent-line
/// prefetcher studies, not as a different coherence unit.
#define WEFT_SYNTH_CACHE_LINE 64u
#define WEFT_SYNTH_CACHE_LINE2 128u

#if defined(__GNUC__) || defined(__clang__)
#define WEFT_SYNTH_ALIGNED(x) __attribute__((aligned(x)))
#else
#define WEFT_SYNTH_ALIGNED(x)
#endif

/// Cooperative CPU pause for spin loops (Law 3: bound + yield friendly).
#if defined(__x86_64__) || defined(__i386__)
#define WEFT_SYNTH_CPU_RELAX() __asm__ __volatile__("pause")
#elif defined(__aarch64__)
#define WEFT_SYNTH_CPU_RELAX() __asm__ __volatile__("yield")
#else
#define WEFT_SYNTH_CPU_RELAX() do { } while (0)
#endif

/* --- sanitizer awareness (relaxed budgets under instrumentation) --------- */

#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
#define WEFT_SYNTH_TSAN 1
#else
#define WEFT_SYNTH_TSAN 0
#endif

/* --- time ----------------------------------------------------------------- */

/// CLOCK_MONOTONIC_RAW nanoseconds — RAW because NTP/CFS rate adjustment
/// would smear exactly the stall windows this lab exists to measure.
static inline int64_t weft_synth_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

/// Cycle counter. x86_64: lfence+rdtsc (serialized read); aarch64:
/// CNTVCT_EL0; otherwise falls back to raw ns. Sub-100 ns budgets are
/// measured in cycles and converted with the calibrated frequency below.
static inline uint64_t weft_synth_cycles(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return (uint64_t)weft_synth_now_ns();
#endif
}

/// Calibrated cycles-per-second, written ONCE by
/// weft_synth_cycle_calibrate() before measurement threads start (the
/// batteries call it up front; readers afterwards see a stable value).
extern uint64_t weft_synth_cycle_hz;

/// Calibrate the cycle counter against CLOCK_MONOTONIC_RAW over a ~20 ms
/// window. Idempotent: the first caller wins, later calls return the
/// cached value. Call from a single thread before spawning workers.
uint64_t weft_synth_cycle_calibrate(void);

/// Cycle delta to nanoseconds (rounded); needs a prior calibration.
static inline uint64_t weft_synth_cycles_to_ns(uint64_t cyc) {
    if (weft_synth_cycle_hz == 0u) {
        return cyc;  /* uncalibrated: caller gets raw units, honestly */
    }
    return (uint64_t)(((__uint128_t)cyc * 1000000000ull +
                       (__uint128_t)weft_synth_cycle_hz / 2u) /
                      (__uint128_t)weft_synth_cycle_hz);
}

/// Online CPU count (0 on failure — callers clamp to 1).
unsigned weft_synth_cpu_count(void);

/* --- deterministic PRNG (Law 2: replayable chaos) ------------------------- */

/// splitmix64 step: the seeding scrambler (never used as the main stream).
static inline uint64_t weft_synth_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// xorshift64* — the main engine stream (period 2^64-1, one register).
static inline uint64_t weft_synth_xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/// Uniform double in [0, 1) drawn from the engine stream (53-bit mantissa).
static inline double weft_synth_u01(uint64_t *state) {
    return (double)(weft_synth_xorshift64(state) >> 11) *
           (1.0 / 9007199254740992.0);
}

/// FNV-1a 64 mixing helper for trace hashes (determinism fingerprints).
static inline uint64_t weft_synth_fnv1a(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 0x100000001B3ull;
    return h;
}

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_SYNTH__COMMON_H_ */
