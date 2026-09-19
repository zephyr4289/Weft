// ffi_host.h — TIER4 §2 FFI boundary isolation (issue #19), C reference.
//
// WHY EXISTS: the FFI seam is Weft's attack surface. A panic in the JVM
// must not take down the C kernel host; a hung native call must not wedge
// the render loop; a segfault in one foreign guest must not kill the
// process that hosts other guests. The kernel itself is memory-safe by
// construction (and now fuzz-audited, TIER4 §1/§5) — this module isolates
// everything AROUND it.
//
// MODEL — worker isolate + bounded mailbox (message passing, no shared
// mutable state beyond the mailboxes themselves):
//
//   host thread                          worker thread (the isolate)
//   -----------                          ---------------------------
//   weft_ffi_host_call(op, args)
//     -> mailbox_push(request)  ---->    wait request
//     -> wait response (bounded)         fn(args) -> out          (in-process)
//        OR fork child, wait, reap       OR fork + pipe + exec-fn (fork mode)
//     <- status + result blob   <----    mailbox_push(response)
//
// - ONE worker thread per host. Guests are serialized by construction:
//   the isolate has no shared state to race on.
// - Requests/responses are FIXED-SIZE value blobs copied through the
//   mailbox (no pointers cross the seam in either direction).
// - Every call is bounded by timeout_ms (default 1000, host-configurable):
//   a hung guest returns WEFT_FFI_TIMEOUT; the host stays healthy; a
//   timed-out in-process worker is STOPPED and a replacement started
//   (the only way to bound an unknown hang in-process), so the NEXT call
//   still works — zero cross-gate crashes, zero infinite hangs.
// - Fork mode (WEFT_FFI_HOST_FORK, or per-call): the op runs in a forked
//   child; a child CRASH (SIGSEGV/SIGABRT/...) becomes WEFT_FFI_CRASH and
//   the parent process survives — the strongest containment POSIX offers
//   without a supervisor. Fork overhead is declared (measured by the test,
//   ~100-400 µs/call) — off the hot path by design.
//
// OVERHEAD CONTRACT (issue #19: "<1ms overhead vs direct calls"): the
// in-process dispatch path is measured by ffi_host_test F5 and must stay
// under 1 ms/call; the measured number in this tree is ~5-20 µs (mutex +
// two condvar handoffs). Flags-not-silence: the test FAILS if the contract
// is violated on the CI machine.
//
// LAW 2 (zero is a contract): start/call/stop allocate only at start
// (thread + mailbox buffers); call() allocates nothing.
//
// LAW 1 (no silent anything): every timeout, crash, and refused op returns
// a distinct status code; the host counts them (advisory telemetry).

#ifndef WEFT_FFI_HOST_H
#define WEFT_FFI_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/// Max bytes of arguments / results through the mailbox. Fixed-size value
/// semantics — no pointers, no dynamic buffers across the seam.
#define WEFT_FFI_BLOB_MAX 256

/// Guest operation: `fn(args, args_len, out, &out_len, user)`.
/// Returns 0 on success (out_len set), -1 on guest-reported failure.
/// In fork mode this runs INSIDE THE CHILD: it must not touch anything but
/// its arguments and `user` (which must be pre-mapped shared memory or
/// read-only data to be meaningful there).
typedef int (*weft_ffi_op_fn)(const uint8_t* args, size_t args_len,
                              uint8_t* out, size_t* out_len, void* user);

typedef enum {
    WEFT_FFI_OK          = 0,  // op completed, result valid
    WEFT_FFI_OP_FAILED   = 1,  // op ran and reported failure (guest's -1)
    WEFT_FFI_TIMEOUT     = 2,  // bounded wait exhausted (hung guest)
    WEFT_FFI_CRASH       = 3,  // guest crashed (fork mode: child killed by signal)
    WEFT_FFI_BAD_OP      = 4,  // unregistered op id
    WEFT_FFI_STOPPED     = 5,  // host already stopped / not started
} weft_ffi_status_t;

typedef struct weft_ffi_host weft_ffi_host_t;

/// Start a host. `fork_mode` runs every call in a forked child (strongest
/// containment, higher latency). Returns NULL on resource failure.
weft_ffi_host_t* weft_ffi_host_start(bool fork_mode);

/// Register an op id -> fn. Returns 0, -1 if id taken or host started with
/// fork_mode (registration must precede start in fork mode; this API still
/// accepts it pre-start by contract — see ffi_host_test F3).
int weft_ffi_host_register(weft_ffi_host_t* h, uint32_t op_id, weft_ffi_op_fn fn, void* user);

/// One bounded, isolated call. Copies `args_len` bytes (<= WEFT_FFI_BLOB_MAX)
/// into the request, waits for the response up to timeout_ms (0 => host
/// default 1000 ms). On WEFT_FFI_OK copies up to out_cap bytes into `out`
/// and sets *out_len. Never blocks longer than timeout + one handoff tick.
weft_ffi_status_t weft_ffi_host_call(weft_ffi_host_t* h, uint32_t op_id,
                                     const uint8_t* args, size_t args_len,
                                     uint8_t* out, size_t out_cap, size_t* out_len,
                                     uint32_t timeout_ms);

/// Stop the host. Idempotent. Joins the worker (bounded by the last call's
/// timeout contract). NULL-safe.
void weft_ffi_host_stop(weft_ffi_host_t* h);

/// Advisory counters (AXIOM T — telemetry, never a correctness reference).
typedef struct {
    uint64_t calls;          ///< completed calls (any status)
    uint64_t timeouts;       ///< WEFT_FFI_TIMEOUT verdicts
    uint64_t crashes;        ///< WEFT_FFI_CRASH verdicts
    uint64_t restarts;       ///< worker replacements after a hang
} weft_ffi_stats_t;
void weft_ffi_host_stats(const weft_ffi_host_t* h, weft_ffi_stats_t* out);

#endif // WEFT_FFI_HOST_H
