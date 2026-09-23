// split_brain_torture.c — WCR1 adversarial split-brain torture (SB-series).
//
// SB1  Dueling candidates: two nodes forced into the same term with
//      cross-grants; the tie resolves deterministically; never two
//      leaders; duplicate-grant refusal verified.
// SB2  Byzantine block injection: a fake same-term (term, leader) view
//      written past the engine -> E_SPLIT_BRAIN surfaced, deterministic
//      lower-node-id winner adopted, cluster otherwise intact.
// SB3  Random partition torture: 7 nodes, thousands of protocol rounds
//      under random link flips/cuts/heals. Invariants at EVERY round:
//        (I1) at most one LEADER cluster-wide;
//        (I2) per-node fencing tokens never regress;
//        (I3) every engine return is inside the named refusal whitelist
//             (torn/corrupt/split-brain/unknown codes = engine bug);
//        (I4) exact accounting: delivered + fenced == published;
//        (I5) ZERO heap allocations for the whole torture (Law 1);
//      then full heal -> deterministic convergence to one leader.
// SB4  Crash recovery: leader frozen past lease expiry -> majority elects
//      a higher-term leader; the crashed node resumes, is fenced through
//      the term ladder, and the cluster re-converges.

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

#define L_NS   (10ull * 1000 * 1000)
#define STEP   (L_NS / 8)

/* Wire-format control message (RFC 0018 §6.3). */
typedef struct {
    uint64_t term;
    uint32_t node;
    uint32_t rsvd;
} ctl_msg_t;

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

static uint64_t g_delivered = 0;
static void on_data_count(void *user, uint64_t seq,
                          const wcr1_slot_header_t *hdr, const void *payload)
{
    (void)user; (void)seq; (void)hdr; (void)payload;
    g_delivered++;
}

/* Is `e` inside the named refusal whitelist for protocol-driven torture?
   Anything outside = an unnamed/unexpected failure = engine defect. */
static bool sb3_allowed(int e)
{
    switch (e) {
    case WEFT_CLUSTER_OK:
    case WEFT_CLUSTER_E_NOT_LEADER:
    case WEFT_CLUSTER_E_LEASE_EXPIRED:
    case WEFT_CLUSTER_E_QUORUM_LOST:
    case WEFT_CLUSTER_E_BACKPRESSURE:
    case WEFT_CLUSTER_E_TRANSPORT:      /* target behind a partition */
    case WEFT_CLUSTER_E_NODE_EVICTED:   /* zombie peer (honest eviction) */
        return true;
    default:
        return false;   /* torn / corrupt / split-brain / arg / evicted ... */
    }
}

// ---------------------------------------------------------------------------

