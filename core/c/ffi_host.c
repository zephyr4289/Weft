// ffi_host.c — TIER4 §2 FFI boundary isolation (issue #19), C reference.
// See ffi_host.h for the model and the contracts.
//
// Concurrency notes (this file is deliberately boring):
// - The mailbox is one request slot + one response slot under one mutex,
//   two condvars. Single-flight by design (backpressure, not buffering).
// - pthread_cond_timedwait uses CLOCK_REALTIME absolute deadlines (the
//   portable default) — the bound is wall-clock, monotonic-vs-realtime
//   skew is bounded by NTP discipline on every host this runs on; the
//   callers that care about monotonic bounds use the fork mode where the
//   deadline is enforced by the kernel's own timers (waitpid polling).
// - A hung in-process guest is contained by CANCELLING the worker and
//   starting a replacement (weft_ffi_host_call). pthread_cancel fires at
//   the guest's next cancellation point; a guest that never reaches one
//   is unbounded in-process BY THE PLATFORM — that is what fork mode is
//   for, and the test pins the fork-mode containment instead (F3).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ffi_host.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FII_MAX_OPS 16
#define FII_DEFAULT_TIMEOUT_MS 1000u
#define FII_FORK_POLL_US (1 * 1000)  // 1 ms waitpid poll granularity

typedef struct {
    uint32_t op_id;
    uint32_t timeout_ms;   // caller's bound — the fork-mode kill deadline
    uint8_t  args[WEFT_FFI_BLOB_MAX];
    size_t   args_len;
} ffi_req_t;

typedef struct {
    uint32_t       id;
    weft_ffi_op_fn fn;
    void*          user;
} ffi_op_t;

typedef struct {
    uint32_t           op_id;
    weft_ffi_status_t  status;
    uint8_t            out[WEFT_FFI_BLOB_MAX];
    size_t             out_len;
    bool               ready;
} ffi_resp_t;

struct weft_ffi_host {
    // op registry (fixed at start; read-only afterwards)
    ffi_op_t ops[FII_MAX_OPS];
    int n_ops;

    bool fork_mode;
    uint32_t default_timeout_ms;

    // mailbox: one request slot, one response slot (single-flight by design)
    pthread_mutex_t mx;
    pthread_cond_t  req_cv;    // worker waits here
    pthread_cond_t  resp_cv;   // caller waits here
    ffi_req_t       req;
    ffi_resp_t      resp;
    bool            req_pending;
    bool            shutdown;

    pthread_t worker;
    bool worker_alive;

    // advisory telemetry
    uint64_t t_calls, t_timeouts, t_crashes, t_restarts;
};

