// weft_test_util.h — the AC-series gate harness (house style: numbered
// gates, PASS/FAIL lines, non-zero exit on any failure, every skip says
// why and carries its label).

#ifndef WEFT_TEST_UTIL_H
#define WEFT_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_gates_passed = 0;
static int g_gates_failed = 0;
static int g_gates_skipped = 0;

#define GATE(id, cond)                                                  \
    do {                                                                \
        if (cond) {                                                     \
            printf("%s: PASS\n", id);                                   \
            g_gates_passed++;                                           \
        } else {                                                        \
            printf("%s: FAIL (%s:%d)\n", id, __FILE__, __LINE__);       \
            g_gates_failed++;                                           \
        }                                                               \
    } while (0)

#define GATE_SKIP(id, why)                                              \
    do {                                                                \
        printf("%s: SKIP (%s)\n", id, why);                             \
        g_gates_skipped++;                                              \
    } while (0)

#define GATE_SUMMARY(name)                                              \
    do {                                                                \
        printf("%s: %d passed, %d failed, %d skipped\n", name,          \
               g_gates_passed, g_gates_failed, g_gates_skipped);        \
        return g_gates_failed ? 1 : 0;                                  \
    } while (0)

#endif // WEFT_TEST_UTIL_H
