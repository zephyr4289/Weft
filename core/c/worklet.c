// worklet.c — the zero-overhead native render worklet (see worklet.h).
//
// The handoff is a counting semaphore + two atomics: post() increments
// `posted` and sem_post()s once; the worklet thread sem_wait()s, reads
// the tick ordinal (its own counter, incremented per wake — identical
// order because the thread is the single consumer), runs fn, and
// increments `executed`. posted/executed are the ONLY shared state the
// producer touches; both are relaxed-release pairs through the
// semaphore's ordering (sem_post/sem_wait give the happens-before), so
// no locks and no per-tick allocation anywhere.

#include "worklet.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <semaphore.h>

struct weft_worklet {
    weft_worklet_fn fn;
    void *ctx;
    sem_t ticks;
    _Atomic uint64_t posted;
    _Atomic uint64_t executed;
    _Atomic int stop;
    pthread_t thread;
    unsigned qos_flags;
    int started;
};

static void *worklet_main(void *arg) {
    weft_worklet *w = (weft_worklet *)arg;
    for (;;) {
        while (sem_wait(&w->ticks) == -1) {
            if (errno != EINTR) break; /* declared: retry only on EINTR */
        }
        if (atomic_load_explicit(&w->stop, memory_order_acquire)) break;
        uint64_t tick = atomic_load_explicit(&w->executed, memory_order_relaxed) + 1;
        w->fn(w->ctx, tick);
        atomic_store_explicit(&w->executed, tick, memory_order_release);
    }
    return NULL;
}

void weft_worklet_spawn(weft_worklet_fn fn, void *ctx,
                        const weft_qos_spec *spec,
                        weft_worklet_spawn_report *report) {
    memset(report, 0, sizeof(*report));
    if (!fn || !spec) {
        snprintf(report->err, sizeof(report->err), "fn/spec NULL");
        return;
    }
    weft_worklet *w = calloc(1, sizeof(weft_worklet));
    if (!w) {
        snprintf(report->err, sizeof(report->err), "calloc failed");
        return;
    }
    w->fn = fn;
    w->ctx = ctx;
    atomic_store(&w->posted, 0);
    atomic_store(&w->executed, 0);
    atomic_store(&w->stop, 0);
    if (sem_init(&w->ticks, 0, 0) != 0) {
        snprintf(report->err, sizeof(report->err), "sem_init: %s",
                 strerror(errno));
        free(w);
        return;
    }
    if (pthread_create(&w->thread, NULL, worklet_main, w) != 0) {
        snprintf(report->err, sizeof(report->err), "pthread_create: %s",
                 strerror(errno));
        sem_destroy(&w->ticks);
        free(w);
        return;
    }
    w->started = 1;
    /* Harden the worklet's OWN thread (the spawn-side contract). */
    w->qos_flags = weft_thread_apply_qos(spec);
    report->qos_flags = w->qos_flags;
    report->started = 1;
    report->worklet = w;
}

int weft_worklet_post(weft_worklet *w) {
    if (!w) return EINVAL;
    atomic_fetch_add_explicit(&w->posted, 1, memory_order_relaxed);
    while (sem_post(&w->ticks) == -1) {
        if (errno != EINTR) return errno;
    }
    return 0;
}

int weft_worklet_drain(weft_worklet *w, unsigned timeout_ms) {
    if (!w) return EINVAL;
    const uint64_t target =
        atomic_load_explicit(&w->posted, memory_order_acquire);
    for (unsigned waited = 0; waited <= timeout_ms; waited += 2) {
        if (atomic_load_explicit(&w->executed, memory_order_acquire) >= target) {
            return 0;
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 2 * 1000 * 1000; /* 2 ms slices */
        nanosleep(&ts, NULL);
    }
    return ETIMEDOUT;
}

uint64_t weft_worklet_executed(const weft_worklet *w) {
    return w ? atomic_load_explicit(&w->executed, memory_order_acquire) : 0;
}

uint64_t weft_worklet_pending(const weft_worklet *w) {
    if (!w) return 0;
    return atomic_load_explicit(&w->posted, memory_order_acquire) -
           atomic_load_explicit(&w->executed, memory_order_acquire);
}

void weft_worklet_destroy(weft_worklet *w) {
    if (!w) return;
    if (w->started) {
        atomic_store_explicit(&w->stop, 1, memory_order_release);
        sem_post(&w->ticks); /* wake it so it can observe stop */
        pthread_join(w->thread, NULL);
        sem_destroy(&w->ticks);
    }
    free(w);
}
