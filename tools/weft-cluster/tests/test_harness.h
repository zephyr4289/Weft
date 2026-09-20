// test_harness.h — the CL-series gate micro-harness (house style: every
// gate prints [PASS]/[FAIL] with a number; exit code is the failure
// count; honest SKIP prints its reason and does NOT fail).

#ifndef WEFT_TEST_HARNESS_H
#define WEFT_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
static int g_pass = 0;
static int g_skip = 0;

#define GATE(name, cond)                                              \
    do {                                                              \
        if (cond) {                                                   \
            g_pass++;                                                 \
            printf("[PASS] %s\n", name);                              \
        } else {                                                      \
            g_fail++;                                                 \
            printf("[FAIL] %s  (%s:%d)\n", name, __FILE__, __LINE__); \
        }                                                             \
    } while (0)

#define GATEI(name, v, want)                                          \
    do {                                                              \
        const long long _v = (long long)(v);                          \
        const long long _w = (long long)(want);                       \
        if (_v == _w) {                                               \
            g_pass++;                                                 \
            printf("[PASS] %s\n", name);                              \
        } else {                                                      \
            g_fail++;                                                 \
            printf("[FAIL] %s (got %lld want %lld, %s:%d)\n",          \
                   name, _v, _w, __FILE__, __LINE__);                  \
        }                                                             \
    } while (0)

#define SKIP(name, why)                                               \
    do {                                                              \
        g_skip++;                                                     \
        printf("[SKIP] %s — %s\n", name, why);                        \
    } while (0)

#define TEST_EXIT()                                                   \
    do {                                                              \
        printf("--- %d pass / %d fail / %d skip\n", g_pass, g_fail,   \
               g_skip);                                               \
        return g_fail == 0 ? 0 : 1;                                   \
    } while (0)

#endif // WEFT_TEST_HARNESS_H