// ---------------------------------------------------------------------------

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct timespec abstime_in_ms(uint64_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static int find_op(const weft_ffi_host_t* h, uint32_t id, int* idx) {
    for (int i = 0; i < h->n_ops; i++) {
        if (h->ops[i].id == id) { *idx = i; return 1; }
    }
    return 0;
}

// Run a fork-mode guest with a bounded wait. Returns the response.
static ffi_resp_t run_forked(const ffi_op_t* o, const ffi_req_t* r, uint64_t timeout_ms) {
    ffi_resp_t resp;
    memset(&resp, 0, sizeof resp);
    resp.op_id = r->op_id;
    resp.status = WEFT_FFI_CRASH;  // provisional; revised on every happy path

    int pfd[2];
    if (pipe(pfd) != 0) {
        resp.status = WEFT_FFI_OP_FAILED;
        return resp;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        resp.status = WEFT_FFI_OP_FAILED;
        return resp;
    }
    if (pid == 0) {
        // ---- child: run the guest, write status + blob, exit ----
        close(pfd[0]);
        uint8_t out[WEFT_FFI_BLOB_MAX];
        size_t out_len = 0;
        int rc = o->fn(r->args, r->args_len, out, &out_len, o->user);
        int32_t st = (rc == 0) ? (int32_t)WEFT_FFI_OK : (int32_t)WEFT_FFI_OP_FAILED;
        if (out_len > WEFT_FFI_BLOB_MAX) out_len = WEFT_FFI_BLOB_MAX;
        ssize_t ig = 0;
        ig += write(pfd[1], &st, sizeof st);
        ig += write(pfd[1], &out_len, sizeof out_len);
        if (out_len > 0) ig += write(pfd[1], out, out_len);
        (void)ig;
        close(pfd[1]);
        _exit(0);
    }

    // ---- parent: bounded waitpid poll at 1 ms ----
    close(pfd[1]);
    uint64_t deadline = now_ns() + timeout_ms * 1000000ull;
    int wst = 0;
    pid_t wr = 0;
    bool done = false, timedout = false;
    for (;;) {
        wr = waitpid(pid, &wst, WNOHANG);
        if (wr == pid) { done = true; break; }
        if (wr < 0 && errno == EINTR) continue;
        if (now_ns() >= deadline) { timedout = true; break; }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = FII_FORK_POLL_US * 1000 };
        nanosleep(&ts, NULL);
    }

    if (timedout) {
        // Bound enforced by US: kill the child, report TIMEOUT, reap later
        // (WNOHANG may race; a blocking reap here is safe — the child dies).
        kill(pid, SIGKILL);
        int wst2 = 0;
        while (waitpid(pid, &wst2, 0) < 0 && errno == EINTR) {}
        resp.status = WEFT_FFI_TIMEOUT;
        close(pfd[0]);
        return resp;
    }

    if (!done || WIFSIGNALED(wst) || (WIFEXITED(wst) && WEXITSTATUS(wst) != 0)) {
        // crashed: child died on a signal or bailed without a result
        resp.status = WEFT_FFI_CRASH;
        close(pfd[0]);
        return resp;
    }

    // Child exited cleanly: plumb status + blob through the pipe.
    int32_t st = (int32_t)WEFT_FFI_CRASH;
    size_t out_len = 0;
    ssize_t got = read(pfd[0], &st, sizeof st);
    got += read(pfd[0], &out_len, sizeof out_len);
    if (got > 0 && (st == (int32_t)WEFT_FFI_OK || st == (int32_t)WEFT_FFI_OP_FAILED)) {
        if (out_len > WEFT_FFI_BLOB_MAX) out_len = WEFT_FFI_BLOB_MAX;
        size_t have = 0;
        while (have < out_len) {
            ssize_t n = read(pfd[0], resp.out + have, out_len - have);
            if (n <= 0) break;
            have += (size_t)n;
        }
        resp.out_len = have;
        resp.status = (weft_ffi_status_t)st;
    } else {
        resp.status = WEFT_FFI_CRASH;  // died mid-write
    }
    close(pfd[0]);
    return resp;
}

// ---------------------------------------------------------------------------

static void* worker_main(void* p) {
    weft_ffi_host_t* h = (weft_ffi_host_t*)p;

    pthread_mutex_lock(&h->mx);
    for (;;) {
        while (!h->req_pending && !h->shutdown) {
            pthread_cond_wait(&h->req_cv, &h->mx);
        }
        if (h->shutdown && !h->req_pending) break;

        // Copy the request out (bounded blob), release the lock, run the
        // guest UNLOCKED — a slow/hung guest must not hold the mailbox.
        ffi_req_t r = h->req;
        h->req_pending = false;
        pthread_mutex_unlock(&h->mx);

        ffi_resp_t resp;
        memset(&resp, 0, sizeof resp);
        resp.op_id = r.op_id;

        int oi = -1;
        if (!find_op(h, r.op_id, &oi)) {
            resp.status = WEFT_FFI_BAD_OP;
        } else {
            const weft_ffi_op_fn fn = h->ops[oi].fn;
            void* user = h->ops[oi].user;
            if (h->fork_mode) {
                ffi_op_t o = { r.op_id, fn, user };
                resp = run_forked(&o, &r, r.timeout_ms);
            } else {
                uint8_t out[WEFT_FFI_BLOB_MAX];
                size_t out_len = 0;
                int rc = fn(r.args, r.args_len, out, &out_len, user);
                resp.status = (rc == 0) ? WEFT_FFI_OK : WEFT_FFI_OP_FAILED;
                if (out_len > WEFT_FFI_BLOB_MAX) out_len = WEFT_FFI_BLOB_MAX;
                memcpy(resp.out, out, out_len);
                resp.out_len = out_len;
            }
        }

        pthread_mutex_lock(&h->mx);
        h->resp = resp;
        h->resp.ready = true;
        pthread_cond_broadcast(&h->resp_cv);
    }
    pthread_mutex_unlock(&h->mx);
    return NULL;
}

weft_ffi_host_t* weft_ffi_host_start(bool fork_mode) {
    weft_ffi_host_t* h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fork_mode = fork_mode;
    h->default_timeout_ms = FII_DEFAULT_TIMEOUT_MS;
    pthread_mutex_init(&h->mx, NULL);
    pthread_cond_init(&h->req_cv, NULL);
    pthread_cond_init(&h->resp_cv, NULL);
    if (pthread_create(&h->worker, NULL, worker_main, h) != 0) {
        pthread_mutex_destroy(&h->mx);
        pthread_cond_destroy(&h->req_cv);
        pthread_cond_destroy(&h->resp_cv);
        free(h);
        return NULL;
    }
    h->worker_alive = true;
    return h;
}

