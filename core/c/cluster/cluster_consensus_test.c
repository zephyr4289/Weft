// cluster_consensus_test.c — WCR1 ring-embedded consensus conformance
// (C-series), driven through the sim_cluster harness (virtual clock,
// deterministic elections, partition link matrix).
//
// C1  Cold-boot election: 5 nodes, exactly one leader, all converged.
// C2  Lease stability: leadership held across many lease periods; follower
//     lease views stay fresh (the same-token lease-refresh path).
// C3  Quorum loss: minority leader steps down; majority elects a new
//     leader with a strictly higher fencing token.
// C4  Fencing: stale tokens refused (E_FENCED); non-leaders refused
//     (E_NOT_LEADER).
// C5  Partition healing: single-leader invariant holds at EVERY tick;
//     per-node tokens never regress; full convergence post-heal.
// C6  Vote uniqueness: one grant per term per node (wire-format injection).
// C7  Clock skew gate: attach refused beyond WCR1_MAX_CLOCK_SKEW_NS.
// C8  Eviction: frozen peer evicted after 3 leases; its ops return
//     E_NODE_EVICTED; the leader fences it from the data plane.
// C9  Consensus seqlock: stable read, torn (odd hb), torn (token/term
//     pairing violation), null state.
// C10 100k+ leader publishes + polls across a 5-node cluster with ZERO
//     heap allocations on the steady-state path (Law 1 at cluster scale).

#include "weft_cluster.h"
#include "weft_cluster_internal.h"
#include "sim_cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

#define REPORT(id, name)                                                    \
    do {                                                                    \
        if (g_fail == fail_before) printf("%-4s %-52s PASS\n", id, name);   \
        else printf("%-4s %-52s FAIL (%d)\n", id, name, g_fail - fail_before);\
    } while (0)

#define L_NS   (10ull * 1000 * 1000)        /* 10 ms virtual lease */
#define STEP   (L_NS / 8)                   /* 1.25 ms tick         */

/* Counting alloc hooks (Law 1). */
static uint64_t g_allocs = 0;

static void *counting_alloc(size_t size, size_t alignment, void *user)
{
    (void)user;
    g_allocs++;
    void *p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}

static void counting_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}

/* Data-plane delivery counter (C10). */
static uint64_t g_delivered = 0;
static uint64_t g_bad_payload = 0;

static void on_data_count(void *user, uint64_t seq,
                          const wcr1_slot_header_t *hdr, const void *payload)
{
    (void)user; (void)hdr; (void)seq;   /* ring seq interleaves heartbeats */
    g_delivered++;
    /* The payload carries its own message index + pattern; deliveries
       must arrive in strict index order. */
    uint64_t n;
    memcpy(&n, payload, 8);
    if (n != g_delivered) g_bad_payload++;
    for (int k = 8; k < 128; k++)
        if (((const uint8_t *)payload)[k] != (uint8_t)(n + k)) {
            g_bad_payload++;
            break;
        }
}

/* Wire-format control message (matches the engine's in-band layout:
   u64 term LE | u32 node | u32 rsvd — RFC 0018 §6.3). */
typedef struct {
    uint64_t term;
    uint32_t node;
    uint32_t rsvd;
} ctl_msg_t;

// ---------------------------------------------------------------------------

static void t_c1_cold_boot_election(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");

    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_count_leaders(s) == 1, "exactly one leader (%d)",
          sim_count_leaders(s));
    CHECK(sim_leader_node(s) == 1, "deterministic first leader = node 1");
    for (uint32_t n = 1; n <= 5; n++) {
        wcr1_ctx_t *c = sim_ctx(s, n);
        CHECK(wcr1_ctx_role(c) == (n == 1 ? WCR1_ROLE_LEADER
                                          : WCR1_ROLE_FOLLOWER),
              "node %u role", n);
        CHECK(wcr1_ctx_token(c) == wcr1_fencing_token(1, 1),
              "node %u token 0x%llx", n,
              (unsigned long long)wcr1_ctx_token(c));
        CHECK(wcr1_ctx_term(c) == 1, "node %u term", n);
    }
    CHECK(wcr1_ctx_stats(sim_ctx(s, 1))->elections_won == 1,
          "node 1 won exactly one election");
    sim_destroy(s);
    REPORT("C1", "cold-boot election: 1 leader, all converged");
}

