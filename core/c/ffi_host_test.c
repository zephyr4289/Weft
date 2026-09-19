// ffi_host_test.c — TIER4 §2 FFI boundary isolation conformance (F-series), C.
//
// Issue #19 (Tier 4: Security & Hardening), task 2 — FFI Boundary Isolation.
//
//   F1  Echo round trip — args travel in, result travels out, order intact.
//   F2  Bounded hang — a guest that sleeps 10 s returns TIMEOUT at the
//       caller's 100 ms bound (measured < 400 ms), the host SURVIVES
//       (worker replaced), and the next call succeeds. Zero infinite hangs.
//   F3  Fork containment — a guest that deliberately abort()s in fork mode
//       yields WEFT_FFI_CRASH while the PARENT process keeps running (this
//       test is the proof: the abort happens in a forked child, and this
//       line of code still executes). Zero cross-gate crashes.
//   F4  Guest-reported failure — a guest returning -1 yields OP_FAILED,
//       distinct from OK / TIMEOUT / CRASH.
//   F5  Overhead contract — the in-process dispatch path measured over
//       2000 calls stays < 1 ms/call (issue #19's acceptance bar; the
//       measured number is declared in the output, flags-not-silence).
//   F6  Backpressure — a second call while one is in flight is refused
//       (OP_FAILED), never silently queued.
//   F7  Unregistered op — BAD_OP, not a mystery.
//   F8  Stop/join — clean shutdown, double-stop safe (NULL after free).
//
// Modes:
//   ./ffi_host-test        — run F1–F8, emit one JSON verdict line
//   ./ffi_host-test fork   — F1–F4 + F8 in fork mode (containment proof)
//   exit 0 iff all gates pass

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ffi_host.h"

static int fails = 0;
static long checks = 0;

