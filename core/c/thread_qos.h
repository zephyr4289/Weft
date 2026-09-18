// thread_qos.h — real-time thread QoS primitives, driver layer (Series 8).
//
// WHY EXISTS: the lead's Series-8 mandate — "Real-Time Render Thread QoS
// & Scheduling: real-time thread priority management (QOS_CLASS_USER_
// INTERACTIVE / Android render-thread affinity) to isolate consumer loops
// from OS background task jitter." The kernel (the seqlock triad) is
// wait-free; the CONSUMER thread still competes with background work for
// a core. This module gives every runtime one portable, honest primitive:
//
//   weft_thread_apply_qos(thread, spec) — affinity mask + scheduling
//   class on the calling process's own thread handle (self-hardening;
//   hardening OTHER processes' threads is not a thing an SDK does).
//
// HONESTY CONTRACT (no silent failure — AXIOM T applied to the OS):
//   every attempt returns a FLAG saying what actually happened. Affinity
//   usually applies (Linux/macOS/Windows unprivileged per-thread affinity
//   is allowed); SCHED_FIFO/RTPRIO needs privileges and legitimately
//   fails in most containers — the caller gets QOS_SCHED_UNPRIVILEGED
//   and the thread keeps its nice-level boost attempt instead. The
//   Android/iOS ports (RenderQos.kt / RenderQoS.swift) map the same
//   flags onto Process.setThreadPriority / QOS_CLASS_USER_INTERACTIVE.

#ifndef WEFT_THREAD_QOS_H
#define WEFT_THREAD_QOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QoS classes (the closed set — a fifth is a new RFC). */
typedef enum {
    WEFT_QOS_USER_INTERACTIVE = 0, /* render-critical: the cadence loop */
    WEFT_QOS_USER_INITIATED    = 1, /* input-coupled work               */
    WEFT_QOS_BACKGROUND        = 2  /* prefetch/recycle worklets        */
} weft_qos_class;

/* Result flags (bitmask — more than one can apply). */
#define WEFT_QOS_APPLIED_AFFINITY   0x1u /* affinity mask set             */
#define WEFT_QOS_APPLIED_SCHED      0x2u /* real-time class applied       */
#define WEFT_QOS_SCHED_UNPRIVILEGED 0x4u /* RT class refused (EPERM) —
                                            nice boost attempted instead  */
#define WEFT_QOS_AFFINITY_PARTIAL   0x8u /* mask narrowed to online CPUs  */
#define WEFT_QOS_UNSUPPORTED        0x10u /* platform lacks the primitive */

/* The spec: a closed, documented request. cpu_mask is a bitmask over
 * logical CPU indices (bit i = allow core i); 0 = no affinity request
 * (priority-only hardening). rt_priority is the SCHED_FIFO level to
 * attempt (1..99; 0 = skip the RT attempt entirely). */
typedef struct {
    weft_qos_class cls;
    uint64_t cpu_mask;
    int rt_priority;
} weft_qos_spec;

/* Build the big-core-preference mask for this machine (highest max-
 * frequency clusters first; all-ones when the topology is homogeneous
 * or unreadable — a documented, honest all-ones). Called ONCE at
 * spawn/registration, never per tick. */
uint64_t weft_qos_bigcore_mask(void);

/* Apply `spec` to the CALLING thread. Returns the flag bitmask (never
 * throws, never silently no-ops). Zero allocation. */
unsigned weft_thread_apply_qos(const weft_qos_spec *spec);

/* Convenience: the render-critical preset — USER_INTERACTIVE, no forced
 * affinity (the scheduler's big-core preference is trusted), no RT
 * attempt (unprivileged containers would just log EPERM). */
unsigned weft_thread_apply_render_qos(void);

#ifdef __cplusplus
}
#endif

#endif /* WEFT_THREAD_QOS_H */
