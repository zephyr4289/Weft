// sim_cluster.c — the multi-node simulated cluster harness (see header).
//
// All cross-node writes go through the gated sim transport; all reads are
// local (exactly the production memory model). The virtual clock makes
// elections deterministic; the link matrix partitions at event granularity.

#include "sim_cluster.h"
#include "weft_cluster_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    sim_cluster_t *sim;
    uint32_t       node;      /* node_id (1-based) */
} sim_node_ref_t;

struct sim_cluster {
    uint32_t         n_nodes;
    sim_opts_t       opts;
    uint64_t         vclock_ns;
    int64_t          clock_offset_ns[SIM_MAX_NODES];
    bool             link[SIM_MAX_NODES][SIM_MAX_NODES];
    /* ring[p][h]: produced by node p+1, hosted by node h+1 */
    uint8_t         *ring[SIM_MAX_NODES][SIM_MAX_NODES];
    uint64_t         ring_len[SIM_MAX_NODES][SIM_MAX_NODES];
    wcr1_ctx_t      *ctx[SIM_MAX_NODES];
    wcr1_transport_t transport[SIM_MAX_NODES];
    sim_node_ref_t   nref[SIM_MAX_NODES];
    uint64_t         dropped;
    uint64_t         rng;
};

static const uint8_t SIM_CLUSTER_ID[16] = {
    'P', 'I', 'L', '3', '-', 'S', 'I', 'M', '-', 'C', 'L', 'U', 'S', 'T', 'E', 'R'
};

sim_opts_t sim_opts_default(void)
{
    sim_opts_t o;
    memset(&o, 0, sizeof o);
    o.lease_ns = 10ull * 1000 * 1000;   /* 10 ms virtual */
    o.capacity_slots = 64;
    o.slot_size = 192;
    return o;
}

// ---------------------------------------------------------------------------
// Virtual clock (per-node offsets model bounded skew)
// ---------------------------------------------------------------------------

static uint64_t sim_clock_fn(void *user)
{
    sim_node_ref_t *r = (sim_node_ref_t *)user;
    int64_t now = (int64_t)r->sim->vclock_ns +
                  r->sim->clock_offset_ns[r->node - 1u];
    return (uint64_t)now;
}

// ---------------------------------------------------------------------------
// Gated transport: one-sided writes dropped across partitioned links
// ---------------------------------------------------------------------------

/* Locate the ring containing dest among the rings PRODUCED by `node`
   (every engine-issued remote write targets a ring the writer produces). */
static int sim_locate(sim_cluster_t *s, uint32_t node, const void *dest,
                      uint64_t *len_out)
{
    uint32_t p = node - 1u;
    for (uint32_t h = 0; h < s->n_nodes; h++) {
        if (h == p) continue;
        uint8_t *base = s->ring[p][h];
        if (!base) continue;
        uintptr_t d = (uintptr_t)dest;
        if (d >= (uintptr_t)base &&
            d < (uintptr_t)base + s->ring_len[p][h]) {
            if (len_out) *len_out = s->ring_len[p][h];
            return (int)h;
        }
    }
    return -1;
}

static bool sim_gate(sim_cluster_t *s, uint32_t node, const void *dest)
{
    int h = sim_locate(s, node, dest, NULL);
    if (h < 0) return true;               /* not one of ours: local store */
    if (!s->link[node - 1u][h]) {
        s->dropped++;
        return false;                     /* partitioned: silently dropped */
    }
    return true;
}

static bool sim_store32(wcr1_transport_t *t, volatile uint32_t *dest,
                        uint32_t v)
{
    sim_node_ref_t *r = (sim_node_ref_t *)t->user;
    if (!sim_gate(r->sim, r->node, (const void *)dest)) return false;
    __atomic_store_n(dest, v, __ATOMIC_RELAXED);
    return true;
}