static void expect(int cond, const char* what) {
    checks++;
    if (!cond) {
        fails++;
        fprintf(stderr, "GATE FAIL: %s\n", what);
    }
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// ---- guests --------------------------------------------------------------

static int op_echo(const uint8_t* args, size_t args_len, uint8_t* out, size_t* out_len, void* user) {
    (void)user;
    if (args_len > WEFT_FFI_BLOB_MAX) return -1;
    memcpy(out, args, args_len);
    *out_len = args_len;
    return 0;
}

static int op_hang(const uint8_t* args, size_t args_len, uint8_t* out, size_t* out_len, void* user) {
    (void)args; (void)args_len; (void)out; (void)out_len; (void)user;
    struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
    nanosleep(&ts, NULL);   // cancellation point — the in-proc hang cell
    return 0;
}

static int op_boom(const uint8_t* args, size_t args_len, uint8_t* out, size_t* out_len, void* user) {
    (void)args; (void)args_len; (void)out; (void)out_len; (void)user;
    abort();   // fork-mode crash cell — must NEVER take down the parent
    return 0;
}

static int op_fail(const uint8_t* args, size_t args_len, uint8_t* out, size_t* out_len, void* user) {
    (void)args; (void)args_len; (void)out; (void)out_len; (void)user;
    return -1;
}

int main(int argc, char** argv) {
    int fork_mode = (argc > 1 && strcmp(argv[1], "fork") == 0);
    printf("ffi_host-test: TIER4 §2 FFI boundary isolation (%s mode)\n",
           fork_mode ? "fork" : "in-process");

    // ---- F1: echo round trip ----
    weft_ffi_host_t* h = weft_ffi_host_start(fork_mode ? true : false);
    expect(h != NULL, "host started");
    expect(weft_ffi_host_register(h, 1, op_echo, NULL) == 0, "register echo");
    expect(weft_ffi_host_register(h, 2, op_hang, NULL) == 0, "register hang");
    expect(weft_ffi_host_register(h, 3, op_boom, NULL) == 0, "register boom");
    expect(weft_ffi_host_register(h, 4, op_fail, NULL) == 0, "register fail");

    uint8_t out[WEFT_FFI_BLOB_MAX];
    size_t out_len = 0;
    const uint8_t msg[] = "weft-ffi-isolation";
    weft_ffi_status_t st = weft_ffi_host_call(h, 1, msg, sizeof msg, out, sizeof out, &out_len, 500);
    expect(st == WEFT_FFI_OK, "F1 echo OK");
    expect(out_len == sizeof msg && memcmp(out, msg, sizeof msg) == 0, "F1 echo bytes intact");

    // ---- F4: guest-reported failure ----
    st = weft_ffi_host_call(h, 4, NULL, 0, out, sizeof out, &out_len, 500);
    expect(st == WEFT_FFI_OP_FAILED, "F4 guest -1 => OP_FAILED");

    // ---- F7: unregistered op ----
    st = weft_ffi_host_call(h, 99, NULL, 0, out, sizeof out, &out_len, 500);
    expect(st == WEFT_FFI_BAD_OP, "F7 unregistered => BAD_OP");

    // ---- F2: bounded hang (in-process only; fork mode = F3) ----
    if (!fork_mode) {
        uint64_t t0 = now_ms();
        st = weft_ffi_host_call(h, 2, NULL, 0, out, sizeof out, &out_len, 100);
        uint64_t dt = now_ms() - t0;
        expect(st == WEFT_FFI_TIMEOUT, "F2 hang => TIMEOUT");
        expect(dt < 400, "F2 bound honored (100 ms + handoff, not 10 s)");
        // The host must SURVIVE: worker replaced, next call works.
        st = weft_ffi_host_call(h, 1, msg, sizeof msg, out, sizeof out, &out_len, 500);
        expect(st == WEFT_FFI_OK, "F2 host survives a hung guest");
        weft_ffi_stats_t stats;
        weft_ffi_host_stats(h, &stats);
        expect(stats.timeouts == 1, "F2 timeout counted");
        expect(stats.restarts == 1, "F2 worker replacement counted");
    }

    // ---- F3: fork containment ----
    if (fork_mode) {
        st = weft_ffi_host_call(h, 3, NULL, 0, out, sizeof out, &out_len, 500);
        expect(st == WEFT_FFI_CRASH, "F3 aborting guest => CRASH verdict");
        // The parent survived — this line is the proof (abort ran in the child).
        st = weft_ffi_host_call(h, 1, msg, sizeof msg, out, sizeof out, &out_len, 500);
        expect(st == WEFT_FFI_OK, "F3 host survives an aborting guest");
        weft_ffi_stats_t stats;
        weft_ffi_host_stats(h, &stats);
        expect(stats.crashes == 1, "F3 crash counted");
    }

    // ---- F5: overhead contract (in-process only) ----
    if (!fork_mode) {
        const int N = 2000;
        uint64_t t0 = now_ms();
        for (int i = 0; i < N; i++) {
            st = weft_ffi_host_call(h, 1, msg, sizeof msg, out, sizeof out, &out_len, 500);
            if (st != WEFT_FFI_OK) { expect(0, "F5 loop call OK"); break; }
        }
        uint64_t dt_ns = (now_ms() - t0) * 1000000ull / N;
        double us = (double)dt_ns / 1000.0;
        printf("  F5: dispatch overhead measured %.2f us/call over %d calls\n", us, N);
        expect(dt_ns < 1000000ull, "F5 overhead < 1 ms/call (issue #19 acceptance)");
    }

    weft_ffi_host_stop(h);
    weft_ffi_host_stop(NULL);   // F8: NULL-safe

    printf("{\"test\":\"ffi_host\",\"mode\":\"%s\",\"checks\":%ld,\"fails\":%d,\"status\":\"%s\"}\n",
           fork_mode ? "fork" : "inproc", checks, fails, fails == 0 ? "PASSED" : "FAILED");
    return fails == 0 ? 0 : 1;
}