static void t_c2_lease_stability(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");

    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_leader_node(s) == 1, "boot leader");

    /* 20 lease periods of steady state. */
    for (int round = 0; round < 160; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
        CHECK(sim_count_leaders(s) == 1, "single leader at every tick");
        CHECK(sim_leader_node(s) == 1, "leadership stable");
        if (g_fail != fail_before) break;
    }
    CHECK(wcr1_ctx_stats(sim_ctx(s, 1))->elections_won == 1,
          "no spurious re-elections");
    CHECK(wcr1_ctx_stats(sim_ctx(s, 1))->lease_refreshes >= 30,
          "lease refreshes flowing (%llu)",
          (unsigned long long)wcr1_ctx_stats(sim_ctx(s, 1))->lease_refreshes);

    /* Follower lease views stay ahead of the clock (fresh same-token
       lease adoption — the path that prevents spurious elections). */
    uint64_t now = sim_vclock(s);
    for (uint32_t n = 2; n <= 5; n++)
        CHECK(wcr1_ctx_lease_expire(sim_ctx(s, n)) > now,
              "node %u lease fresh", n);
    sim_destroy(s);
    REPORT("C2", "lease stability across 20 lease periods");
}

static void t_c3_c4_quorum_fencing(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");
    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    uint64_t old_token = wcr1_ctx_token(sim_ctx(s, 1));
    CHECK(old_token == wcr1_fencing_token(1, 1), "boot token");

    /* Partition: {1,2} | {3,4,5} — leader 1 lands in the minority. */
    uint32_t ga[2] = {1, 2}, gb[3] = {3, 4, 5};
    sim_partition_groups(s, ga, 2, gb, 3);

    bool stepped_down = false;
    for (int round = 0; round < 48; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
        CHECK(sim_count_leaders(s) <= 1, "single-leader invariant");
        if (g_fail != fail_before) break;
        if (wcr1_ctx_role(sim_ctx(s, 1)) != WCR1_ROLE_LEADER) {
            stepped_down = true;
        }
    }
    CHECK(stepped_down, "minority leader stepped down");
    CHECK(wcr1_ctx_stats(sim_ctx(s, 1))->quorum_losses >= 1,
          "quorum loss recorded");
    CHECK(sim_count_leaders(s) == 1, "majority side elected a leader");
    uint32_t new_leader = sim_leader_node(s);
    CHECK(new_leader == 3, "majority leader = node 3 (got %u)", new_leader);
    uint64_t new_token = wcr1_ctx_token(sim_ctx(s, new_leader));
    CHECK(new_token == wcr1_fencing_token(wcr1_ctx_term(sim_ctx(s, 3)), 3),
          "token format (term<<32)|leader");
    CHECK(new_token > old_token, "fencing token strictly increased");

    /* C4: fencing + leadership refusals while partitioned. */
    uint8_t payload[16] = "fenced-payload!";
    uint64_t seq = 0;
    int e = wcr1_publish_as_leader(sim_ctx(s, 1), 2, payload, 16, 0, &seq);
    CHECK(e == WEFT_CLUSTER_E_NOT_LEADER,
          "deposed leader refused, got %s", wcr1_err_name(e));
    e = wcr1_fence_check(sim_ctx(s, new_leader), old_token);
    CHECK(e == WEFT_CLUSTER_E_FENCED, "stale token fenced, got %s",
          wcr1_err_name(e));
    e = wcr1_fence_check(sim_ctx(s, new_leader), new_token);
    CHECK(e == WEFT_CLUSTER_OK, "current token accepted");

    /* C5: heal — invariants at every tick, tokens never regress,
       convergence. */
    uint64_t prev_token[6] = {0};
    for (uint32_t n = 1; n <= 5; n++)
        prev_token[n] = wcr1_ctx_token(sim_ctx(s, n));
    sim_heal_all(s);
    bool converged = false;
    for (int round = 0; round < 200 && !converged; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
        CHECK(sim_count_leaders(s) <= 1, "single leader while healing");
        for (uint32_t n = 1; n <= 5; n++) {
            uint64_t t = wcr1_ctx_token(sim_ctx(s, n));
            CHECK(t >= prev_token[n], "node %u token regressed", n);
            prev_token[n] = t;
        }
        if (g_fail != fail_before) break;
        converged = (sim_count_leaders(s) == 1);
        if (converged) {
            for (uint32_t n = 1; n <= 5; n++)
                if (wcr1_ctx_token(sim_ctx(s, n)) !=
                    wcr1_ctx_token(sim_ctx(s, sim_leader_node(s))))
                    converged = false;
        }
    }
    CHECK(converged, "cluster converged after heal");
    CHECK(wcr1_ctx_term(sim_ctx(s, sim_leader_node(s))) >= 2,
          "post-heal term advanced");
    sim_destroy(s);
    REPORT("C3-5", "quorum loss, fencing, partition heal + convergence");
}