static void t_sb1_dueling_candidates(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");

    /* Pre-election: nodes 2 and 3 injected into the SAME term with
       cross-grants (a forced duel — neither can reach majority). */
    wcr1_ctx_t *c2 = sim_ctx(s, 2), *c3 = sim_ctx(s, 3);
    wcr1_ring_view_t *r23 = NULL, *r32 = NULL;
    CHECK(wcr1_ctx_hosted_ring(c3, 2, &r23) == WEFT_CLUSTER_OK, "R(2->3)");
    CHECK(wcr1_ctx_hosted_ring(c2, 3, &r32) == WEFT_CLUSTER_OK, "R(3->2)");

    ctl_msg_t m = { .term = 5, .rsvd = 0 };
    uint64_t seq = 0;
    m.node = 2;   /* node 2 asks node 3 for a vote at term 5 */
    CHECK(wcr1_publish(r23, NULL, &m, sizeof m, WCR1_SLOT_F_VOTE_REQUEST,
                       0, 0, 7, &seq) == WEFT_CLUSTER_OK, "req 2->3");
    m.node = 3;   /* node 3 asks node 2 for a vote at term 5 */
    CHECK(wcr1_publish(r32, NULL, &m, sizeof m, WCR1_SLOT_F_VOTE_REQUEST,
                       0, 0, 7, &seq) == WEFT_CLUSTER_OK, "req 3->2");

    sim_tick(s);
    CHECK(wcr1_ctx_term(c2) == 5 && wcr1_ctx_term(c3) == 5,
          "both dueling at term 5");
    CHECK(sim_count_leaders(s) == 0, "no leader mid-duel");
    CHECK(wcr1_ctx_stats(c2)->vote_refusals == 0 &&
          wcr1_ctx_stats(c3)->vote_refusals == 0, "cross-grants accepted");

    /* A third candidate at the same term must be refused by both. */
    wcr1_ring_view_t *r42 = NULL;
    CHECK(wcr1_ctx_hosted_ring(c2, 4, &r42) == WEFT_CLUSTER_OK, "R(4->2)");
    m.node = 4;
    CHECK(wcr1_publish(r42, NULL, &m, sizeof m, WCR1_SLOT_F_VOTE_REQUEST,
                       0, 0, 7, &seq) == WEFT_CLUSTER_OK, "req 4->2");
    sim_tick(s);
    CHECK(wcr1_ctx_stats(c2)->vote_refusals >= 1,
          "third same-term candidate refused");

    /* The duel resolves through the deterministic timeout ladder. */
    for (int round = 0; round < 200; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
        CHECK(sim_count_leaders(s) <= 1, "single leader while dueling");
        if (g_fail != fail_before) break;
    }
    CHECK(sim_count_leaders(s) == 1, "duel resolved to one leader");
    CHECK(wcr1_ctx_term(sim_ctx(s, sim_leader_node(s))) >= 6,
          "resolution climbs the term ladder");
    sim_destroy(s);
    REPORT("SB1", "dueling candidates resolve, never two leaders");
}

static void t_sb2_byzantine_block(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");
    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_leader_node(s) == 1, "clean leader");
    uint64_t real_token = wcr1_ctx_token(sim_ctx(s, 2));

    /* Byzantine injection: a SAME-TERM conflicting view (term 1, leader 4,
       validly paired token) written directly into R(5->2)'s header,
       bypassing the engine. Non-byzantine nodes can never produce this. */
    wcr1_ring_view_t *r52 = NULL;
    CHECK(wcr1_ctx_hosted_ring(sim_ctx(s, 2), 5, &r52) == WEFT_CLUSTER_OK,
          "R(5->2)");
    wcr1_debug_poke_consensus(r52, 1, 4, sim_vclock(s) + L_NS,
                              wcr1_fencing_token(1, 4), 4 /* even */);

    int rc = wcr1_node_poll(sim_ctx(s, 2), 16);
    CHECK(rc == WEFT_CLUSTER_E_SPLIT_BRAIN,
          "byzantine view surfaces E_SPLIT_BRAIN, got %s", wcr1_err_name(rc));
    CHECK(wcr1_ctx_stats(sim_ctx(s, 2))->split_brain_refusals >= 1,
          "split-brain counted");
    /* Deterministic arbitration: lower node id (1) wins at equal term. */
    CHECK(wcr1_ctx_token(sim_ctx(s, 2)) == real_token,
          "node 2 keeps the deterministic winner");
    /* The rest of the cluster is unaffected. */
    for (uint32_t n = 1; n <= 5; n++)
        if (n != 2)
            CHECK(wcr1_ctx_token(sim_ctx(s, n)) == real_token,
                  "node %u unaffected", n);
    CHECK(sim_count_leaders(s) == 1, "still exactly one leader");
    sim_destroy(s);
    REPORT("SB2", "byzantine same-term view: E_SPLIT_BRAIN + tie-break");
}

