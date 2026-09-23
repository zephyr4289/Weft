// weft_cluster_consensus.c — WCR1 ring-embedded consensus (RFC 0018 §6).
//
// The cluster state machine lives INSIDE the ring headers — no external
// coordination service (no zookeeper/etcd), no extra RPC roundtrips:
//   - Elections ride the data plane: VOTE_REQUEST / VOTE_GRANT slots flow
//     through the same producer->home rings as tensors (in-band control).
//   - The leader replicates its consensus block (term, leader, lease,
//     fencing token, quorum mask) into every ring it produces; followers
//     read it LOCALLY from their hosted rings (zero network reads).
//   - The consensus block is guarded by a heartbeat_seq seqlock: odd =
//     write-in-progress, even = committed; the fencing token embeds
//     (term, leader) so a torn block can never pass validation.
//   - Deterministic split-brain arbitration: (higher term wins; same term
//     -> lower node id wins; conflicting same-term views -> E_SPLIT_BRAIN
//     surfaced + the deterministic winner adopted). Quorum intersection
//     makes two same-term leaders impossible for non-Byzantine nodes.
//
// Law 1: ctx + all ring regions + peer tables allocated at create/attach.
// Law 2: every loop bounded; election timeouts are node-id staggered
//        deterministic functions of the (injectable) clock.
// Law 4: every anomaly returns a unique named code.

#include "weft_cluster.h"
#include "weft_cluster_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WCR1_EVICTION_TIMEOUT_LEASES 3u   /* watermark static for 3 leases */

/* In-band control payload (16 bytes, wire-stable on WCR1-legal hosts). */
typedef struct {
    uint64_t term;    /* the term this control message belongs to */
    uint32_t node;    /* candidate (request) / voter (grant) */
    uint32_t rsvd;
} wcr1_control_msg_t;

struct wcr1_ctx {
    wcr1_cfg_t       cfg;
    uint32_t         node_id;
    int              role;
    bool             self_evicted;

    /* --- election / leadership state (node-local) --- */
    uint64_t         local_term;      /* highest term seen or started     */
    uint64_t         voted_for;       /* node voted for in local_term     */
    uint64_t         votes_mask;      /* my tally while CANDIDATE         */
    uint64_t         election_deadline_ns;
    uint64_t         boot_ns;
    uint64_t         next_refresh_ns;
    uint64_t         last_quorum_mask;

    /* --- adopted cluster view --- */
    uint64_t         adopted_term;
    uint64_t         adopted_leader;
    uint64_t         adopted_lease_expire;
    uint64_t         adopted_token;

    /* --- hosted rings: I am HOME, each peer produces one (R(P->me)) --- */
    wcr1_ring_view_t hosted[WCR1_MAX_NODES];
    void            *hosted_region[WCR1_MAX_NODES];
    uint64_t         hosted_region_len[WCR1_MAX_NODES];
    uint32_t         hosted_count;

    /* --- producer rings: I produce one per peer (R(me->P)) --- */
    wcr1_ring_view_t producing[WCR1_MAX_NODES];
    uint64_t         producing_hb[WCR1_MAX_NODES];     /* hb seq cache    */
    uint64_t         peer_last_watermark[WCR1_MAX_NODES];
    uint64_t         peer_progress_ns[WCR1_MAX_NODES];
    uint64_t         evicted_mask;
    uint32_t         producing_count;

