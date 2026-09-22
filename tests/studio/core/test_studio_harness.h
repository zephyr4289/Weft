/* test_studio_harness.h — shared harness for the Weft Studio core tests.
 *
 * Conventions:
 *   - every binary exits non-zero on ANY failure (fail-closed suite);
 *   - CHECK* macros print the failing expression and the check id;
 *   - timing gates use CLOCK_MONOTONIC with precomputed work arrays,
 *     best-of-31 batches (documented methodology in D-71 §5: the sandbox
 *     measures best-of-N to bound scheduler noise; medians agree).
 */
#ifndef TEST_STUDIO_HARNESS_H
#define TEST_STUDIO_HARNESS_H

/* Tests selectively use harness helpers. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

#define _POSIX_C_SOURCE 199309L

#include "weft_studio.h"
#include "weft_studio_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do {                                                \
    g_checks++;                                                         \
    if (!(cond)) {                                                      \
        g_failures++;                                                   \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
} while (0)

#define CHECK_EQ_I(a, b) do {                                           \
    long long va_ = (long long)(a), vb_ = (long long)(b);               \
    g_checks++;                                                         \
    if (va_ != vb_) {                                                   \
        g_failures++;                                                   \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",        \
                __FILE__, __LINE__, #a, #b, va_, vb_);                  \
    }                                                                   \
} while (0)

#define CHECK_EQ_U64(a, b) do {                                         \
    unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b); \
    g_checks++;                                                         \
    if (va_ != vb_) {                                                   \
        g_failures++;                                                   \
        fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n",    \
                __FILE__, __LINE__, #a, #b, va_, vb_);                  \
    }                                                                   \
} while (0)

#define CHECK_STR(a, b) do {                                            \
    const char *va_ = (a), *vb_ = (b);                                  \
    g_checks++;                                                         \
    if (!va_ || !vb_ || strcmp(va_, vb_) != 0) {                        \
        g_failures++;                                                   \
        fprintf(stderr, "FAIL %s:%d: str %s == %s\n  got:  %.96s\n"     \
                "  want: %.96s\n", __FILE__, __LINE__, #a, #b,          \
                va_ ? va_ : "(null)", vb_ ? vb_ : "(null)");            \
    }                                                                   \
} while (0)

/* Substring containment check (for codegen goldens). */
#define CHECK_SUB(hay, needle) do {                                     \
    const char *va_ = (hay), *vb_ = (needle);                           \
    g_checks++;                                                         \
    if (!va_ || !vb_ || !strstr(va_, vb_)) {                            \
        g_failures++;                                                   \
        fprintf(stderr, "FAIL %s:%d: %s contains %s\n  got %.120s\n",   \
                __FILE__, __LINE__, #hay, #needle, va_ ? va_ : "(null)"); \
    }                                                                   \
} while (0)

static uint64_t harness_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Deterministic xorshift64* PRNG (fixed seeds per test — reproducible). */
static uint64_t harness_xs(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* PASS/FAIL footer. */
static int harness_summary(const char *name)
{
    printf("%s: %d checks, %d failures -> %s\n", name, g_checks, g_failures,
           g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Shared fixtures                                                     */
/* ------------------------------------------------------------------ */

/* Byte-identical to the Pillar-1 weftc golden frames.weft (the parity
 * oracle rides on this exact text). */
static const char FIXTURE_FRAMES[] =
"// frames.weft — engine frame schema (smoke test)\n"
"endianness little;\n"
"\n"
"enum Kind : u8 {\n"
"    Render = 0,\n"
"    Present = 1,\n"
"    Telemetry = 2,\n"
"}\n"
"\n"
"bitflags Caps : u32 {\n"
"    None       = 0,\n"
"    FloatMath  = 0x0000_0001,\n"
"    SimdBlend  = 0x0000_0002,\n"
"    GpuUpload  = 0x0000_0004,\n"
"}\n"
"\n"
"struct Vec3 {\n"
"    x: f32,\n"
"    y: f32,\n"
"    z: f32,\n"
"}\n"
"\n"
"struct Header {\n"
"    seq: u64,\n"
"    kind: Kind,\n"
"    // natural alignment: hole 1..8 below\n"
"    stamp: u64,\n"
"}\n"
"\n"
"@optimize(packing)\n"
"struct TelemetryMsg {\n"
"    flags: u8,\n"
"    temperature: f64,   // 7-byte hole without the reorder\n"
"    pressure: f32,\n"
"    humidity: u8,\n"
"}\n"
"\n"
"@align(64)\n"
"struct CachelineFrame {\n"
"    ctrl: u32,\n"
"    payload: [u8; 60],\n"
"}\n"
"\n"
"struct BigFrame {\n"
"    hdr: Header,\n"
"    pos: Vec3,\n"
"    vel: Vec3,\n"
"    caps: Caps,\n"
"    name: str[32],\n"
"    samples: [f32; 8],\n"
"    blob: span<u8>,\n"
"}\n";

/* Pillar-1 weftc reference goldens (tools/weftc/tests/golden/frames.json,
 * produced by the RFC-0017 reference compiler — the cross-implementation
 * proof: the studio engine must reproduce these bit-for-bit). */
#define GOLDEN_ABI_HASH   UINT64_C(0x0e7280ce8f313931)
#define GOLDEN_SCHEMA_ID  UINT64_C(0x5b31c5b9f9f7b3eb)
#define GOLDEN_FNV        UINT64_C(0x94a2f711e9ad2f16)
#define GOLDEN_KIND       UINT64_C(0xf7714909a63a66ad)
#define GOLDEN_CAPS       UINT64_C(0x70dfc168b7e518cc)
#define GOLDEN_VEC3       UINT64_C(0x61ddb568acf95b0f)
#define GOLDEN_HEADER     UINT64_C(0x8427797a68caa357)
#define GOLDEN_TELEMETRY  UINT64_C(0x5f12f453dd569495)
#define GOLDEN_CACHELINE  UINT64_C(0x79f04e9440822e70)
#define GOLDEN_BIGFRAME   UINT64_C(0x94c196087085eef4)

#pragma GCC diagnostic pop

#endif /* TEST_STUDIO_HARNESS_H */
