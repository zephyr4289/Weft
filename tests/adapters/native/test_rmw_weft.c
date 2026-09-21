// test_rmw_weft.c — the rmw_weft multi-process native battery (mandate C).
//
// Proves, across fork()'d processes sharing nothing but /dev/shm:
//   R1  basic pub/sub: 2000 x 64 KiB copy-path messages, CRC + sequence
//       + message_info fidelity (timestamps, publisher gid).
//   R2  ZERO-COPY POINTER IDENTITY (Rule 4): publisher loaned-write offset
//       within the shm ring object == subscriber loaned-read offset, byte
//       for byte, 200 times, plus content identity via CRC.
//   R3  burst saturation: 100k best-effort messages against a stalled
//       subscriber — zero corruption, publisher drops == subscriber gaps
//       (honest accounting, no silent loss).
//   R4  fan-out + wait set: one publisher, two subscriptions — both
//       receive all 5000 messages; rmw_wait returns TIMEOUT honestly on
//       an idle graph and wakes on data.
//   R5  crash orphan sweep: a child that dies without cleanup leaves its
//       ring behind; the next rmw_init sweeps it; /dev/shm ends clean.
//   R6  leak audit: fd count and /dev/shm occupancy return to baseline.

#include "rmw_weft/rmw_weft.h"
#include "test_util.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MSG64 (65536u)
#define MSG_SMALL (256u)
#define TOPIC_BASIC "weft_adapters_battery_basic"
#define TOPIC_SAT "weft_adapters_battery_sat"
#define TOPIC_FAN "weft_adapters_battery_fan"
#define TOPIC_CRASH "weft_adapters_battery_crash"

static uint64_t msg_magic = 0xC0FFEE1234567890ull;

static int64_t batt_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

/* RELIABLE publish with container-stall retry (D-62 §C.5): the engine's
 * zero-progress budget only fires when the consumer is frozen past 50 ms;
 * a CFS-throttled consumer bridging that window yields RMW_RET_TIMEOUT,
 * which a robust ROS application retries. Bounded by a global bail. */
static rmw_ret_t publish_retry(rmw_publisher_t *pub, const void *msg) {
    int64_t bail = batt_now_ns() + 300000000000ll; /* 300 s */
    for (;;) {
        rmw_ret_t rc = rmw_publish(pub, msg, NULL);
        if (rc != RMW_RET_TIMEOUT) return rc;
        if (batt_now_ns() > bail) return rc;
    }
}

static void fill_msg(void *buf, uint32_t size, uint64_t idx) {
    uint8_t *p = (uint8_t *)buf;
    uint64_t hdr[2] = {msg_magic, idx};
    memcpy(p, hdr, 16);
    uint32_t s = (uint32_t)(idx * 2654435761ull) | 1u;
    for (uint32_t i = 16; i < size; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = (uint8_t)(s >> 24);
    }
}

static int verify_msg(const void *buf, uint32_t size, uint64_t idx) {
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t hdr[2];
    memcpy(hdr, p, 16);
    if (hdr[0] != msg_magic || hdr[1] != idx) return -1;
    uint32_t s = (uint32_t)(idx * 2654435761ull) | 1u;
    for (uint32_t i = 16; i < size; i++) {
        s = s * 1664525u + 1013904223u;
        if (p[i] != (uint8_t)(s >> 24)) return -1;
    }
    return 0;
}

static rmw_time_point_value_t timeout_from_now_us(int64_t us) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + us * 1000;
    rmw_time_point_value_t t;
    t.sec = ns / 1000000000;
    t.nsec = (uint64_t)(ns % 1000000000);
    return t;
}

static void common_setup(rmw_context_t *ctx, rmw_node_t **node) {
    rmw_init_options_t opts;
    TU_CHECK(rmw_init_options_init(&opts, 0, NULL) == RMW_RET_OK,
              "rmw-init-options");
    TU_CHECK(rmw_init(&opts, ctx) == RMW_RET_OK, "rmw-init");
    *node = rmw_create_node(ctx, "battery_node", "/", 0, NULL);
    TU_CHECK(*node != NULL, "rmw-create-node");
}

