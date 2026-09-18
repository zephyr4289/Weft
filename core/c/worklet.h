// worklet.h — the zero-overhead native render worklet (Series 8).
//
// WHY EXISTS: the lead's Series-8 mandate — "Deepening our buffer
// recycling and memory-pressure backstops into zero-overhead native
// worklets." The governed consumer (RFC-0009) is a per-tick composition
// running on whatever thread the platform drives it with; a worklet is
// that loop OWNED by the runtime: a dedicated OS thread, QOS-hardened at
// spawn (thread_qos.h), pulling tick requests through a semaphore-acked
// SPSC handoff with ZERO per-tick allocation, locks, or syscalls beyond
// the wake itself.
//
//     producer ──post()──> [sem] ──> worklet thread: fn(ctx, tick) ──> executed++
//
// CONTRACT (all proven by worklet_test.c):
//   W1 no lost ticks:   posted == executed after drain (exactly).
//   W2 single-consumer order: fn observes ticks in post order (the SPSC
//      semaphore discipline — one wake per post).
//   W3 the QoS flags from spawn are observable and honest (affinity
//      applied/refused is in the spawn report).
//   W4 per-tick cost: a post->run->ack round trip is ~microseconds; the
//      worklet adds NO allocation per tick (state is the struct's own).
//
// The worklet never allocates after spawn; destroy() joins the thread.
// A worklet is SINGLE-producer (the display ticker) by contract.

#ifndef WEFT_WORKLET_H
#define WEFT_WORKLET_H

#include <stdint.h>
#include <stddef.h>
#include "thread_qos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct weft_worklet weft_worklet;

/* One tick of the consumer loop. Runs on the worklet thread; must not
 * allocate (Law 2 — the whole point). `tick` is the post ordinal. */
typedef void (*weft_worklet_fn)(void *ctx, uint64_t tick);

typedef struct {
    weft_worklet *worklet;
    unsigned qos_flags;   /* what thread_qos actually applied at spawn */
    int started;          /* 0/1 */
    char err[128];        /* NUL-terminated on failure */
} weft_worklet_spawn_report;

/* Spawn a worklet running `fn(ctx, tick)` per posted tick with `spec`
 * applied to its thread (never NULL — pass a zeroed spec for defaults).
 * Never returns a partially-started worklet: on failure report->worklet
 * is NULL and report->err carries the reason. */
void weft_worklet_spawn(weft_worklet_fn fn, void *ctx,
                        const weft_qos_spec *spec,
                        weft_worklet_spawn_report *report);

/* Request one tick. Returns 0 on success, EINVAL if worklet is NULL.
 * Never blocks on the worklet (the semaphore is counted; a burst of
 * posts drains in order). */
int weft_worklet_post(weft_worklet *w);

/* Blocks until executed == posted (or timeout ms elapses). Returns 0
 * when drained, ETIMEDOUT otherwise. */
int weft_worklet_drain(weft_worklet *w, unsigned timeout_ms);

/* The executed counter (advisory, AXIOM T). */
uint64_t weft_worklet_executed(const weft_worklet *w);

/* Post - executed right now (the worklet's only queue depth). */
uint64_t weft_worklet_pending(const weft_worklet *w);

/* Join and free. NULL-safe and idempotent. */
void weft_worklet_destroy(weft_worklet *w);

#ifdef __cplusplus
}
#endif

#endif /* WEFT_WORKLET_H */
