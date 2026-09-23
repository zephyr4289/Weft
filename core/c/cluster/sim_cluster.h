// sim_cluster.h — multi-node simulated cluster harness for the WCR1
// protocol engine (cluster_consensus_test.c / split_brain_torture.c).
//
// Simulates N nodes in one process:
//   - every node hosts one ring per peer producer and produces into one
//     ring per peer (full mesh), exactly like the production topology;
//   - "one-sided RDMA writes" are direct stores through a GATED transport
//     vtable: the link matrix drops stores across partitioned links (the
//     same seam Engineer 2's ibverbs transport will occupy);
//   - a fully VIRTUAL clock (deterministic elections, no sleeps);
//   - per-node clock offsets for skew gating tests.
//
// Partitions may only change BETWEEN sim_tick() rounds (event granularity
// — matches RC-QP semantics where a posted WR either lands whole or not).

#ifndef SIM_CLUSTER_H
#define SIM_CLUSTER_H

#include "weft_cluster.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_MAX_NODES 8

typedef struct {
    uint64_t lease_ns;        /* default 10 ms (virtual)                  */
    uint64_t capacity_slots;  /* default 64                               */
    uint64_t slot_size;       /* default 192 (64B hdr + 128B payload)     */
    void (*on_data)(void *user, uint64_t seq,
                    const wcr1_slot_header_t *hdr, const void *payload);
    void *on_data_user;
} sim_opts_t;

sim_opts_t sim_opts_default(void);

typedef struct sim_cluster sim_cluster_t;

/* Create the full mesh (N ctx's + N*(N-1) rings, all pre-allocated). */
sim_cluster_t *sim_create(uint32_t n_nodes, const sim_opts_t *opts);
void           sim_destroy(sim_cluster_t *sim);

/* Link gating: link a->b gates a's one-sided writes into b-hosted rings. */
void sim_set_link(sim_cluster_t *sim, uint32_t a_node, uint32_t b_node,
                  bool up);
/* Cut every cross-group link (both directions). Groups are node ids. */
void sim_partition_groups(sim_cluster_t *sim, const uint32_t *ga,
                          uint32_t na, const uint32_t *gb, uint32_t nb);
void sim_heal_all(sim_cluster_t *sim);

/* Virtual clock. */
void     sim_advance(sim_cluster_t *sim, uint64_t dt_ns);
uint64_t sim_vclock(const sim_cluster_t *sim);
void     sim_set_clock_offset(sim_cluster_t *sim, uint32_t node,
                              int64_t offset_ns);

/* One deterministic protocol round: tick(0..n-1) then poll(0..n-1). */
void sim_tick(sim_cluster_t *sim);

/* Introspection / drivers. */
int       sim_count_leaders(const sim_cluster_t *sim);
uint32_t  sim_leader_node(const sim_cluster_t *sim);   /* 0 = none     */
wcr1_ctx_t *sim_ctx(sim_cluster_t *sim, uint32_t node_id);
uint64_t  sim_dropped_stores(const sim_cluster_t *sim);
uint64_t  sim_rand(sim_cluster_t *sim);                /* deterministic */

#ifdef __cplusplus
}
#endif

#endif /* SIM_CLUSTER_H */