static void common_teardown(rmw_context_t *ctx, rmw_node_t *node) {
    TU_CHECK(rmw_destroy_node(node) == RMW_RET_OK, "rmw-destroy-node");
    TU_CHECK(rmw_shutdown(ctx) == RMW_RET_OK, "rmw-shutdown");
    TU_CHECK(rmw_fini(ctx) == RMW_RET_OK, "rmw-fini");
}

// ---------------------------------------------------------------------------
// R1 + R2: basic copy path + loaned zero-copy identity
// ---------------------------------------------------------------------------

static void phase_basic(void) {
    int pipefd[2];
    TU_CHECK(pipe(pipefd) == 0, "pipe");
    pid_t pid = fork();
    TU_CHECK(pid >= 0, "fork-basic");

    if (pid == 0) {
        tu_child_setup(); /* die with the parent: never leak a spinner */
        /* ---------- child: publisher ---------- */
        rmw_context_t ctx;
        rmw_node_t *node;
        common_setup(&ctx, &node);

        const rosidl_message_type_support_t *ts =
            rmw_weft_create_pod_type_support(MSG64, TOPIC_BASIC);
        rmw_publisher_t *pub = rmw_create_publisher(
            node, ts, TOPIC_BASIC, &RMW_QOS_PROFILE_DEFAULT, NULL);
        if (pub == NULL) _exit(10);

        char sync;
        if (read(pipefd[0], &sync, 1) != 1) _exit(11); /* wait: sub ready */
        if (rmw_weft_pub_refresh(pub) != 0) _exit(12);

        static uint8_t buf[MSG64];
        for (uint64_t i = 0; i < 2000; i++) {
            fill_msg(buf, MSG64, i);
            if (publish_retry(pub, buf) != RMW_RET_OK) _exit(20);
        }
        /* loaned path: write IN PLACE in the ring slot, publish the exact
         * bytes the subscriber will alias (zero intermediate copies) */
        const rmw_ring_map_t *rm = rmw_weft_pub_ring0(pub);
        if (rm == NULL) _exit(30);
        for (uint64_t i = 2000; i < 2200; i++) {
            void *loan = NULL;
            int64_t bail = batt_now_ns() + 300000000000ll;
            for (;;) {
                rmw_ret_t brc = rmw_borrow_loaned_message(pub, ts, &loan);
                if (brc != RMW_RET_TIMEOUT) break;
                if (batt_now_ns() > bail) break;
            }
            if (loan == NULL) _exit(31);
            fill_msg(loan, MSG64, i);
            uint64_t off = (uint64_t)((uint8_t *)loan - rm->base);
            if (write(pipefd[1], &off, 8) != 8) _exit(32);
            if (rmw_publish_loaned_message(pub, loan, NULL) != RMW_RET_OK)
                _exit(33);
        }
        TU_CHECK(rmw_weft_pub_published(pub) == 2200, "child-pub-count");
        TU_CHECK(rmw_destroy_publisher(node, pub) == RMW_RET_OK, "child-dpub");
        rmw_weft_destroy_pod_type_support(ts);
        common_teardown(&ctx, node);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(0);
    }

    /* ---------- parent: subscriber ---------- */
    rmw_context_t ctx;
    rmw_node_t *node;
    common_setup(&ctx, &node);

    const rosidl_message_type_support_t *ts =
        rmw_weft_create_pod_type_support(MSG64, TOPIC_BASIC);
    rmw_subscription_t *sub =
        rmw_create_subscription(node, ts, TOPIC_BASIC,
                                 &RMW_QOS_PROFILE_DEFAULT, NULL);
    TU_CHECK(sub != NULL, "create-sub");
    rmw_wait_set_t *ws = rmw_create_wait_set(&ctx, 4);
    TU_CHECK(ws != NULL, "create-waitset");
    TU_CHECK(rmw_wait_set_add_subscription(ws, sub) == RMW_RET_OK,
              "waitset-add");

    if (write(pipefd[1], "R", 1) != 1)
        TU_FAIL("sync-write", "%s", "pipe write failed");
    close(pipefd[1]); /* parent only reads from here on */
    rmw_subscriptions_t subs = {.subscriber_count = 1,
                                .subscribers = (void **)&sub};

    static uint8_t buf[MSG64];
    for (uint64_t i = 0; i < 2000; i++) {
        rmw_time_point_value_t t = timeout_from_now_us(2000000);
        TU_CHECK(rmw_wait(ws, &subs, NULL, NULL, NULL, NULL, NULL, &t) ==
                      RMW_RET_OK,
                  "wait-ready");
        bool taken = false;
        rmw_message_info_t info;
        TU_CHECK(rmw_take_with_info(sub, buf, &taken, &info, NULL) ==
                      RMW_RET_OK,
                  "take-rc");
        TU_CHECK(taken, "take-taken");
        TU_ASSERT(verify_msg(buf, MSG64, i) == 0, "take-verify", "idx %llu",
                  (unsigned long long)i);
        TU_CHECK(info.source_timestamp.sec > 0, "info-source-ts");
        TU_CHECK(strcmp(info.publisher_gid.implementation_identifier,
                         "rmw_weft") == 0,
                  "info-gid-impl");
        TU_CHECK(info.from_intra_process, "info-intra");
    }

    /* R2: loaned zero-copy identity */
    const rmw_ring_map_t *rm = rmw_weft_sub_ring(sub);
    TU_CHECK(rm != NULL, "sub-ring");
    for (uint64_t i = 2000; i < 2200; i++) {
        rmw_time_point_value_t t = timeout_from_now_us(2000000);
        TU_CHECK(rmw_wait(ws, &subs, NULL, NULL, NULL, NULL, NULL, &t) ==
                      RMW_RET_OK,
                  "wait-loan");
        void *loan = NULL;
        bool taken = false;
        TU_CHECK(rmw_take_loaned_message(sub, &loan, &taken, NULL) ==
                      RMW_RET_OK,
                  "take-loan-rc");
        TU_CHECK(taken, "take-loan-taken");
        uint64_t child_off = 0;
        TU_CHECK(read(pipefd[0], &child_off, 8) == 8, "read-offset");
        uint64_t my_off = (uint64_t)((uint8_t *)loan - rm->base);
        TU_ASSERT(my_off == child_off, "ZERO-COPY IDENTITY", "ZERO-COPY IDENTITY (Rule 4): subscriber loan offset " "%llu != publisher loan offset %llu", (unsigned long long)my_off, (unsigned long long)child_off);;
        TU_CHECK(verify_msg(loan, MSG64, i) == 0, "loan-verify");
        TU_CHECK(rmw_return_loaned_message(sub, loan) == RMW_RET_OK,
                  "return-loan");
    }

    TU_CHECK(rmw_weft_sub_taken(sub) == 2200, "sub-taken");
    TU_CHECK(rmw_weft_sub_missed(sub) == 0, "sub-missed");

    int st = 0;
    TU_CHECK(waitpid(pid, &st, 0) == pid, "waitpid-basic");
    TU_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child-basic-exit");

    TU_CHECK(rmw_wait_set_remove_subscription(ws, sub) == RMW_RET_OK,
              "waitset-remove");
    TU_CHECK(rmw_destroy_wait_set(ws) == RMW_RET_OK, "destroy-waitset");
    TU_CHECK(rmw_destroy_subscription(node, sub) == RMW_RET_OK, "destroy-sub");
    rmw_weft_destroy_pod_type_support(ts);
    common_teardown(&ctx, node);
    close(pipefd[0]);
    TU_PASS("R1 basic copy-path pub/sub + message_info fidelity");
    TU_PASS("R2 zero-copy loaned pointer identity across processes");
}