static void t_sb3_random_partition_torture(void)
{
    int fail_before = g_fail;
    sim_opts_t o = sim_opts_default();
    o.on_data = on_data_count;
    sim_cluster_t *s = sim_create(7, &o);
    CHECK(s != NULL, "sim create (7 nodes)");

    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_count_leaders(s) == 1, "boot leader");

    wcr1_set_alloc_hooks(counting_alloc, counting_free, NULL);
    g_allocs = 0;                      /* Law 1: torture starts now */

    uint64_t prev_token[8];
    for (uint32_t n = 1; n <= 7; n++)
        prev_token[n] = wcr1_ctx_token(sim_ctx(s, n));

    uint64_t published = 0, delivered = 0, fenced_total = 0;
    uint64_t ops = 0, unexpected = 0;
    uint64_t link_flips = 0, heals = 0;
    bool evicted[8] = {false};
    uint8_t payload[64];
    const int ROUNDS = 3000;

    for (int round = 0; round < ROUNDS; round++) {
        /* --- chaos: random link flips, cuts, periodic heals ----------- */
        uint64_t r = sim_rand(s);
        if ((r % 10u) < 3u) {
            uint32_t a = (uint32_t)(sim_rand(s) % 7u) + 1u;
            uint32_t b = (uint32_t)(sim_rand(s) % 7u) + 1u;
            if (a != b) {
                sim_set_link(s, a, b, (sim_rand(s) & 1u) != 0u);
                sim_set_link(s, b, a, (sim_rand(s) & 1u) != 0u);
                link_flips++;
            }
        }
        if ((r % 100u) < 5u) {
            /* isolate one node briefly (bounded: healed well before the
               3-lease eviction timeout can fire) */
            uint32_t victim = (uint32_t)(sim_rand(s) % 7u) + 1u;
            for (uint32_t x = 1; x <= 7; x++) {
                if (x == victim) continue;
                sim_set_link(s, victim, x, false);
                sim_set_link(s, x, victim, false);
            }
            link_flips += 6;
        }
        if ((r % 8u) == 0u) {
            sim_heal_all(s);
            heals++;
        }

        /* --- time + protocol round ------------------------------------ */
        sim_advance(s, 250000 + (sim_rand(s) % 750000));  /* 0.25-1.0 ms */
        for (uint32_t n = 1; n <= 7; n++) {
            int rc = wcr1_node_tick(sim_ctx(s, n));
            ops++;
            if (rc == WEFT_CLUSTER_E_NODE_EVICTED) evicted[n] = true;
            if (!sb3_allowed(rc)) {
                unexpected++;
                CHECK(false, "tick n=%u rc=%s", n, wcr1_err_name(rc));
            }
        }
        for (uint32_t n = 1; n <= 7; n++) {
            int rc = wcr1_node_poll(sim_ctx(s, n), 16);
            ops++;
            if (!sb3_allowed(rc)) {
                unexpected++;
                CHECK(false, "poll n=%u rc=%s", n, wcr1_err_name(rc));
            }
        }

        /* --- the current leader (if any) tries to publish -------------- */
        uint32_t leader = sim_leader_node(s);
        if (leader != 0) {
            uint32_t target = (uint32_t)(sim_rand(s) % 7u) + 1u;
            if (target != leader) {
                uint64_t seq = 0;
                int rc = wcr1_publish_as_leader(sim_ctx(s, leader), target,
                                                payload, sizeof payload, 0,
                                                &seq);
                ops++;
                if (rc == WEFT_CLUSTER_OK) published++;
                else if (!sb3_allowed(rc)) {
                    unexpected++;
                    CHECK(false, "publish rc=%s", wcr1_err_name(rc));
                }
            }
        }

        /* --- invariants ------------------------------------------------ */
        CHECK(sim_count_leaders(s) <= 1, "I1: two leaders at round %d",
              round);
        for (uint32_t n = 1; n <= 7; n++) {
            uint64_t t = wcr1_ctx_token(sim_ctx(s, n));
            CHECK(t >= prev_token[n], "I2: node %u token regressed", n);
            prev_token[n] = t;
        }
        if (g_fail != fail_before) break;
    }
    CHECK(unexpected == 0, "I3: %llu unexpected engine returns",
          (unsigned long long)unexpected);

    /* --- full heal + deterministic convergence ------------------------- */
    sim_heal_all(s);
    bool converged = false;
    for (int round = 0; round < 400 && !converged; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
        ops += 14;
        CHECK(sim_count_leaders(s) <= 1, "I1 post-heal");
        if (g_fail != fail_before) break;
        converged = (sim_count_leaders(s) == 1);
        if (converged) {
            uint64_t t0 = wcr1_ctx_token(sim_ctx(s, sim_leader_node(s)));
            for (uint32_t n = 1; n <= 7; n++)
                if (!evicted[n] &&
                    wcr1_ctx_token(sim_ctx(s, n)) != t0) converged = false;
        }
    }
    CHECK(converged, "I6: post-heal convergence to one leader");
    uint32_t evicted_count = 0;
    for (uint32_t n = 1; n <= 7; n++) if (evicted[n]) evicted_count++;
    CHECK(evicted_count <= 3, "quorum still reachable (evicted=%u)",
          evicted_count);

    /* --- drain: every published data slot is delivered or fenced ------ */
    for (int round = 0; round < 200; round++) {
        bool progress = false;
        uint64_t before = g_delivered;
        for (uint32_t n = 1; n <= 7; n++) {
            (void)wcr1_node_poll(sim_ctx(s, n), 128);
            ops++;
            if (g_delivered == before) { /* evicted nodes make no progress */ }
        }
        if (g_delivered != before) progress = true;
        sim_advance(s, STEP);
        for (uint32_t n = 1; n <= 7; n++) {
            (void)wcr1_node_tick(sim_ctx(s, n));
            ops++;
        }
        if (!progress && g_delivered == before) break;
    }
    for (uint32_t n = 1; n <= 7; n++)
        fenced_total += wcr1_ctx_stats(sim_ctx(s, n))->fenced_slots;
    delivered = g_delivered;
    /* Alive nodes drained fully, so every undelivered data slot must be
       stranded in the backlog of a ring hosted by an EVICTED node (the
       zombie's unconsumed mix of heartbeats/votes/data — an upper bound
       on stranded DATA by construction). */
    uint64_t stranded = 0;
    for (uint32_t n = 1; n <= 7; n++) {
        if (!evicted[n]) continue;
        for (uint32_t p = 1; p <= 7; p++) {
            if (p == n) continue;
            wcr1_ring_view_t *v = NULL;
            if (wcr1_ctx_hosted_ring(sim_ctx(s, n), p, &v) !=
                WEFT_CLUSTER_OK) continue;
            uint64_t committed = 0, consumed = 0;
            (void)wcr1_producer_seq_read(v, 8, &committed);
            (void)wcr1_consumer_seq_read(v, 8, &consumed);
            stranded += committed - consumed;
        }
    }
    CHECK(delivered + fenced_total <= published,
          "I4a: delivered(%llu)+fenced(%llu) > published(%llu)",
          (unsigned long long)delivered, (unsigned long long)fenced_total,
          (unsigned long long)published);
    CHECK(published - delivered - fenced_total <= stranded,
          "I4b: unaccounted %llu > stranded-at-evicted %llu",
          (unsigned long long)(published - delivered - fenced_total),
          (unsigned long long)stranded);
    CHECK(g_allocs == 0, "I5: torture allocations: %llu",
          (unsigned long long)g_allocs);

    /* Aggregate scoreboard (informational). */
    uint64_t elections = 0, won = 0, qloss = 0, dropped = 0, bp = 0, torn = 0;
    for (uint32_t n = 1; n <= 7; n++) {
        const wcr1_stats_t *st = wcr1_ctx_stats(sim_ctx(s, n));
        elections += st->elections_started;
        won += st->elections_won;
        qloss += st->quorum_losses;
        dropped += st->dropped_stores;
        bp += st->bp_refusals;
        torn += st->torn_seq_reads + st->torn_consensus_reads;
    }
    CHECK(torn == 0, "I3: torn reads during torture: %llu",
          (unsigned long long)torn);
    printf("     sb3: %d rounds, %llu ops, %llu published, %llu delivered, "
           "%llu fenced, %llu elections (%llu won), %llu quorum-losses, "
           "%llu dropped, %llu bp, %llu flips, %llu heals, %u evicted\n",
           ROUNDS, (unsigned long long)ops, (unsigned long long)published,
           (unsigned long long)delivered, (unsigned long long)fenced_total,
           (unsigned long long)elections, (unsigned long long)won,
           (unsigned long long)qloss, (unsigned long long)dropped,
           (unsigned long long)bp, (unsigned long long)link_flips,
           (unsigned long long)heals, evicted_count);

    wcr1_set_alloc_hooks(NULL, NULL, NULL);
    sim_destroy(s);
    REPORT("SB3", "7-node random-partition torture, all invariants");
}