static bool sim_store64(wcr1_transport_t *t, volatile uint64_t *dest,
                        uint64_t v)
{
    sim_node_ref_t *r = (sim_node_ref_t *)t->user;
    if (!sim_gate(r->sim, r->node, (const void *)dest)) return false;
    __atomic_store_n(dest, v, __ATOMIC_RELAXED);
    return true;
}

static bool sim_store_buf(wcr1_transport_t *t, void *dest, const void *src,
                          size_t len)
{
    sim_node_ref_t *r = (sim_node_ref_t *)t->user;
    if (!sim_gate(r->sim, r->node, dest)) return false;
    memcpy(dest, src, len);
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

sim_cluster_t *sim_create(uint32_t n_nodes, const sim_opts_t *opts)
{
    if (n_nodes < 1 || n_nodes > SIM_MAX_NODES) return NULL;
    sim_opts_t o = opts ? *opts : sim_opts_default();
    if (o.lease_ns == 0) o.lease_ns = 10ull * 1000 * 1000;
    if (o.capacity_slots == 0) o.capacity_slots = 64;
    if (o.slot_size == 0) o.slot_size = 192;

    sim_cluster_t *s = (sim_cluster_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->n_nodes = n_nodes;
    s->opts = o;
    s->vclock_ns = 4u * o.lease_ns;       /* room for negative offsets */
    s->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)n_nodes;

    for (uint32_t a = 0; a < n_nodes; a++)
        for (uint32_t b = 0; b < n_nodes; b++)
            s->link[a][b] = (a != b);

    /* Per-node ctx: full-mesh peer set, gated transport, virtual clock. */
    for (uint32_t i = 0; i < n_nodes; i++) {
        s->nref[i].sim = s;
        s->nref[i].node = i + 1u;
        s->transport[i].user = &s->nref[i];
        s->transport[i].store32 = sim_store32;
        s->transport[i].store64 = sim_store64;
        s->transport[i].store_buf = sim_store_buf;

        wcr1_cfg_t cfg;
        memset(&cfg, 0, sizeof cfg);
        memcpy(cfg.cluster_id, SIM_CLUSTER_ID, 16);
        cfg.node_id = i + 1u;
        cfg.cluster_size = n_nodes;
        cfg.epoch = 7u;
        cfg.capacity_slots = o.capacity_slots;
        cfg.slot_size = o.slot_size;
        cfg.lease_ns = o.lease_ns;
        cfg.peer_count = n_nodes - 1u;
        uint32_t k = 0;
        for (uint32_t j = 0; j < n_nodes; j++)
            if (j != i) cfg.peer_ids[k++] = j + 1u;
        cfg.clock_fn = sim_clock_fn;
        cfg.clock_user = &s->nref[i];
        cfg.transport = &s->transport[i];
        cfg.on_data = o.on_data;
        cfg.on_data_user = o.on_data_user;

        if (wcr1_ctx_create(&cfg, &s->ctx[i]) != WEFT_CLUSTER_OK) {
            for (uint32_t j = 0; j < i; j++) wcr1_ctx_destroy(s->ctx[j]);
            free(s);
            return NULL;
        }
    }

    /* Discover every hosted ring R(p -> h) from the hosting ctx. */
    for (uint32_t p = 0; p < n_nodes; p++) {
        for (uint32_t h = 0; h < n_nodes; h++) {
            if (p == h) continue;
            wcr1_ring_view_t *v = NULL;
            if (wcr1_ctx_hosted_ring(s->ctx[h], p + 1u, &v) !=
                WEFT_CLUSTER_OK) {
                sim_destroy(s);
                return NULL;
            }
            s->ring[p][h] = (uint8_t *)v->hdr;
            s->ring_len[p][h] = wcr1_region_size(v->capacity, v->slot_size);
        }
    }

    /* Cross-register producer views (the "attach handshake"). */
    for (uint32_t me = 0; me < n_nodes; me++) {
        for (uint32_t peer = 0; peer < n_nodes; peer++) {
            if (me == peer) continue;
            if (wcr1_ctx_register_peer(s->ctx[me], peer + 1u,
                                       s->ring[me][peer],
                                       s->ring_len[me][peer], 0) !=
                WEFT_CLUSTER_OK) {
                sim_destroy(s);
                return NULL;
            }
        }
    }
    return s;
}

void sim_destroy(sim_cluster_t *sim)
{
    if (!sim) return;
    for (uint32_t i = 0; i < sim->n_nodes; i++)
        wcr1_ctx_destroy(sim->ctx[i]);
    free(sim);
}

// ---------------------------------------------------------------------------
// Partitions / clock
// ---------------------------------------------------------------------------

void sim_set_link(sim_cluster_t *sim, uint32_t a_node, uint32_t b_node,
                  bool up)
{
    if (!sim || a_node == 0 || b_node == 0 ||
        a_node > sim->n_nodes || b_node > sim->n_nodes || a_node == b_node)
        return;
    sim->link[a_node - 1u][b_node - 1u] = up;
}

void sim_partition_groups(sim_cluster_t *sim, const uint32_t *ga,
                          uint32_t na, const uint32_t *gb, uint32_t nb)
{
    if (!sim) return;
    for (uint32_t i = 0; i < na; i++)
        for (uint32_t j = 0; j < nb; j++)
            sim_set_link(sim, ga[i], gb[j], false);
    for (uint32_t i = 0; i < nb; i++)
        for (uint32_t j = 0; j < na; j++)
            sim_set_link(sim, gb[i], ga[j], false);
}

void sim_heal_all(sim_cluster_t *sim)
{
    if (!sim) return;
    for (uint32_t a = 0; a < sim->n_nodes; a++)
        for (uint32_t b = 0; b < sim->n_nodes; b++)
            sim->link[a][b] = (a != b);
}

void sim_advance(sim_cluster_t *sim, uint64_t dt_ns)
{
    if (sim) sim->vclock_ns += dt_ns;
}

uint64_t sim_vclock(const sim_cluster_t *sim)
{
    return sim ? sim->vclock_ns : 0;
}

void sim_set_clock_offset(sim_cluster_t *sim, uint32_t node,
                          int64_t offset_ns)
{
    if (!sim || node == 0 || node > sim->n_nodes) return;
    sim->clock_offset_ns[node - 1u] = offset_ns;
}

// ---------------------------------------------------------------------------
// Deterministic protocol round
// ---------------------------------------------------------------------------

void sim_tick(sim_cluster_t *sim)
{
    if (!sim) return;
    for (uint32_t i = 0; i < sim->n_nodes; i++)
        (void)wcr1_node_tick(sim->ctx[i]);
    for (uint32_t i = 0; i < sim->n_nodes; i++)
        (void)wcr1_node_poll(sim->ctx[i], 16);
}

// ---------------------------------------------------------------------------
// Introspection / drivers
// ---------------------------------------------------------------------------

int sim_count_leaders(const sim_cluster_t *sim)
{
    if (!sim) return -1;
    int count = 0;
    for (uint32_t i = 0; i < sim->n_nodes; i++)
        if (wcr1_ctx_role(sim->ctx[i]) == WCR1_ROLE_LEADER) count++;
    return count;
}

uint32_t sim_leader_node(const sim_cluster_t *sim)
{
    if (!sim) return 0;
    for (uint32_t i = 0; i < sim->n_nodes; i++)
        if (wcr1_ctx_role(sim->ctx[i]) == WCR1_ROLE_LEADER) return i + 1u;
    return 0;
}

wcr1_ctx_t *sim_ctx(sim_cluster_t *sim, uint32_t node_id)
{
    if (!sim || node_id == 0 || node_id > sim->n_nodes) return NULL;
    return sim->ctx[node_id - 1u];
}

uint64_t sim_dropped_stores(const sim_cluster_t *sim)
{
    return sim ? sim->dropped : 0;
}

uint64_t sim_rand(sim_cluster_t *sim)
{
    if (!sim) return 0;
    uint64_t x = sim->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    sim->rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}