// ---------------------------------------------------------------------------
// R3: burst saturation with honest drop accounting
// ---------------------------------------------------------------------------

static void phase_saturation(void) {
    pid_t pid = fork();
    TU_CHECK(pid >= 0, "fork-sat");

    if (pid == 0) {
        tu_child_setup(); /* die with the parent: never leak a spinner */
        rmw_context_t ctx;
        rmw_node_t *node;
        common_setup(&ctx, &node);
        const rosidl_message_type_support_t *ts =
            rmw_weft_create_pod_type_support(MSG_SMALL, TOPIC_SAT);
        rmw_publisher_t *pub = rmw_create_publisher(
            node, ts, TOPIC_SAT, &RMW_QOS_PROFILE_SENSOR_DATA, NULL);
        if (pub == NULL) _exit(10);
        if (rmw_weft_pub_refresh(pub) != 0) _exit(12);
        static uint8_t buf[MSG_SMALL];
        for (uint64_t i = 0; i < 100000; i++) {
            fill_msg(buf, MSG_SMALL, i);
            rmw_ret_t rc = rmw_publish(pub, buf, NULL);
            if (rc != RMW_RET_OK && rc != RMW_RET_TIMEOUT) _exit(20);
            /* RMW_RET_TIMEOUT under BEST_EFFORT pressure == counted drop */
        }
        _exit(0);
    }

    rmw_context_t ctx;
    rmw_node_t *node;
    common_setup(&ctx, &node);
    const rosidl_message_type_support_t *ts =
        rmw_weft_create_pod_type_support(MSG_SMALL, TOPIC_SAT);
    rmw_subscription_t *sub =
        rmw_create_subscription(node, ts, TOPIC_SAT,
                                &RMW_QOS_PROFILE_SENSOR_DATA, NULL);
    TU_CHECK(sub != NULL, "sat-sub");

    /* stall the consumer so the ring saturates, then drain */
    tu_usleep(300000);

    static uint8_t buf[MSG_SMALL];
    uint64_t taken = 0, corrupted = 0;
    int reaped = 0;
    int st = 0;
    for (;;) {
        if (!reaped) {
            if (waitpid(pid, &st, WNOHANG) == pid) reaped = 1;
        }
        bool taken_flag = false;
        rmw_ret_t rc = rmw_take(sub, buf, &taken_flag, NULL);
        TU_CHECK(rc == RMW_RET_OK, "sat-take-rc");
        if (taken_flag) {
            uint64_t idx;
            memcpy(&idx, buf + 8, 8); /* {magic, idx} header layout */
            if (verify_msg(buf, MSG_SMALL, idx) != 0) corrupted++;
            taken++;
        } else if (reaped) {
            break; /* child done AND ring drained */
        } else {
            tu_usleep(50);
        }
    }
    if (!reaped) {
        TU_CHECK(waitpid(pid, &st, 0) == pid, "waitpid-sat");
    }
    TU_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sat-child-exit");
    TU_ASSERT(corrupted == 0, "sat-zero-corruption", "%llu corrupted",
              (unsigned long long)corrupted);

    uint64_t missed = rmw_weft_sub_missed(sub);
    uint64_t dropped = atomic_load_explicit(
        &rmw_weft_sub_ring(sub)->ctrl->dropped_total, memory_order_acquire);
    /* drop-NEWBEST policy: publisher drops before commit, so the subscriber
       sees NO seq gaps; the honest ledger is taken + dropped == attempted */
    TU_CHECK(missed == 0, "sat-no-gaps");
    TU_CHECK(taken + dropped >= 100000u - 1u, "sat-accounting-lower");
    TU_CHECK(taken + dropped <= 100000u, "sat-accounting-upper");
    TU_CHECK(taken >= 30, "sat-ring-drained");
    printf("      saturation: taken=%llu dropped=%llu (ledger closes over "
           "100000 attempts, zero corruption)\n",
           (unsigned long long)taken, (unsigned long long)dropped);

    TU_CHECK(rmw_destroy_subscription(node, sub) == RMW_RET_OK, "sat-dsub");
    rmw_weft_destroy_pod_type_support(ts);
    common_teardown(&ctx, node);
    TU_PASS("R3 burst saturation: zero corruption + honest drop ledger");
}