    wcr1_transport_t *transport;
    wcr1_stats_t     stats;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t wcr1_default_clock(void *user)
{
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t wcr1_ctx_now(const wcr1_ctx_t *ctx)
{
    return ctx->cfg.clock_fn ? ctx->cfg.clock_fn(ctx->cfg.clock_user)
                             : wcr1_default_clock(NULL);
}

static uint32_t wcr1_majority(uint32_t cluster_size)
{
    return cluster_size / 2u + 1u;
}

static uint64_t wcr1_bit(uint32_t node_id)
{
    return 1ull << (node_id - 1u);
}

static int wcr1_validate_cfg(const wcr1_cfg_t *cfg)
{
    if (!cfg) return WEFT_CLUSTER_E_ARG;
    if (cfg->node_id == 0 || cfg->node_id > WCR1_MAX_NODES)
        return WEFT_CLUSTER_E_NODE_ID;
    if (cfg->peer_count > WCR1_MAX_NODES - 1u)
        return WEFT_CLUSTER_E_NODE_LIMIT;
    if (cfg->cluster_size != cfg->peer_count + 1u)
        return WEFT_CLUSTER_E_ARG;
    if (cfg->lease_ns < WCR1_MIN_LEASE_NS)
        return WEFT_CLUSTER_E_ARG;
    bool id_zero = true;
    for (int i = 0; i < 16; i++) if (cfg->cluster_id[i]) { id_zero = false; break; }
    if (id_zero) return WEFT_CLUSTER_E_ARG;
    if (cfg->capacity_slots < WCR1_MIN_CAPACITY ||
        cfg->capacity_slots > WCR1_MAX_CAPACITY ||
        (cfg->capacity_slots & (cfg->capacity_slots - 1)) != 0)
        return WEFT_CLUSTER_E_CAPACITY;
    if (cfg->slot_size < WCR1_MIN_SLOT_SIZE || (cfg->slot_size % 64) != 0)
        return WEFT_CLUSTER_E_SLOT_SIZE;
    for (uint32_t i = 0; i < cfg->peer_count; i++) {
        if (cfg->peer_ids[i] == 0 || cfg->peer_ids[i] > WCR1_MAX_NODES ||
            cfg->peer_ids[i] == cfg->node_id)
            return WEFT_CLUSTER_E_NODE_ID;
        for (uint32_t j = 0; j < i; j++)
            if (cfg->peer_ids[i] == cfg->peer_ids[j])
                return WEFT_CLUSTER_E_NODE_ID;
    }
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Context lifecycle (Law 1: everything pre-allocated here)
// ---------------------------------------------------------------------------

int wcr1_ctx_create(const wcr1_cfg_t *cfg, wcr1_ctx_t **out)
{
    if (!out) return WEFT_CLUSTER_E_ARG;
    int e = wcr1_validate_cfg(cfg);
    if (e) return e;

    wcr1_ctx_t *ctx = (wcr1_ctx_t *)wcr1_engine_alloc(sizeof *ctx, 64);
    if (!ctx) return WEFT_CLUSTER_E_NOMEM;
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg = *cfg;
    ctx->node_id = cfg->node_id;
    ctx->role = WCR1_ROLE_FOLLOWER;
    ctx->transport = cfg->transport ? cfg->transport : &WCR1_TRANSPORT_LOCAL;
    if (!ctx->cfg.clock_fn) ctx->cfg.clock_fn = wcr1_default_clock;
    if (ctx->cfg.max_acquire_retries == 0)
        ctx->cfg.max_acquire_retries = WCR1_DEFAULT_ACQUIRE_RETRIES;
    ctx->boot_ns = wcr1_ctx_now(ctx);
    ctx->election_deadline_ns =
        ctx->boot_ns + wcr1_election_timeout_ns(cfg->node_id, cfg->lease_ns);

    /* Host one ring per peer producer: R(peer -> me). */
    for (uint32_t i = 0; i < cfg->peer_count; i++) {
        e = wcr1_ring_create(cfg, cfg->peer_ids[i], &ctx->hosted[i],
                             &ctx->hosted_region[i],
                             &ctx->hosted_region_len[i]);
        if (e) {
            for (uint32_t k = 0; k < i; k++)
                wcr1_engine_free(ctx->hosted_region[k]);
            wcr1_engine_free(ctx);
            return e;
        }
        ctx->hosted_count++;
    }
    *out = ctx;
    return WEFT_CLUSTER_OK;
}

void wcr1_ctx_destroy(wcr1_ctx_t *ctx)
{
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->hosted_count; i++)
        wcr1_engine_free(ctx->hosted_region[i]);
    wcr1_engine_free(ctx);
}

int wcr1_ctx_register_peer(wcr1_ctx_t *ctx, uint32_t peer_node_id,
                           void *region, uint64_t region_len,
                           int64_t peer_clock_offset_ns)
{
    if (!ctx || !region) return WEFT_CLUSTER_E_ARG;
    if (ctx->producing_count >= WCR1_MAX_NODES - 1u)
        return WEFT_CLUSTER_E_NODE_LIMIT;
    for (uint32_t i = 0; i < ctx->producing_count; i++)
        if (ctx->producing[i].home_node == peer_node_id)
            return WEFT_CLUSTER_E_NODE_ID;   /* duplicate registration */
    if (peer_clock_offset_ns > (int64_t)WCR1_MAX_CLOCK_SKEW_NS ||
        peer_clock_offset_ns < -(int64_t)WCR1_MAX_CLOCK_SKEW_NS)
        return WEFT_CLUSTER_E_CLOCK_SKEW;

    bool expected = false;
    for (uint32_t i = 0; i < ctx->cfg.peer_count; i++)
        if (ctx->cfg.peer_ids[i] == peer_node_id) { expected = true; break; }
    if (!expected) return WEFT_CLUSTER_E_NODE_ID;

    wcr1_ring_view_t view;
    int e = wcr1_ring_attach(region, region_len, ctx->cfg.cluster_id,
                             ctx->cfg.epoch, peer_node_id, ctx->node_id,
                             ctx->cfg.capacity_slots, ctx->cfg.slot_size,
                             &view);
    if (e) return e;

    uint32_t idx = ctx->producing_count;
    ctx->producing[idx] = view;

    /* Bootstrap the consensus hb cache with one stable local read. */
    wcr1_consensus_view_t cview;
    e = wcr1_consensus_read(&ctx->producing[idx],
                            ctx->cfg.max_acquire_retries, &cview);
    if (e) return e;
    ctx->producing_hb[idx] = cview.heartbeat_seq;

    uint64_t wm = 0;
    (void)wcr1_consumer_seq_read(&ctx->producing[idx], 0, &wm);
    ctx->peer_last_watermark[idx] = wm;
    ctx->peer_progress_ns[idx] = wcr1_ctx_now(ctx);
    ctx->producing_count++;
    return WEFT_CLUSTER_OK;
}

int wcr1_ctx_hosted_ring(wcr1_ctx_t *ctx, uint32_t producer_node_id,
                         wcr1_ring_view_t **out)
{
    if (!ctx || !out) return WEFT_CLUSTER_E_ARG;
    for (uint32_t i = 0; i < ctx->hosted_count; i++)
        if (ctx->hosted[i].producer_node == producer_node_id) {
            *out = &ctx->hosted[i];
            return WEFT_CLUSTER_OK;
        }
    return WEFT_CLUSTER_E_NODE_ID;
}

int wcr1_ctx_producer_ring(wcr1_ctx_t *ctx, uint32_t home_node_id,
                           wcr1_ring_view_t **out)
{
    if (!ctx || !out) return WEFT_CLUSTER_E_ARG;
    for (uint32_t i = 0; i < ctx->producing_count; i++)
        if (ctx->producing[i].home_node == home_node_id) {
            *out = &ctx->producing[i];
            return WEFT_CLUSTER_OK;
        }
    return WEFT_CLUSTER_E_NODE_ID;
}

// ---------------------------------------------------------------------------
// Consensus block read/write (seqlock via heartbeat_seq, RFC 0018 §6.1)
// ---------------------------------------------------------------------------

int wcr1_consensus_read(const wcr1_ring_view_t *view, uint64_t max_retries,
                        wcr1_consensus_view_t *out)
{
    if (!view || !view->hdr || !out) return WEFT_CLUSTER_E_ARG;
    if (max_retries == 0) max_retries = WCR1_DEFAULT_ACQUIRE_RETRIES;
    const wcr1_ring_header_t *h = view->hdr;

    for (uint64_t r = 0;; r++) {
        if (r >= max_retries) return WEFT_CLUSTER_E_CONSENSUS_TORN;

        /* Seqlock read: hb1 (acquire) -> fields (relaxed) -> rmb -> hb2. */
        uint64_t hb1 = __atomic_load_n(&h->heartbeat_seq, __ATOMIC_ACQUIRE);
        uint64_t term = __atomic_load_n(&h->term, __ATOMIC_RELAXED);
        uint64_t lease = __atomic_load_n(&h->lease_expire_ns, __ATOMIC_RELAXED);
        uint32_t leader = __atomic_load_n(&h->leader_node_id, __ATOMIC_RELAXED);
        uint32_t cflags = __atomic_load_n(&h->cflags, __ATOMIC_RELAXED);
        uint64_t token = __atomic_load_n(&h->fencing_token, __ATOMIC_RELAXED);
        uint64_t qmask = __atomic_load_n(&h->quorum_mask, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE); /* rmb: fields before hb2 */
        uint64_t hb2 = __atomic_load_n(&h->heartbeat_seq, __ATOMIC_ACQUIRE);

        if (hb1 != hb2 || (hb1 & 1ull)) continue;      /* unstable / in-progress */

        if (hb1 == 0) {                                 /* null state */
            if (term | lease | leader | cflags | token | qmask) continue;
            memset(out, 0, sizeof *out);
            return WEFT_CLUSTER_OK;
        }
        /* Pairing guard: token embeds (term, leader). A torn block whose
           fields came from two different refreshes cannot pass BOTH checks
           — this is the cross-writer tear detector. */
        if ((token >> 32) != term) continue;
        if ((token & 0xFFFFFFFFull) != (uint64_t)leader) continue;

        out->term = term;
        out->lease_expire_ns = lease;
        out->leader_node = leader;
        out->cflags = cflags;
        out->fencing_token = token;
        out->heartbeat_seq = hb1;
        out->quorum_mask = qmask;
        return WEFT_CLUSTER_OK;
    }
}

int wcr1_consensus_block_write(wcr1_ring_view_t *view,
                               const wcr1_transport_t *tr,
                               uint64_t term, uint32_t leader,
                               uint64_t lease_expire_ns, uint64_t token,
                               uint32_t cflags, uint64_t quorum_mask,
                               uint64_t *hb_cache_io)
{
    if (!view || !view->hdr || !hb_cache_io) return WEFT_CLUSTER_E_ARG;
    const wcr1_transport_t *t = tr ? tr : &WCR1_TRANSPORT_LOCAL;
    wcr1_ring_header_t *h = view->hdr;
    uint64_t hb = *hb_cache_io;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!t->store64((wcr1_transport_t *)t, &h->heartbeat_seq, hb + 1))
        return WEFT_CLUSTER_E_TRANSPORT;                 /* odd: invalidate */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    (void)t->store64((wcr1_transport_t *)t, &h->term, term);
    (void)t->store64((wcr1_transport_t *)t, &h->lease_expire_ns,
                     lease_expire_ns);
    (void)t->store32((wcr1_transport_t *)t, &h->leader_node_id, leader);
    (void)t->store32((wcr1_transport_t *)t, &h->cflags, cflags);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    (void)t->store64((wcr1_transport_t *)t, &h->fencing_token, token);
    (void)t->store64((wcr1_transport_t *)t, &h->quorum_mask, quorum_mask);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!t->store64((wcr1_transport_t *)t, &h->heartbeat_seq, hb + 2))
        return WEFT_CLUSTER_E_TRANSPORT;                 /* even: COMMIT */
    *hb_cache_io = hb + 2;
    return WEFT_CLUSTER_OK;
}