static void t_c6_vote_uniqueness(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");
    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }

    /* Inject two competing VOTE_REQUESTs at term 99, node 4 then node 5,
       both delivered into rings hosted by node 3 (wire-format injection). */
    wcr1_ctx_t *c3 = sim_ctx(s, 3);
    wcr1_ring_view_t *r43 = NULL, *r53 = NULL;
    CHECK(wcr1_ctx_hosted_ring(c3, 4, &r43) == WEFT_CLUSTER_OK, "R(4->3)");
    CHECK(wcr1_ctx_hosted_ring(c3, 5, &r53) == WEFT_CLUSTER_OK, "R(5->3)");

    ctl_msg_t m4 = { .term = 99, .node = 4, .rsvd = 0 };
    ctl_msg_t m5 = { .term = 99, .node = 5, .rsvd = 0 };
    uint64_t seq = 0;
    CHECK(wcr1_publish(r43, NULL, &m4, sizeof m4, WCR1_SLOT_F_VOTE_REQUEST,
                       0, 0, 7, &seq) == WEFT_CLUSTER_OK, "request from 4");
    CHECK(wcr1_publish(r53, NULL, &m5, sizeof m5, WCR1_SLOT_F_VOTE_REQUEST,
                       0, 0, 7, &seq) == WEFT_CLUSTER_OK, "request from 5");

    sim_tick(s);   /* node 3 polls: grants 4, refuses 5 (same term) */

    CHECK(wcr1_ctx_stats(c3)->vote_refusals >= 1,
          "duplicate-term request refused");
    CHECK(wcr1_ctx_term(c3) == 99, "term adopted from first request");
    sim_destroy(s);
    REPORT("C6", "vote uniqueness (one grant per term per node)");
}

static void t_c7_clock_skew(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    uint8_t cid[16] = "C7-SKEW-CLUSTER!";
    memcpy(cfg.cluster_id, cid, 16);
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = L_NS;

    cfg.node_id = 1;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 2;
    wcr1_ctx_t *a = NULL;
    CHECK(wcr1_ctx_create(&cfg, &a) == WEFT_CLUSTER_OK, "ctx A (node 1)");

    cfg.node_id = 2;
    cfg.peer_ids[0] = 1;
    wcr1_ctx_t *b = NULL;
    CHECK(wcr1_ctx_create(&cfg, &b) == WEFT_CLUSTER_OK, "ctx B (node 2)");

    /* R(1->2): hosted by node 2, produced by node 1. */
    wcr1_ring_view_t *r12 = NULL;
    CHECK(wcr1_ctx_hosted_ring(b, 1, &r12) == WEFT_CLUSTER_OK, "R(1->2)");
    uint64_t rlen = wcr1_region_size(64, 192);

    /* Bounded skew attaches; beyond the bound is refused. */
    CHECK(wcr1_ctx_register_peer(a, 2, r12->hdr, rlen, 100000) ==
          WEFT_CLUSTER_OK, "100us skew accepted");
    CHECK(wcr1_ctx_register_peer(a, 2, r12->hdr, rlen, 100000) ==
          WEFT_CLUSTER_E_NODE_ID, "duplicate registration refused");

    wcr1_ring_view_t *r21 = NULL;
    CHECK(wcr1_ctx_hosted_ring(a, 2, &r21) == WEFT_CLUSTER_OK, "R(2->1)");
    CHECK(wcr1_ctx_register_peer(b, 1, r21->hdr, rlen,
                                 (int64_t)WCR1_MAX_CLOCK_SKEW_NS + 1) ==
          WEFT_CLUSTER_E_CLOCK_SKEW, "skew beyond bound refused");
    CHECK(wcr1_ctx_register_peer(b, 1, r21->hdr, rlen,
                                 -(int64_t)(WCR1_MAX_CLOCK_SKEW_NS + 1)) ==
          WEFT_CLUSTER_E_CLOCK_SKEW, "negative skew beyond bound refused");

    wcr1_ctx_destroy(a);
    wcr1_ctx_destroy(b);
    REPORT("C7", "clock skew gate at attach");
}