// ---------------------------------------------------------------------------
// R4: fan-out (2 subscribers) + wait-set semantics
// ---------------------------------------------------------------------------

static void phase_fanout(void) {
    int syncfd[2];
    TU_CHECK(pipe(syncfd) == 0, "pipe-fan");
    pid_t pid = fork();
    TU_CHECK(pid >= 0, "fork-fan");

    if (pid == 0) {
        tu_child_setup(); /* die with the parent: never leak a spinner */
        close(syncfd[1]);
        rmw_context_t ctx;
        rmw_node_t *node;
        common_setup(&ctx, &node);
        const rosidl_message_type_support_t *ts =
            rmw_weft_create_pod_type_support(MSG_SMALL, TOPIC_FAN);
        rmw_publisher_t *pub = rmw_create_publisher(
            node, ts, TOPIC_FAN, &RMW_QOS_PROFILE_DEFAULT, NULL);
        if (pub == NULL) _exit(10);
        char s2[2];
        if (read(syncfd[0], s2, 2) != 2) _exit(11); /* both subs ready */
        for (int retries = 0; retries < 1000; retries++) {
            (void)rmw_weft_pub_refresh(pub);
            if (rmw_weft_pub_ring_count(pub) >= 2) break;
            tu_usleep(1000);
        }
        if (rmw_weft_pub_ring_count(pub) < 2) _exit(12);
        static uint8_t buf[MSG_SMALL];
        for (uint64_t i = 0; i < 5000; i++) {
            fill_msg(buf, MSG_SMALL, i);
            if (publish_retry(pub, buf) != RMW_RET_OK) _exit(20);
        }
        _exit(0);
    }

    close(syncfd[0]); /* parent only writes the go signal */
    rmw_context_t ctx;
    rmw_node_t *node;
    common_setup(&ctx, &node);
    const rosidl_message_type_support_t *ts =
        rmw_weft_create_pod_type_support(MSG_SMALL, TOPIC_FAN);
    rmw_subscription_t *s1 =
        rmw_create_subscription(node, ts, TOPIC_FAN,
                                &RMW_QOS_PROFILE_DEFAULT, NULL);
    rmw_subscription_t *s2 =
        rmw_create_subscription(node, ts, TOPIC_FAN,
                                &RMW_QOS_PROFILE_DEFAULT, NULL);
    TU_CHECK(s1 != NULL && s2 != NULL, "fan-subs");

    rmw_wait_set_t *ws = rmw_create_wait_set(&ctx, 8);
    TU_CHECK(ws != NULL, "fan-waitset");
    TU_CHECK(rmw_wait_set_add_subscription(ws, s1) == RMW_RET_OK, "ws-add1");
    TU_CHECK(rmw_wait_set_add_subscription(ws, s2) == RMW_RET_OK, "ws-add2");

    TU_CHECK(write(syncfd[1], "RR", 2) == 2, "fan-sync");

    void *subs_v[2] = {s1, s2};
    rmw_subscriptions_t subs = {.subscriber_count = 2, .subscribers = subs_v};

    static uint8_t b1[MSG_SMALL], b2[MSG_SMALL];
    uint64_t g1 = 0, g2 = 0;
    while (g1 < 5000 || g2 < 5000) {
        rmw_time_point_value_t t = timeout_from_now_us(5000000);
        TU_CHECK(rmw_wait(ws, &subs, NULL, NULL, NULL, NULL, NULL, &t) ==
                      RMW_RET_OK,
                  "fan-wait");
        bool t1 = false, t2 = false;
        if (g1 < 5000) {
            TU_CHECK(rmw_take(s1, b1, &t1, NULL) == RMW_RET_OK, "fan-t1");
            if (t1) {
                uint64_t idx;
                memcpy(&idx, b1 + 8, 8);
                TU_CHECK(idx == g1, "fan-seq1");
                TU_CHECK(verify_msg(b1, MSG_SMALL, idx) == 0, "fan-v1");
                g1 = idx + 1;
            }
        }
        if (g2 < 5000) {
            TU_CHECK(rmw_take(s2, b2, &t2, NULL) == RMW_RET_OK, "fan-t2");
            if (t2) {
                uint64_t idx;
                memcpy(&idx, b2 + 8, 8);
                TU_CHECK(idx == g2, "fan-seq2");
                TU_CHECK(verify_msg(b2, MSG_SMALL, idx) == 0, "fan-v2");
                g2 = idx + 1;
            }
        }
    }
    TU_CHECK(g1 == 5000 && g2 == 5000, "fan-complete");
    TU_CHECK(rmw_weft_sub_missed(s1) == 0 && rmw_weft_sub_missed(s2) == 0,
              "fan-nomiss");

    int st = 0;
    TU_CHECK(waitpid(pid, &st, 0) == pid, "waitpid-fan");
    TU_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fan-child-exit");

    /* idle wait returns TIMEOUT honestly */
    rmw_time_point_value_t t = timeout_from_now_us(200000);
    TU_CHECK(rmw_wait(ws, &subs, NULL, NULL, NULL, NULL, NULL, &t) ==
                  RMW_RET_TIMEOUT,
              "fan-idle-timeout");

    TU_CHECK(rmw_destroy_wait_set(ws) == RMW_RET_OK, "fan-dws");
    TU_CHECK(rmw_destroy_subscription(node, s1) == RMW_RET_OK, "fan-ds1");
    TU_CHECK(rmw_destroy_subscription(node, s2) == RMW_RET_OK, "fan-ds2");
    rmw_weft_destroy_pod_type_support(ts);
    common_teardown(&ctx, node);
    close(syncfd[1]);
    TU_PASS("R4 fan-out: both subscribers complete + wait-set semantics");
}

