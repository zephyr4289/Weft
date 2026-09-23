// weft_cluster_core.h — RFC-0019 §2.3: shared cluster-transport plumbing
// (status ladder, capability probes, the house dlopen discipline, and
// Law-2 deadline helpers).
//
// WHY EXISTS: four engines, one honesty surface. Every backend reports
// through the SAME named status ladder (a refusal code a caller can
// switch on, never an errno soup), probes the SAME capabilities
// (CAP_NET_ADMIN/CAP_BPF/CAP_NET_RAW/CAP_IPC_LOCK via /proc/self/status
// CapEff — the effective-set source of truth, no libcap), resolves its
// shared library through the SAME dlopen-first helper (the gpu_ring /
// weft_vk_bridge rule: zero link-time hard dependencies), and bounds
// every wait through the SAME deadline arithmetic (Law 2: a poll loop
// without a deadline is a hang, not a fast path).
//
// STATUS PHILOSOPHY (Law 4, operationalized): transport-specific detail
// lives in the engine's own err[] buffer; the STATUS is the switchable
// fact. The fabric selector (weft_cluster_fabric) routes on exactly
// these codes: DRIVER_NOT_FOUND / HW_ABSENT / PERMS / SYS_UNSUPPORTED
// are all honest refusals that demote the cascade — they never crash,
// never fake a pass, and never block the fallback roads.

#ifndef WEFT_CLUSTER_CORE_H
#define WEFT_CLUSTER_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Named status ladder (shared; engines map their failures onto it)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_CLUSTER_OK              = 0,

    // generic
    WEFT_CLUSTER_E_INVALID_ARG   = -1,   ///< NULL/geometry/config refusal
    WEFT_CLUSTER_E_NO_MEMORY     = -2,   ///< setup allocation failure
    WEFT_CLUSTER_E_TIMEOUT       = -3,   ///< Law 2 deadline expired
    WEFT_CLUSTER_E_BUSY          = -4,   ///< bounded pool exhausted
    WEFT_CLUSTER_E_IO            = -5,   ///< syscall/hw op failed (err[])
    WEFT_CLUSTER_E_STATE         = -6,   ///< wrong session state / order
    WEFT_CLUSTER_E_UNSUPPORTED   = -7,   ///< feature not in this build/kernel

    // honest refusals (Law 4 — each demotes the fabric cascade)
    WEFT_CLUSTER_E_DRIVER        = -100, ///< dlopen failed: library absent
    WEFT_CLUSTER_E_HW_ABSENT     = -101, ///< library present, device absent
    WEFT_CLUSTER_E_PERMS         = -102, ///< CAP_NET_ADMIN/BPF/RAW missing
    WEFT_CLUSTER_E_SYS           = -103, ///< kernel lacks the syscall/family
} weft_cluster_status_t;

/// Short name for logs ("ok"/"driver_absent"/...).
const char* weft_cluster_status_str(weft_cluster_status_t st);

// ---------------------------------------------------------------------------
// Capability probes (cached; /proc/self/status CapEff — the truth source)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_CAP_NET_RAW     = 13,
    WEFT_CAP_NET_ADMIN   = 12,
    WEFT_CAP_IPC_LOCK    = 14,
    WEFT_CAP_SYS_ADMIN   = 21,
    WEFT_CAP_PERFMON     = 38,
    WEFT_CAP_BPF         = 39,
} weft_cap_t;

/// 1 if the effective capability set holds `cap` (cached after first
/// read; a missing /proc reports 0 — the conservative refusal).
int weft_cap_effective(weft_cap_t cap);

/// One capability line for evidence logs (e.g. "caps: net_raw=0
/// net_admin=0 bpf=0 ipc_lock=0 — XDP/BPF legs will refuse (Law 4)").
size_t weft_caps_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// The house dlopen discipline (gpu_ring / weft_vk_bridge rule)
// ---------------------------------------------------------------------------

/// dlopen the FIRST name in `names` that loads. Returns the handle and
/// stores the winning index in *which (may be NULL). NULL when none
/// load — the caller turns that into WEFT_CLUSTER_E_DRIVER with the
/// attempted names in its err[] (an honest refusal, never a guess).
void* weft_dlopen_first(const char* const names[], int count, int* which);

// ---------------------------------------------------------------------------
// Law-2 deadline helpers (every bounded poll uses these)
// ---------------------------------------------------------------------------

/// CLOCK_MONOTONIC now (nanoseconds).
uint64_t weft_now_ns(void);

/// Deadline `timeout_ns` in the future.
uint64_t weft_deadline_after(uint64_t timeout_ns);

/// Remaining ns until `deadline` (0 when reached/passed).
uint64_t weft_deadline_remaining(uint64_t deadline);

/// A single cooperative yield for bounded poll fallbacks (cheaper than a
/// spin; never sleeps unbounded — Law 2's no-hang law for poll loops).
void weft_poll_yield(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_CLUSTER_CORE_H