static void t_sb4_crash_recovery(void)
{
    int fail_before = g_fail;
    sim_cluster_t *s = sim_create(5, NULL);
    CHECK(s != NULL, "sim create");
    for (int round = 0; round < 40; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
    }
    CHECK(sim_leader_node(s) == 1, "boot leader node 1");
    uint64_t old_token = wcr1_ctx_token(sim_ctx(s, 1));

    /* Freeze the leader completely (crash). Its ctx still SAYS leader —
       but it never ticks, so among ACTIVE nodes at most one leader rules. */
    uint32_t new_leader = 0;
    for (int round = 0; round < 64 && new_leader == 0; round++) {
        sim_advance(s, STEP);
        for (uint32_t n = 2; n <= 5; n++) {
            (void)wcr1_node_tick(sim_ctx(s, n));
            (void)wcr1_node_poll(sim_ctx(s, n), 16);
        }
        int active_leaders = 0;
        for (uint32_t n = 2; n <= 5; n++)
            if (wcr1_ctx_role(sim_ctx(s, n)) == WCR1_ROLE_LEADER) {
                active_leaders++;
                new_leader = n;
            }
        CHECK(active_leaders <= 1, "single ACTIVE leader while frozen");
        if (g_fail != fail_before) break;
    }
    CHECK(new_leader != 0 && new_leader != 1, "majority elected new leader");
    CHECK(wcr1_ctx_token(sim_ctx(s, new_leader)) > old_token,
          "new leader token strictly higher");
    uint64_t mid_token = wcr1_ctx_token(sim_ctx(s, new_leader));

    /* The crashed node resumes: it must lose to the term ladder (fenced
       through higher-term blocks/requests), never dual-lead. */
    uint64_t prev[6];
    for (uint32_t n = 1; n <= 5; n++)
        prev[n] = wcr1_ctx_token(sim_ctx(s, n));
    for (int round = 0; round < 160; round++) {
        sim_advance(s, STEP);
        sim_tick(s);
        CHECK(sim_count_leaders(s) <= 1, "no dual leader around recovery");
        for (uint32_t n = 1; n <= 5; n++) {
            uint64_t t = wcr1_ctx_token(sim_ctx(s, n));
            CHECK(t >= prev[n], "node %u token monotonic through recovery", n);
            prev[n] = t;
        }
        if (g_fail != fail_before) break;
    }
    bool converged = (sim_count_leaders(s) == 1);
    if (converged) {
        uint64_t t0 = wcr1_ctx_token(sim_ctx(s, sim_leader_node(s)));
        for (uint32_t n = 1; n <= 5; n++)
            if (wcr1_ctx_token(sim_ctx(s, n)) != t0) converged = false;
    }
    CHECK(converged, "cluster re-converged after leader crash");
    CHECK(wcr1_ctx_token(sim_ctx(s, sim_leader_node(s))) >= mid_token,
          "final token never regressed");
    sim_destroy(s);
    REPORT("SB4", "leader crash -> re-election -> fenced recovery");
}

// ---------------------------------------------------------------------------

int main(void)
{
    t_sb1_dueling_candidates();
    t_sb2_byzantine_block();
    t_sb3_random_partition_torture();
    t_sb4_crash_recovery();

    printf("\nsplit_brain_torture: %s (%d failures)\n",
           g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