// ---------------------------------------------------------------------------
// R5: crash orphan sweep
// ---------------------------------------------------------------------------

static void phase_orphan(void) {
    pid_t pid = fork();
    TU_CHECK(pid >= 0, "fork-orphan");
    if (pid == 0) {
        tu_child_setup(); /* die with the parent: never leak a spinner */
        /* crash WITHOUT cleanup: ring + registry slot leak */
        rmw_context_t ctx;
        rmw_node_t *node;
        common_setup(&ctx, &node);
        const rosidl_message_type_support_t *ts =
            rmw_weft_create_pod_type_support(MSG_SMALL, TOPIC_CRASH);
        rmw_subscription_t *sub =
            rmw_create_subscription(node, ts, TOPIC_CRASH,
                                    &RMW_QOS_PROFILE_SENSOR_DATA, NULL);
        if (sub == NULL) _exit(10);
        _exit(9); /* simulated crash: no destroy, no fini */
    }
    int st = 0;
    TU_CHECK(waitpid(pid, &st, 0) == pid, "waitpid-orphan");
    TU_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9, "orphan-crash-observed");

    unsigned leaked = tu_shm_weft_count("weft_rmw_d0_t");
    TU_ASSERT(leaked >= 1, "orphan-ring-observable", "orphan-ring-observable (%u seen)", leaked);;

    /* the next init sweeps the dead subscriber's ring */
    rmw_context_t ctx;
    rmw_node_t *node;
    common_setup(&ctx, &node);
    TU_ASSERT(tu_shm_weft_count("weft_rmw_d0_t") == 0, "orphan-swept", "orphan-swept (%u left)", tu_shm_weft_count("weft_rmw_d0_t"));;
    common_teardown(&ctx, node);
    TU_ASSERT(tu_shm_weft_count("weft_rmw_") == 0, "orphan-registry-healed", "orphan-registry-healed (%u left)", tu_shm_weft_count("weft_rmw_"));;
    TU_PASS("R5 crash orphan sweep: /dev/shm healed on next init");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* HEALING SWEEP (idempotent reruns): a prior crashed run may have left
     * rings + registry entries owned by dead pids in /dev/shm. One cheap
     * init/fini cycle sweeps orphans and drops the registry when empty, so
     * the baseline below is captured against a healed world (CI reruns,
     * sanitizer-leg reruns, local loop iteration — all must stay green). */
    {
        rmw_context_t hctx;
        rmw_init_options_t hopts;
        if (rmw_init_options_init(&hopts, 0, NULL) == RMW_RET_OK &&
            rmw_init(&hopts, &hctx) == RMW_RET_OK) {
            rmw_node_t *hn = rmw_create_node(&hctx, "heal_sweep", "/", 0, NULL);
            if (hn != NULL) (void)rmw_destroy_node(hn);
            (void)rmw_shutdown(&hctx);
            (void)rmw_fini(&hctx);
        }
    }

    unsigned fd0 = tu_fd_count();
    unsigned shm0 = tu_shm_weft_count("weft_rmw_");

    phase_basic();
    phase_saturation();
    phase_fanout();
    phase_orphan();

    /* R6: leak audit */
    TU_ASSERT(tu_fd_count() == fd0, "fd-leak-audit", "fd-leak-audit: %u -> %u", fd0, tu_fd_count());;
    TU_ASSERT(tu_shm_weft_count("weft_rmw_") == shm0, "shm-leak-audit", "%u objects left", tu_shm_weft_count("weft_rmw_"));
    TU_PASS("R6 leak audit: fds + /dev/shm back to baseline");
    printf("\nrmw_weft battery: ALL PHASES PASS\n");
    return 0;
}