static void t_c8_eviction(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");
    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_leader_node(s) == 1, "boot leader");

    /* Freeze node 5: the leader's heartbeat slots go unconsumed, its
       watermark freezes, and after 3 leases the leader evicts it. */
    for (int round = 0; round < 20; round++) {
        sim_advance(s, L_NS / 4);
        for (uint32_t n = 1; n <= 4; n++) {
            wcr1_ctx_t *c = sim_ctx(s, n);
            (void)wcr1_node_tick(c);
            (void)wcr1_node_poll(c, 16);
        }
    }
    /* Node 5 wakes up and polls: reads the NODE_EVICTED mark. */
    wcr1_ctx_t *c5 = sim_ctx(s, 5);
    (void)wcr1_node_poll(c5, 16);
    int e = wcr1_node_tick(c5);
    CHECK(e == WEFT_CLUSTER_E_NODE_EVICTED,
          "evicted node refused, got %s", wcr1_err_name(e));

    /* Leader fences the zombie from the data plane. */
    uint8_t payload[8] = "zombie!";
    uint64_t seq = 0;
    e = wcr1_publish_as_leader(sim_ctx(s, 1), 5, payload, 8, 0, &seq);
    CHECK(e == WEFT_CLUSTER_E_NODE_EVICTED,
          "leader refuses evicted peer, got %s", wcr1_err_name(e));

    /* Live peers still served. */
    e = wcr1_publish_as_leader(sim_ctx(s, 1), 2, payload, 8, 0, &seq);
    CHECK(e == WEFT_CLUSTER_OK, "live peer still served, got %s",
          wcr1_err_name(e));
    sim_destroy(s);
    REPORT("C8", "zombie eviction after 3 frozen leases");
}