void wcr1_debug_poke_consensus(wcr1_ring_view_t *view, uint64_t term,
                               uint32_t leader, uint64_t lease_expire,
                               uint64_t token, uint64_t heartbeat_seq)
{
    if (!view || !view->hdr) return;
    wcr1_ring_header_t *h = view->hdr;
    wcr1_le64_put(&h->term, term);
    wcr1_le64_put(&h->lease_expire_ns, lease_expire);
    wcr1_le32_put(&h->leader_node_id, leader);
    wcr1_le32_put(&h->cflags, 0);
    wcr1_le64_put(&h->fencing_token, token);
    wcr1_le64_put(&h->heartbeat_seq, heartbeat_seq);
    wcr1_le64_put(&h->quorum_mask, 0);
}

// ---------------------------------------------------------------------------
// Election machinery
// ---------------------------------------------------------------------------

static int wcr1_become_leader(wcr1_ctx_t *ctx, uint64_t now);
static int wcr1_leader_refresh(wcr1_ctx_t *ctx, uint64_t now);

static int wcr1_start_election(wcr1_ctx_t *ctx, uint64_t now)
{
    ctx->local_term += 1;
    ctx->voted_for = ctx->node_id;          /* self-vote */
    ctx->votes_mask = wcr1_bit(ctx->node_id);
    ctx->role = WCR1_ROLE_CANDIDATE;
    /* Retry window = the node's STAGGERED timeout (not a flat lease): the
       node-id jitter de-synchronizes dueling candidates, so the lowest-id
       node re-campaigns first and breaks ties deterministically. */
    ctx->election_deadline_ns =
        now + wcr1_election_timeout_ns(ctx->node_id, ctx->cfg.lease_ns);
    ctx->stats.elections_started++;

    wcr1_control_msg_t msg = {
        .term = ctx->local_term, .node = ctx->node_id, .rsvd = 0
    };
    for (uint32_t i = 0; i < ctx->producing_count; i++) {
        uint64_t seq = 0;
        int e = wcr1_publish(&ctx->producing[i], ctx->transport, &msg,
                             sizeof msg, WCR1_SLOT_F_VOTE_REQUEST, now,
                             0 /* token: control plane, pre-leadership */,
                             ctx->cfg.epoch, &seq);
        if (e == WEFT_CLUSTER_E_BACKPRESSURE) {
            ctx->stats.bp_refusals++;       /* peer ring full: liveness fact */
        } else if (e == WEFT_CLUSTER_E_TRANSPORT) {
            ctx->stats.dropped_stores++;    /* unreachable: keep campaigning */
        } else if (e != WEFT_CLUSTER_OK) {
            return e;
        }
        /* (Election-in-progress observability rides the data plane:
           VOTE_REQUEST slots ARE the marker — RFC 0018 §6.3. Writing an
           out-of-seqlock cflags bit here would break the null-state
           invariant hb==0 => all-zero block and tear honest readers.) */
    }
    /* Single-node cluster (or a tally already at majority): immediate win. */
    if (__builtin_popcountll(ctx->votes_mask) >=
        wcr1_majority(ctx->cfg.cluster_size)) {
        (void)wcr1_become_leader(ctx, now);
        return wcr1_leader_refresh(ctx, now);
    }
    return WEFT_CLUSTER_OK;
}

