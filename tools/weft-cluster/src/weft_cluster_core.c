// weft_cluster_core.c — shared plumbing (see weft_cluster_core.h).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "weft_cluster_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <dlfcn.h>

const char* weft_cluster_status_str(weft_cluster_status_t st) {
    switch (st) {
        case WEFT_CLUSTER_OK:              return "ok";
        case WEFT_CLUSTER_E_INVALID_ARG:   return "invalid_arg";
        case WEFT_CLUSTER_E_NO_MEMORY:     return "no_memory";
        case WEFT_CLUSTER_E_TIMEOUT:       return "timeout";
        case WEFT_CLUSTER_E_BUSY:          return "busy";
        case WEFT_CLUSTER_E_IO:            return "io";
        case WEFT_CLUSTER_E_STATE:         return "bad_state";
        case WEFT_CLUSTER_E_UNSUPPORTED:   return "unsupported";
        case WEFT_CLUSTER_E_DRIVER:        return "driver_absent";
        case WEFT_CLUSTER_E_HW_ABSENT:     return "hw_absent";
        case WEFT_CLUSTER_E_PERMS:         return "perms_refused";
        case WEFT_CLUSTER_E_SYS:           return "sys_unsupported";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

static unsigned long long g_cap_eff = 0;
static int g_cap_read = 0;

static void caps_read_once(void) {
    if (g_cap_read) return;
    g_cap_read = 1;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;  // conservative: nothing held
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "CapEff:", 7) == 0) {
            g_cap_eff = strtoull(line + 7, NULL, 16);
            break;
        }
    }
    fclose(f);
}

int weft_cap_effective(weft_cap_t cap) {
    caps_read_once();
    return (g_cap_eff >> (unsigned)cap) & 1ull;
}

size_t weft_caps_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen,
                     "caps: net_raw=%d net_admin=%d bpf=%d perfmon=%d "
                     "ipc_lock=%d sys_admin=%d",
                     weft_cap_effective(WEFT_CAP_NET_RAW),
                     weft_cap_effective(WEFT_CAP_NET_ADMIN),
                     weft_cap_effective(WEFT_CAP_BPF),
                     weft_cap_effective(WEFT_CAP_PERFMON),
                     weft_cap_effective(WEFT_CAP_IPC_LOCK),
                     weft_cap_effective(WEFT_CAP_SYS_ADMIN));
    return n > 0 ? (size_t)n : 0;
}

// ---------------------------------------------------------------------------
// dlopen discipline
// ---------------------------------------------------------------------------

void* weft_dlopen_first(const char* const names[], int count, int* which) {
    for (int i = 0; i < count; i++) {
        void* h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            if (which) *which = i;
            return h;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

uint64_t weft_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t weft_deadline_after(uint64_t timeout_ns) {
    return weft_now_ns() + timeout_ns;
}

uint64_t weft_deadline_remaining(uint64_t deadline) {
    uint64_t now = weft_now_ns();
    return (now >= deadline) ? 0 : deadline - now;
}

void weft_poll_yield(void) { sched_yield(); }
