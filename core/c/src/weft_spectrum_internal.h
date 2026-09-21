// weft_spectrum_internal.h — internal (non-ABI) surface shared by the
// weft-spectrum core (weft_hw_probe.c, weft_governor.c) and the test
// harness (tests/spectrum/). NOT part of the frozen public ABI and NOT
// installed for consumers: Engineer 2 / Engineer 3 consume
// core/c/include/weft_spectrum.h only.
//
// Two internal seams are exposed here:
//   1. weft_probe_source_t — the OS source abstraction (auxv, sysctl,
//      sysprop, read_file, file_exists, sysconf, arch_hint, now_ns).
//      The real per-OS implementations live in weft_hw_probe.c; the
//      deterministic mock tables for the 12 golden archetypes live in
//      tests/spectrum/weft_mock_archetypes.c. Injection is init-time
//      only (before the first probe, before going multithreaded).
//   2. weft_crc32c_* / weft_fnv1a64 — the frozen integrity primitives
//      (bitwise CRC-32C, no tables, no init, fully deterministic).
//
// Weft Core Laws apply unchanged: zero allocation on every path here.

#ifndef WEFT_SPECTRUM_INTERNAL_H
#define WEFT_SPECTRUM_INTERNAL_H

#include "weft_spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Probe architecture ids (runtime dispatch)                           */
/*                                                                     */
/* The ISA interpretation path is selected at RUNTIME from the active  */
/* source's arch_hint() so that the full x86-64 / arm64 / riscv64      */
/* interpretation matrix is exercisable on any host via the mock      */
/* archetypes (100% branch coverage without exotic hardware). The     */
/* default (real) source returns the compile-time host architecture;  */
/* genuinely host-specific syscalls (e.g. prctl PR_SVE_GET_VL) stay    */
/* compile-time guarded and simply do not fire on foreign hosts —     */
/* the profile then reports the architectural minimum, honestly.      */
/* ------------------------------------------------------------------ */

enum weft_probe_arch {
    WEFT_PROBE_ARCH_X86_64 = 0,
    WEFT_PROBE_ARCH_ARM64  = 1,
    WEFT_PROBE_ARCH_RISCV64 = 2,
    WEFT_PROBE_ARCH_UNKNOWN = 3
};

/* ------------------------------------------------------------------ */
/* Source operations vtable                                            */
/*                                                                     */
/* Contract (all zero-allocation, all bounded, all fail-closed):       */
/*   auxv(type)      -> hwcap word for AT_* type; 0 = unavailable      */
/*   sysctl(name)    -> 0 on success (-1 fail), value NUL-terminated   */
/*   sysprop(key)    -> same contract as sysctl (Android properties)   */
/*   read_file(path) -> bytes read (<= cap-1, NUL-terminated) or -1    */
/*   file_exists(p)  -> 1 present / 0 absent                           */
/*   sysconf(name)   -> _SC_NPROCESSORS_ONLN-style value; -1 = fail    */
/*   arch_hint()     -> enum weft_probe_arch the source simulates      */
/*   now_ns()        -> monotonic nanoseconds (mocks: 0 = frozen)      */
/* ------------------------------------------------------------------ */

typedef struct weft_probe_source {
    uint64_t (*auxv)(uint32_t type);
    int      (*sysctl)(const char *name, char *buf, size_t cap);
    int      (*sysprop)(const char *key, char *buf, size_t cap);
    int      (*read_file)(const char *path, char *buf, size_t cap);
    int      (*file_exists)(const char *path);
    long     (*sysconf)(int name);
    uint32_t (*arch_hint)(void);
    uint64_t (*now_ns)(void);
} weft_probe_source_t;

/* Select the active source (NULL = per-OS default). Init-time only:
 * MUST be called before the first weft_hw_probe() and before the
 * process goes multithreaded (single governance thread contract). */
void weft_probe_set_source(const weft_probe_source_t *source);

/* Currently active source (never NULL after first use). */
const weft_probe_source_t *weft_probe_get_source(void);

/* ------------------------------------------------------------------ */
/* Frozen integrity primitives                                         */
/* ------------------------------------------------------------------ */

/* Bitwise reflected CRC-32C (Castagnoli poly 0x1EDC6F41, init/final
 * 0xFFFFFFFF). Table-free and init-free: deterministic on every host,
 * cheap enough for the one-shot probe/seal paths (queries never CRC). */
uint32_t weft_crc32c(const void *data, size_t len);

/* FNV-1a 64 — used for the frozen schema signatures. */
uint64_t weft_fnv1a64(const char *s);

/* ------------------------------------------------------------------ */
/* Golden archetype surface (tests/spectrum/weft_mock_archetypes.c)    */
/*                                                                     */
/* 12 deterministic hardware archetypes spanning the silicon spectrum */
/* the mission mandates — flagship workstations down to 2-4 GB budget */
/* phones and RISC-V boards. Each archetype is a full mock SOURCE      */
/* (synthetic auxv / cpuinfo / sysfs / sysctl / properties) so the    */
/* REAL probe pipeline runs end-to-end and its output is byte-frozen  */
/* into the committed golden ABI fixtures.                             */
/* ------------------------------------------------------------------ */

enum weft_archetype_id {
    WEFT_ARCHETYPE_APPLE_M4_MAX       = 0,
    WEFT_ARCHETYPE_APPLE_A17_PRO      = 1,
    WEFT_ARCHETYPE_SNAPDRAGON_8GEN3   = 2,
    WEFT_ARCHETYPE_DIMENSITY_9300     = 3,
    WEFT_ARCHETYPE_HELIO_G88          = 4,
    WEFT_ARCHETYPE_RASPBERRY_PI_5     = 5,
    WEFT_ARCHETYPE_VISIONFIVE_2       = 6,
    WEFT_ARCHETYPE_EPYC_9654          = 7,
    WEFT_ARCHETYPE_XEON_SPR_AMX       = 8,
    WEFT_ARCHETYPE_GRACE_HOPPER       = 9,
    WEFT_ARCHETYPE_DESKTOP_ZEN4       = 10,
    WEFT_ARCHETYPE_CONTAINER_FALLBACK = 11,
    WEFT_ARCHETYPE_COUNT               = 12
};

/* Select archetype as the active mock source (fails ENOARCHETYPE on a
 * bad id). Also resets the deterministic mock clock to 0. */
weft_spectrum_status_t weft_mock_archetype_select(uint32_t archetype_id);

/* Archetype display name (static storage, never NULL for valid ids). */
const char *weft_mock_archetype_name(uint32_t archetype_id);

/* Restore the per-OS default source (call after mock-based tests). */
void weft_mock_archetype_clear(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFT_SPECTRUM_INTERNAL_H */
