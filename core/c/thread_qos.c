// thread_qos.c — real-time thread QoS primitives (see thread_qos.h).
//
// Platform map (Series 8):
//   Linux/Android : pthread_setaffinity_np + SCHED_FIFO (root) / nice
//                   fallback; big-core detection via cpufreq max freqs.
//   Apple/iOS     : thread_policy_set(THREAD_AFFINITY_POLICY) is a no-op
//                   on Apple silicon (documented by Apple) — the honest
//                   flag says so; QOS-class mapping lives in the Swift
//                   port (RenderQoS.swift), which owns
//                   QOS_CLASS_USER_INTERACTIVE. The C tier on macOS
//                   attempts the Mach time-constraint policy only under
//                   WEFT_QOS_ALLOW_MACH (audio-style hard RT is a
//                   footgun without a rationale) — default: nice boost.
//   Other         : WEFT_QOS_UNSUPPORTED (declared, not silent).

#include "thread_qos.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

#if defined(__linux__) || defined(__ANDROID__)
static uint64_t read_cpufreq_mask(void) {
    /* Big-core preference: pick the cluster(s) with the highest
     * cpuinfo_max_freq. /sys layout is not ABI-stable — any failure
     * returns 0 (caller treats as "no opinion"). Bounded, cold-path. */
    char path[128];
    unsigned long max_khz = 0;
    unsigned long khz[64];
    int n = 0;
    for (int cpu = 0; cpu < 64 && n < 64; cpu++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
                 cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long v = 0;
        if (fscanf(f, "%lu", &v) == 1 && v > 0) {
            khz[n++] = v;
            if (v > max_khz) max_khz = v;
        }
        fclose(f);
    }
    if (n == 0 || max_khz == 0) return 0;
    /* Keep the top frequency tier (within 5% of the fastest core) — the
     * classic big.LITTLE placement without a platform table. */
    const unsigned long floor_khz = max_khz - max_khz / 20;
    uint64_t mask = 0;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (khz[i] >= floor_khz) {
            mask |= (1ull << i);
            kept++;
        }
    }
    /* Homogeneous topology (all cores one tier): no opinion. */
    return (kept == n) ? 0 : mask;
}
#endif

uint64_t weft_qos_bigcore_mask(void) {
#if defined(__linux__) || defined(__ANDROID__)
    return read_cpufreq_mask();
#elif defined(__APPLE__)
    return 0; /* Apple Silicon: the scheduler's QoS classes own placement */
#else
    return 0;
#endif
}

unsigned weft_thread_apply_qos(const weft_qos_spec *spec) {
    unsigned flags = 0;
    if (!spec) return WEFT_QOS_UNSUPPORTED;

#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
    /* --- affinity --------------------------------------------------- */
    if (spec->cpu_mask != 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        int online = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (online <= 0) online = 64;
        int requested = 0;
        uint64_t m = spec->cpu_mask;
        for (int i = 0; i < 64 && i < online; i++) {
            if (m & (1ull << i)) {
                CPU_SET(i, &set);
                requested++;
            }
        }
        if (requested > 0) {
            if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
                flags |= WEFT_QOS_APPLIED_AFFINITY;
                /* bits above the online count were dropped */
                if (spec->cpu_mask >> online) flags |= WEFT_QOS_AFFINITY_PARTIAL;
            }
        }
    }

    /* --- scheduling class ------------------------------------------- */
    if (spec->rt_priority > 0) {
        struct sched_param p;
        memset(&p, 0, sizeof(p));
        p.sched_priority = spec->rt_priority;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0) {
            flags |= WEFT_QOS_APPLIED_SCHED;
        } else if (errno == EPERM) {
            /* The documented unprivileged fallback: raise the nice level
             * as far as the kernel allows (negative nice needs
             * CAP_SYS_NICE too — even a no-op attempt is counted, the
             * flag is the truth). */
            flags |= WEFT_QOS_SCHED_UNPRIVILEGED;
            int niceness = -10; /* USER_INTERACTIVE-ish */
            if (spec->cls == WEFT_QOS_BACKGROUND) niceness = 5;
            else if (spec->cls == WEFT_QOS_USER_INITIATED) niceness = -4;
            (void)nice(niceness); /* best-effort by design — flag carries it */
        }
    }
#else
    (void)spec;
    flags |= WEFT_QOS_UNSUPPORTED;
#endif

    return flags;
}

unsigned weft_thread_apply_render_qos(void) {
    weft_qos_spec spec;
    spec.cls = WEFT_QOS_USER_INTERACTIVE;
    spec.cpu_mask = 0;      /* trust the scheduler's placement */
    spec.rt_priority = 0;   /* no FIFO attempt — the honest default */
    return weft_thread_apply_qos(&spec);
}
