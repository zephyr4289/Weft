// qos_test.c — thread_qos flags honesty (Series 8).
#include "thread_qos.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int failures = 0;
    printf("== qos_test ==\n");
    /* A1: the big-core mask either abstains (0) or names online cores. */
    uint64_t mask = weft_qos_bigcore_mask();
    int online = (int)sysconf(_SC_NPROCESSORS_ONLN);
    int mask_ok = 1;
    for (int i = 0; i < 64; i++) {
        if ((mask >> i) & 1 && i >= online) mask_ok = 0;
    }
    printf("A1 big-core mask = 0x%llx (%s, %d online) %s\n",
           (unsigned long long)mask, mask ? "opinion" : "abstain", online,
           mask_ok ? "OK" : "BAD");
    if (!mask_ok) failures++;

    /* A2: applying the render preset never silently no-ops — the flags
     * name exactly what happened (affinity bits or none; no RT claim). */
    unsigned flags = weft_thread_apply_render_qos();
    printf("A2 render preset flags = 0x%x\n", flags);
    if (flags & (WEFT_QOS_APPLIED_SCHED | WEFT_QOS_SCHED_UNPRIVILEGED)) {
        fprintf(stderr, "A2-FAIL: render preset must not attempt RT\n");
        failures++;
    }

    /* A3: an explicit RT attempt in an unprivileged container yields the
     * DOCUMENTED flag, never APPLIED_SCHED-or-silence. */
    weft_qos_spec spec;
    __builtin_memset(&spec, 0, sizeof(spec));
    spec.cls = WEFT_QOS_USER_INTERACTIVE;
    spec.rt_priority = 1;
    flags = weft_thread_apply_qos(&spec);
    printf("A3 rt attempt flags = 0x%x (%s)\n", flags,
           (flags & WEFT_QOS_APPLIED_SCHED) ? "applied (privileged)"
                                            : "declared fallback");
    if (!(flags & (WEFT_QOS_APPLIED_SCHED | WEFT_QOS_SCHED_UNPRIVILEGED))) {
        fprintf(stderr, "A3-FAIL: RT attempt produced neither applied nor declared fallback\n");
        failures++;
    }

    /* A4: affinity to a single online core applies and reads back. */
    if (online > 0) {
        spec.cpu_mask = 1ull << 0;
        spec.rt_priority = 0;
        flags = weft_thread_apply_qos(&spec);
        cpu_set_t got;
        CPU_ZERO(&got);
        pthread_getaffinity_np(pthread_self(), sizeof(got), &got);
        int applied = CPU_ISSET(0, &got);
        printf("A4 affinity flags = 0x%x, core0 set readback = %d\n", flags, applied);
        if (!(flags & WEFT_QOS_APPLIED_AFFINITY) || !applied) {
            fprintf(stderr, "A4-FAIL: single-core affinity did not apply\n");
            failures++;
        }
        /* restore: allow all cores again */
        CPU_ZERO(&got);
        for (int i = 0; i < online; i++) CPU_SET(i, &got);
        pthread_setaffinity_np(pthread_self(), sizeof(got), &got);
    }

    if (failures) {
        fprintf(stderr, "qos_test: %d gate failure(s)\n", failures);
        return 1;
    }
    printf("qos_test: ALL GATES GREEN\n");
    return 0;
}