static void t_c9_consensus_seqlock(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    uint8_t cid[16] = "C9-SEQLOCK-RING!";
    memcpy(cfg.cluster_id, cid, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = L_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    wcr1_consensus_view_t cv;
    /* Null state reads clean. */
    CHECK(wcr1_consensus_read(&v, 8, &cv) == WEFT_CLUSTER_OK, "null read");
    CHECK(cv.term == 0 && cv.leader_node == 0 && cv.fencing_token == 0,
          "null state all zero");

    /* Stable publish + read-back. */
    uint64_t hb = 0;
    uint64_t token = wcr1_fencing_token(5, 2);
    CHECK(wcr1_consensus_block_write(&v, NULL, 5, 2, 12345, token, 0, 0xF,
                                     &hb) == WEFT_CLUSTER_OK, "block write");
    CHECK(hb == 2, "hb advanced by 2");
    CHECK(wcr1_consensus_read(&v, 8, &cv) == WEFT_CLUSTER_OK, "stable read");
    CHECK(cv.term == 5 && cv.leader_node == 2 && cv.fencing_token == token &&
          cv.lease_expire_ns == 12345 && cv.quorum_mask == 0xF,
          "read-back matches");

    /* Odd heartbeat = write-in-progress -> bounded tear. */
    wcr1_debug_poke_consensus(&v, 5, 2, 12345, token, 7 /* odd */);
    CHECK(wcr1_consensus_read(&v, 8, &cv) == WEFT_CLUSTER_E_CONSENSUS_TORN,
          "odd hb torn");
    wcr1_debug_poke_consensus(&v, 5, 2, 12345, token, 8 /* even */);

    /* Pairing violation: token names a different term -> torn. */
    wcr1_debug_poke_consensus(&v, 5, 2, 12345, wcr1_fencing_token(6, 2), 8);
    CHECK(wcr1_consensus_read(&v, 8, &cv) == WEFT_CLUSTER_E_CONSENSUS_TORN,
          "token/term pairing torn");
    /* Pairing violation: token names leader 3, field says 4 -> torn. */
    wcr1_debug_poke_consensus(&v, 5, 4, 12345, wcr1_fencing_token(5, 3), 8);
    CHECK(wcr1_consensus_read(&v, 8, &cv) == WEFT_CLUSTER_E_CONSENSUS_TORN,
          "token/leader pairing torn");

    wcr1_engine_free(region);
    REPORT("C9", "consensus seqlock: stable, torn, pairing guards");
}

static void t_c10_zero_alloc_100k(void)
{
    int fail_before = g_fail;
    sim_opts_t o = sim_opts_default();
    o.capacity_slots = 1024;
    o.on_data = on_data_count;
    sim_cluster_t *s = sim_create(5, &o);
    CHECK(s != NULL, "sim create");

    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_leader_node(s) == 1, "boot leader");

    wcr1_set_alloc_hooks(counting_alloc, counting_free, NULL);
    g_allocs = 0;      /* steady-state starts NOW (Law 1) */

    uint8_t payload[128];
    uint64_t seq = 0, published = 0;
    const uint32_t BATCH = 64, ROUNDS = 1563;   /* 100,032 messages */
    for (uint32_t r = 0; r < ROUNDS; r++) {
        for (uint32_t k = 0; k < BATCH; k++) {
            uint64_t n = published + 1;
            memcpy(payload, &n, 8);
            for (int b = 8; b < 128; b++) payload[b] = (uint8_t)(n + b);
            int e = wcr1_publish_as_leader(sim_ctx(s, 1), 2, payload, 128,
                                           0, &seq);
            CHECK(e == WEFT_CLUSTER_OK, "publish %llu: %s",
                  (unsigned long long)n, wcr1_err_name(e));
            if (e != WEFT_CLUSTER_OK) goto done;
            published++;
        }
        sim_advance(s, 1000);   /* 1 us: leases stay trivially valid */
        for (uint32_t n = 1; n <= 5; n++) {
            (void)wcr1_node_tick(sim_ctx(s, n));
            (void)wcr1_node_poll(sim_ctx(s, n), 128);
        }
    }
done:
    CHECK(published == (uint64_t)BATCH * ROUNDS, "published %llu",
          (unsigned long long)published);
    CHECK(g_delivered == published, "delivered %llu / %llu",
          (unsigned long long)g_delivered, (unsigned long long)published);
    CHECK(g_bad_payload == 0, "payload corruptions: %llu",
          (unsigned long long)g_bad_payload);
    CHECK(g_allocs == 0, "steady-state allocations: %llu",
          (unsigned long long)g_allocs);

    wcr1_set_alloc_hooks(NULL, NULL, NULL);
    sim_destroy(s);
    REPORT("C10", "100k cluster sync ops, zero alloc, intact payloads");
}

// ---------------------------------------------------------------------------

int main(void)
{
    t_c1_cold_boot_election();
    t_c2_lease_stability();
    t_c3_c4_quorum_fencing();
    t_c6_vote_uniqueness();
    t_c7_clock_skew();
    t_c8_eviction();
    t_c9_consensus_seqlock();
    t_c10_zero_alloc_100k();

    printf("\ncluster_consensus_test: %s (%d failures)\n",
           g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