int weft_ffi_host_register(weft_ffi_host_t* h, uint32_t op_id, weft_ffi_op_fn fn, void* user) {
    if (!h || !fn) return -1;
    int dummy;
    if (find_op(h, op_id, &dummy)) return -1;
    if (h->n_ops >= FII_MAX_OPS) return -1;
    h->ops[h->n_ops].id = op_id;
    h->ops[h->n_ops].fn = fn;
    h->ops[h->n_ops].user = user;
    h->n_ops++;
    return 0;
}

weft_ffi_status_t weft_ffi_host_call(weft_ffi_host_t* h, uint32_t op_id,
                                     const uint8_t* args, size_t args_len,
                                     uint8_t* out, size_t out_cap, size_t* out_len,
                                     uint32_t timeout_ms) {
    if (!h) return WEFT_FFI_STOPPED;
    if (args_len > WEFT_FFI_BLOB_MAX) return WEFT_FFI_BAD_OP;
    uint32_t tmo = timeout_ms ? timeout_ms : h->default_timeout_ms;
    if (out_len) *out_len = 0;

    pthread_mutex_lock(&h->mx);
    if (!h->worker_alive) {
        pthread_mutex_unlock(&h->mx);
        return WEFT_FFI_STOPPED;
    }
    if (h->req_pending) {
        // Previous caller still holds the single flight: refuse loudly.
        // The render loop wants backpressure, not a silent queue.
        pthread_mutex_unlock(&h->mx);
        return WEFT_FFI_OP_FAILED;
    }
    h->req.op_id = op_id;
    h->req.timeout_ms = tmo;
    if (args_len > 0) memcpy(h->req.args, args, args_len);
    h->req.args_len = args_len;
    h->req_pending = true;
    h->resp.ready = false;
    pthread_cond_broadcast(&h->req_cv);

    // Bounded wait for the response.
    struct timespec deadline = abstime_in_ms(tmo);
    weft_ffi_status_t status;
    for (;;) {
        if (h->resp.ready) {
            status = h->resp.status;
            if (status == WEFT_FFI_OK && out && out_cap) {
                size_t n = h->resp.out_len < out_cap ? h->resp.out_len : out_cap;
                memcpy(out, h->resp.out, n);
                if (out_len) *out_len = h->resp.out_len;
            }
            break;
        }
        int rc = pthread_cond_timedwait(&h->resp_cv, &h->mx, &deadline);
        if (rc == ETIMEDOUT) {
            // The guest hung. In-process, the ONLY way to bound an unknown
            // hang is to stop the worker and start a replacement — the host
            // survives, the next call works. Counted, never silent.
            h->t_timeouts++;
            pthread_cancel(h->worker);
            pthread_join(h->worker, NULL);
            h->req_pending = false;
            h->resp.ready = false;
            if (pthread_create(&h->worker, NULL, worker_main, h) == 0) {
                h->worker_alive = true;
                h->t_restarts++;
            } else {
                h->worker_alive = false;
            }
            status = WEFT_FFI_TIMEOUT;
            break;
        }
        // rc == 0 (resp flipped) or spurious wake: re-check the flag.
    }
    if (status == WEFT_FFI_CRASH) h->t_crashes++;
    h->t_calls++;
    pthread_mutex_unlock(&h->mx);
    return status;
}

void weft_ffi_host_stop(weft_ffi_host_t* h) {
    if (!h) return;
    pthread_mutex_lock(&h->mx);
    if (!h->worker_alive) {
        pthread_mutex_unlock(&h->mx);
        free(h);
        return;
    }
    h->shutdown = true;
    pthread_cond_broadcast(&h->req_cv);
    pthread_t w = h->worker;
    h->worker_alive = false;
    pthread_mutex_unlock(&h->mx);
    pthread_join(w, NULL);
    pthread_mutex_destroy(&h->mx);
    pthread_cond_destroy(&h->req_cv);
    pthread_cond_destroy(&h->resp_cv);
    free(h);
}

void weft_ffi_host_stats(const weft_ffi_host_t* h, weft_ffi_stats_t* out) {
    if (!out) return;
    if (!h) { memset(out, 0, sizeof *out); return; }
    out->calls = h->t_calls;
    out->timeouts = h->t_timeouts;
    out->crashes = h->t_crashes;
    out->restarts = h->t_restarts;
}