static int wcr1_become_leader(wcr1_ctx_t *ctx, uint64_t now)
{
    ctx->role = WCR1_ROLE_LEADER;
    ctx->stats.elections_won++;
    ctx->adopted_term = ctx->local_term;
    ctx->adopted_leader = ctx->node_id;
    ctx->adopted_token = wcr1_fencing_token(ctx->local_term, ctx->node_id);
    ctx->adopted_lease_expire = now + ctx->cfg.lease_ns;
    ctx->next_refresh_ns = now;             /* refresh immediately */
    return WEFT_CLUSTER_OK;
}

static void wcr1_step_down(wcr1_ctx_t *ctx, uint64_t now)
{
    ctx->role = WCR1_ROLE_FOLLOWER;
    ctx->stats.quorum_losses++;
    ctx->election_deadline_ns =
        now + wcr1_election_timeout_ns(ctx->node_id, ctx->cfg.lease_ns);
}

/* Leader lease refresh: replicate the consensus block into every produced
   ring, account quorum, track peer consumption progress, evict zombies. */
static int wcr1_leader_refresh(wcr1_ctx_t *ctx, uint64_t now)
{
    uint64_t new_lease = now + ctx->cfg.lease_ns;
    uint64_t token = wcr1_fencing_token(ctx->local_term, ctx->node_id);
    uint32_t delivered = 1;                 /* self */
    uint64_t mask = wcr1_bit(ctx->node_id);
    uint64_t eviction_deadline =
        (uint64_t)WCR1_EVICTION_TIMEOUT_LEASES * ctx->cfg.lease_ns;

    for (uint32_t i = 0; i < ctx->producing_count; i++) {
        uint32_t peer = ctx->producing[i].home_node;
        if (ctx->evicted_mask & wcr1_bit(peer)) continue;

        int e = wcr1_consensus_block_write(
            &ctx->producing[i], ctx->transport, ctx->local_term,
            ctx->node_id, new_lease, token, 0 /* cflags */,
            ctx->last_quorum_mask, &ctx->producing_hb[i]);
        bool ok = (e == WEFT_CLUSTER_OK);
        if (ok) {
            delivered++;
            mask |= wcr1_bit(peer);
            /* Liveness echo: a header-only heartbeat slot whose CONSUMPTION
               advances the peer's watermark — the eviction signal. */
            uint64_t hb_seq = 0;
            (void)wcr1_publish(&ctx->producing[i], ctx->transport, NULL, 0,
                               WCR1_SLOT_F_HEARTBEAT, now, token,
                               ctx->cfg.epoch, &hb_seq);
        } else if (e == WEFT_CLUSTER_E_TRANSPORT) {
            ctx->stats.dropped_stores++;
        }

        /* Peer progress: watermark advance marks liveness (a consumer that
           drains at >=1 slot per eviction window is alive). */
        uint64_t wm = 0;
        (void)wcr1_consumer_seq_read(&ctx->producing[i], 0, &wm);
        if (wm != ctx->peer_last_watermark[i]) {
            ctx->peer_last_watermark[i] = wm;
            ctx->peer_progress_ns[i] = now;
        } else if (now - ctx->peer_progress_ns[i] > eviction_deadline) {
            /* Eviction: mark the ring's HOME node, fence it from the data
               plane. (Rejoin = new epoch: out of scope, refused honestly.) */
            ctx->evicted_mask |= wcr1_bit(peer);
            (void)wcr1_consensus_block_write(
                &ctx->producing[i], ctx->transport, ctx->local_term,
                ctx->node_id, new_lease, token, WCR1_CFLAG_NODE_EVICTED,
                ctx->last_quorum_mask, &ctx->producing_hb[i]);
        }
    }

    ctx->last_quorum_mask = mask;
    ctx->adopted_lease_expire = new_lease;
    ctx->next_refresh_ns = now + ctx->cfg.lease_ns / 2u;
    ctx->stats.lease_refreshes++;

    if (delivered < wcr1_majority(ctx->cfg.cluster_size)) {
        wcr1_step_down(ctx, now);
        return WEFT_CLUSTER_E_QUORUM_LOST;
    }
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// node_tick — one bounded, clock-driven protocol step
// ---------------------------------------------------------------------------

int wcr1_node_tick(wcr1_ctx_t *ctx)
{
    if (!ctx) return WEFT_CLUSTER_E_ARG;
    if (ctx->self_evicted) return WEFT_CLUSTER_E_NODE_EVICTED;
    uint64_t now = wcr1_ctx_now(ctx);
    int rc = WEFT_CLUSTER_OK;

    if (ctx->role == WCR1_ROLE_LEADER) {
        if (now >= ctx->adopted_lease_expire) {        /* stalled refreshes */
            wcr1_step_down(ctx, now);
            return WEFT_CLUSTER_E_LEASE_EXPIRED;
        }
        if (now >= ctx->next_refresh_ns)
            rc = wcr1_leader_refresh(ctx, now);
        return rc;
    }

    /* FOLLOWER / CANDIDATE */
    bool leader_live = ctx->adopted_token != 0 &&
                       now < ctx->adopted_lease_expire;
    if (leader_live) {
        ctx->election_deadline_ns =
            ctx->adopted_lease_expire +
            wcr1_election_timeout_ns(ctx->node_id, ctx->cfg.lease_ns);
        return WEFT_CLUSTER_OK;
    }
    if (now >= ctx->election_deadline_ns)
        return wcr1_start_election(ctx, now);
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Poll — bounded drain of hosted rings + consensus adoption
// ---------------------------------------------------------------------------

static int wcr1_handle_vote_request(wcr1_ctx_t *ctx,
                                    const wcr1_ring_view_t *ring,
                                    const wcr1_slot_view_t *slot, uint64_t now)
{
    if (slot->payload_bytes < sizeof(wcr1_control_msg_t)) {
        ctx->stats.malformed_control++;
        return WEFT_CLUSTER_OK;
    }
    wcr1_control_msg_t msg;
    memcpy(&msg, slot->payload, sizeof msg);
    uint32_t candidate = ring->producer_node;

    if (msg.node != candidate) {           /* identity mismatch: byzantine guard */
        ctx->stats.malformed_control++;
        return WEFT_CLUSTER_OK;
    }
    if (msg.term < ctx->local_term) {      /* stale request */
        ctx->stats.vote_refusals++;
        return WEFT_CLUSTER_OK;
    }
    if (msg.term == ctx->local_term && ctx->voted_for != 0 &&
        ctx->voted_for != msg.node) {      /* already voted this term */
        ctx->stats.vote_refusals++;
        return WEFT_CLUSTER_OK;
    }

    /* Grant — first grant or idempotent re-grant (safe to repeat: the
       candidate tallies grants into a bitmask, so duplicates are inert). */
    if (msg.term > ctx->local_term) {
        ctx->local_term = msg.term;
        /* A higher term exists — demote from ANY role (a leader that
           observes a higher term has lost leadership by definition). */
        ctx->role = WCR1_ROLE_FOLLOWER;
    }
    ctx->voted_for = msg.node;
    ctx->election_deadline_ns =
        now + wcr1_election_timeout_ns(ctx->node_id, ctx->cfg.lease_ns);

    {
        wcr1_ring_view_t *target = NULL;
        if (wcr1_ctx_producer_ring(ctx, candidate, &target) ==
            WEFT_CLUSTER_OK) {
            wcr1_control_msg_t grant = {
                .term = msg.term, .node = ctx->node_id, .rsvd = 0
            };
            uint64_t seq = 0;
            int e = wcr1_publish(target, ctx->transport, &grant, sizeof grant,
                                 WCR1_SLOT_F_VOTE_GRANT, now, 0,
                                 ctx->cfg.epoch, &seq);
            if (e == WEFT_CLUSTER_E_BACKPRESSURE) ctx->stats.bp_refusals++;
            else if (e == WEFT_CLUSTER_E_TRANSPORT)
                ctx->stats.dropped_stores++;
        } else {
            ctx->stats.malformed_control++;
        }
    }
    return WEFT_CLUSTER_OK;
}

static int wcr1_handle_vote_grant(wcr1_ctx_t *ctx,
                                  const wcr1_ring_view_t *ring,
                                  const wcr1_slot_view_t *slot, uint64_t now)
{
    if (ctx->role != WCR1_ROLE_CANDIDATE) return WEFT_CLUSTER_OK;
    if (slot->payload_bytes < sizeof(wcr1_control_msg_t)) {
        ctx->stats.malformed_control++;
        return WEFT_CLUSTER_OK;
    }
    wcr1_control_msg_t msg;
    memcpy(&msg, slot->payload, sizeof msg);
    uint32_t voter = ring->producer_node;

    /* The grant names its VOTER (msg.node) and must arrive on the ring the
       voter produces into my home. Anything else is stale or malformed. */
    if (msg.term != ctx->local_term || msg.node != voter) {
        if (msg.node != voter) ctx->stats.malformed_control++;
        return WEFT_CLUSTER_OK;
    }
    ctx->votes_mask |= wcr1_bit(voter);

    if (__builtin_popcountll(ctx->votes_mask) >=
        wcr1_majority(ctx->cfg.cluster_size)) {
        wcr1_become_leader(ctx, now);
        return wcr1_leader_refresh(ctx, now);
    }
    return WEFT_CLUSTER_OK;
}

/* Deterministic arbitration: higher term wins; same term -> lower node id. */
static bool wcr1_view_beats(const wcr1_consensus_view_t *a,
                            const wcr1_consensus_view_t *b)
{
    if (a->term != b->term) return a->term > b->term;
    return a->leader_node < b->leader_node;
}

int wcr1_node_poll(wcr1_ctx_t *ctx, uint32_t budget_per_ring)
{
    if (!ctx) return WEFT_CLUSTER_E_ARG;
    if (ctx->self_evicted) return WEFT_CLUSTER_E_NODE_EVICTED;
    if (budget_per_ring == 0) budget_per_ring = 64;
    uint64_t now = wcr1_ctx_now(ctx);
    int rc = WEFT_CLUSTER_OK;

    /* ---- 1. Consensus adoption across hosted rings --------------------
       Winner among observed blocks: (higher term, then lower node id).
       Same-view replicas contribute their freshest lease. Conflict = two
       DIFFERENT leaders observed at the same term (block-vs-block OR
       block-vs-adopted). */
    wcr1_consensus_view_t best;
    memset(&best, 0, sizeof best);
    bool have_any = false;
    bool conflict = false;

    for (uint32_t i = 0; i < ctx->hosted_count; i++) {
        wcr1_consensus_view_t c;
        int e = wcr1_consensus_read(&ctx->hosted[i],
                                    ctx->cfg.max_acquire_retries, &c);
        if (e == WEFT_CLUSTER_E_CONSENSUS_TORN) {
            ctx->stats.torn_consensus_reads++;
            continue;
        }
        if (c.cflags & WCR1_CFLAG_NODE_EVICTED) {
            ctx->self_evicted = true;      /* sticky: ops refuse from now on */
            continue;
        }
        if (c.fencing_token == 0) continue; /* null / not yet published */
        if (!have_any) { best = c; have_any = true; continue; }
        if (c.term == best.term && c.leader_node != best.leader_node)
            conflict = true;               /* same-term dueling views */
        if (wcr1_view_beats(&c, &best)) {
            best = c;
        } else if (c.term == best.term &&
                   c.leader_node == best.leader_node &&
                   c.lease_expire_ns > best.lease_expire_ns) {
            best.lease_expire_ns = c.lease_expire_ns;  /* fresher replica */
        }
    }

    /* Merge the winning block against the currently adopted view.
       RAFT SAFETY GATE: only views at or above our own current term are
       adoptable — a node that has seen term T ignores term<T leadership
       (it may have promised a vote at T) and resolves via its own
       election timeout instead of following a stale leader. */
    if (have_any && best.term >= ctx->local_term) {
        bool identity_change = false;
        if (ctx->adopted_token == 0) {
            identity_change = true;        /* first view ever adopted */
        } else if (best.term == ctx->adopted_term &&
                   best.leader_node != (uint32_t)ctx->adopted_leader) {
            conflict = true;               /* block disagrees w/ adopted */
            if (best.leader_node < (uint32_t)ctx->adopted_leader)
                identity_change = true;    /* deterministic tie-break */
        } else if (best.term > ctx->adopted_term) {
            identity_change = true;        /* higher term always wins */
        } else if (best.term == ctx->adopted_term &&
                   best.leader_node == (uint32_t)ctx->adopted_leader) {
            /* Same leadership: keep the freshest lease, never regress. */
            if (best.lease_expire_ns > ctx->adopted_lease_expire)
                ctx->adopted_lease_expire = best.lease_expire_ns;
        }

        if (identity_change) {
            ctx->adopted_term = best.term;
            ctx->adopted_leader = best.leader_node;
            ctx->adopted_lease_expire = best.lease_expire_ns;
            ctx->adopted_token = best.fencing_token;
            if (best.term > ctx->local_term) {
                ctx->local_term = best.term;
                ctx->voted_for = best.leader_node;  /* implied grant */
            }
            if (ctx->role != WCR1_ROLE_LEADER ||
                best.leader_node != ctx->node_id)
                ctx->role = WCR1_ROLE_FOLLOWER;     /* higher term always wins */

            /* Gossip the adopted view into MY produced rings (speeds
               partition healing; bounded: fires on identity changes only,
               never on lease refreshes — the leader refreshes everyone). */
            for (uint32_t i = 0; i < ctx->producing_count; i++) {
                uint32_t peer = ctx->producing[i].home_node;
                if (ctx->evicted_mask & wcr1_bit(peer)) continue;
                (void)wcr1_consensus_block_write(
                    &ctx->producing[i], ctx->transport, best.term,
                    best.leader_node, best.lease_expire_ns,
                    best.fencing_token, 0, ctx->last_quorum_mask,
                    &ctx->producing_hb[i]);
            }
        }
    }

    if (conflict) {
        ctx->stats.split_brain_refusals++;
        rc = WEFT_CLUSTER_E_SPLIT_BRAIN;   /* named + deterministic winner */
    }
    if (ctx->self_evicted) return WEFT_CLUSTER_E_NODE_EVICTED;

    /* ---- 2. Drain hosted rings (bounded per ring) --------------------- */
    for (uint32_t i = 0; i < ctx->hosted_count; i++) {
        for (uint32_t k = 0; k < budget_per_ring; k++) {
            wcr1_slot_view_t slot;
            int e = wcr1_acquire_next(&ctx->hosted[i],
                                      ctx->cfg.max_acquire_retries, &slot);
            if (e == WEFT_CLUSTER_E_NOT_PUBLISHED) break;
            if (e == WEFT_CLUSTER_E_SEQ_TORN) {
                ctx->stats.torn_seq_reads++;
                break;                      /* bounded: retry next poll */
            }
            if (e == WEFT_CLUSTER_E_SEQ_OVERRUN) {
                /* Honest gap: resync cursor to the live watermark, count it. */
                uint64_t committed = 0;
                (void)wcr1_producer_seq_read(&ctx->hosted[i], 0, &committed);
                ctx->hosted[i].my_cursor = committed;
                ctx->stats.seq_overruns++;
                ctx->stats.gap_resyncs++;
                break;
            }
            if (e != WEFT_CLUSTER_OK) return e;

            ctx->stats.acquires++;
            uint32_t flags = slot.flags;
            if (flags & WCR1_SLOT_F_VOTE_REQUEST)
                (void)wcr1_handle_vote_request(ctx, &ctx->hosted[i], &slot,
                                               now);
            else if (flags & WCR1_SLOT_F_VOTE_GRANT)
                (void)wcr1_handle_vote_grant(ctx, &ctx->hosted[i], &slot,
                                             now);
            else if (flags & WCR1_SLOT_F_HEARTBEAT) {
                /* liveness echo: consumed below, nothing to deliver */
            }
            else {
                /* Data plane: fence stale-token slots, deliver the rest. */
                if (slot.fencing_token < ctx->adopted_token) {
                    ctx->stats.fenced_slots++;
                } else if (ctx->cfg.on_data) {
                    ctx->cfg.on_data(ctx->cfg.on_data_user, slot.seq,
                                     slot.hdr, slot.payload);
                }
            }
            if (wcr1_consume(&ctx->hosted[i], slot.seq) == WEFT_CLUSTER_OK)
                ctx->stats.consumes++;
        }
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Leader-gated publish + fencing
// ---------------------------------------------------------------------------

int wcr1_fence_check(const wcr1_ctx_t *ctx, uint64_t op_token)
{
    if (!ctx) return WEFT_CLUSTER_E_ARG;
    if (op_token < ctx->adopted_token) return WEFT_CLUSTER_E_FENCED;
    return WEFT_CLUSTER_OK;
}

int wcr1_publish_as_leader(wcr1_ctx_t *ctx, uint32_t home_node_id,
                           const void *payload, uint32_t payload_bytes,
                           uint32_t flags, uint64_t *out_seq)
{
    if (!ctx) return WEFT_CLUSTER_E_ARG;
    if (ctx->self_evicted) return WEFT_CLUSTER_E_NODE_EVICTED;
    if (ctx->role != WCR1_ROLE_LEADER) return WEFT_CLUSTER_E_NOT_LEADER;

    uint64_t now = wcr1_ctx_now(ctx);
    if (now >= ctx->adopted_lease_expire) {
        wcr1_step_down(ctx, now);
        return WEFT_CLUSTER_E_LEASE_EXPIRED;
    }

    wcr1_ring_view_t *ring = NULL;
    int e = wcr1_ctx_producer_ring(ctx, home_node_id, &ring);
    if (e) return e;
    if (ctx->evicted_mask & wcr1_bit(home_node_id))
        return WEFT_CLUSTER_E_NODE_EVICTED;   /* fenced: zombie peer */

    e = wcr1_publish(ring, ctx->transport, payload, payload_bytes, flags,
                     now, ctx->adopted_token, ctx->cfg.epoch, out_seq);
    if (e == WEFT_CLUSTER_E_BACKPRESSURE) ctx->stats.bp_refusals++;
    if (e == WEFT_CLUSTER_OK) ctx->stats.publishes++;
    return e;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

int wcr1_ctx_role(const wcr1_ctx_t *ctx)
{
    return ctx ? ctx->role : WCR1_ROLE_FOLLOWER;
}

uint64_t wcr1_ctx_term(const wcr1_ctx_t *ctx)
{
    return ctx ? ctx->local_term : 0;
}

uint64_t wcr1_ctx_token(const wcr1_ctx_t *ctx)
{
    return ctx ? ctx->adopted_token : 0;
}

uint64_t wcr1_ctx_lease_expire(const wcr1_ctx_t *ctx)
{
    return ctx ? ctx->adopted_lease_expire : 0;
}

uint32_t wcr1_ctx_votes_mask(const wcr1_ctx_t *ctx)
{
    return ctx ? (uint32_t)ctx->votes_mask : 0;
}

const wcr1_stats_t *wcr1_ctx_stats(const wcr1_ctx_t *ctx)
{
    return ctx ? &ctx->stats : NULL;
}
