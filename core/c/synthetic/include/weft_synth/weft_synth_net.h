// weft_synth_net.h — Virtual RDMA/UDP Network Chaos Injector + WCR1-style
// lease consensus reference (Pillar 8, module C).
//
// WHY EXISTS: cluster consensus code must survive the network real silicon
// actually has — Bernoulli packet loss, Gilbert-Elliott burst drops,
// microsecond jitter that reorders frames, silent bit rot, and split-brain
// partitions. This module is a software fault-injection layer that sits
// BETWEEN consensus endpoints as a virtual time-stepped fabric: every
// packet's fate (dropped / delayed / corrupted / delivered) is decided
// deterministically at send time from the seeded PRNG, then played out on
// the virtual clock. On top of it runs a lease-based single-primary
// consensus reference implementing the WCR1 contract — monotonic epochs,
// one primary per epoch, majority-quorum leases — so the directive's
// verification requirement ("consensus maintains monotonic epochs and
// single-primary leases under > 20% loss, including split-brain and
// healing") is a property the battery can pound on for millions of ticks.
//
// LAWS CARRIED HERE:
//   Law 1  zero heap: the fabric and consensus engine are caller-allocated
//          structs with fixed pools/wheels/rings — send/tick/recv and the
//          consensus step never allocate (allocator interposition proof).
//   Law 2  determinism: all fate draws flow from the seeded PRNG in a fixed
//          order (drop -> bitflip -> jitter); same seed + same script =
//          same trace hash, replayable bit-for-bit.
//   Law 3  bounded state: pool, wheel buckets, and RX rings have hard
//          capacities; exhaustion is COUNTED (dropped_pool /
//          dropped_rx_full), never silently dropped, never reallocated.
//   Law 4  strict C11 portability, both architectures, -Werror -pedantic.
//   Law 5  weft_synth_net_* / weft_synth_consensus_* symbols only.
//
// THREADING CONTRACT: the fabric is a DETERMINISTIC SIMULATOR — one
// owning thread drives send/tick/recv/consensus (that serialization is
// what makes chaos replayable). It is deliberately not thread-safe; the
// torture battery runs it on its own thread while the bus blender and
// seqlock writer/reader threads hammer alongside.
//
// HONESTY BOUNDARY: this is a link-behavior emulator, not a TCP/IP stack:
// no congestion control, no retransmit (loss is the point), one shared
// Gilbert-Elliott channel for the whole fabric (per-link channels are a
// declared residual), and "RDMA" means the same one-copy packet handoff
// semantics, not RoCE verbs.

#ifndef WEFT_SYNTH__NET_H_
#define WEFT_SYNTH__NET_H_

#include "weft_synth_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- fixed geometry (Law 3: hard capacities, never grown) ---------------- */

#define WEFT_SYNTH_NET_MAX_NODES 16u
#define WEFT_SYNTH_NET_PKT_PAYLOAD 192u
#define WEFT_SYNTH_NET_WHEEL_SPAN 1024u      /* timing wheel, 1 tick = 1 us */
#define WEFT_SYNTH_NET_POOL_CAP 8192u        /* in-flight packet pool       */
#define WEFT_SYNTH_NET_RX_CAP 256u           /* per-node RX ring (packets)  */
#define WEFT_SYNTH_NET_NO_IDX 0xFFFFFFFFu

/* --- packet kinds (consensus rides the same wire as DATA) ---------------- */

enum {
    WEFT_SYNTH_NET_KIND_DATA = 0,
    WEFT_SYNTH_NET_KIND_VOTE_REQ = 1,
    WEFT_SYNTH_NET_KIND_VOTE_GRANT = 2,
    WEFT_SYNTH_NET_KIND_HEARTBEAT = 3,
    WEFT_SYNTH_NET_KIND_HB_ACK = 4,
};

/* --- fault profile -------------------------------------------------------- */

typedef enum {
    WEFT_SYNTH_NET_FAULT_NONE = 0,
    WEFT_SYNTH_NET_FAULT_BERNOULLI = 1,
    WEFT_SYNTH_NET_FAULT_GILBERT_ELLIOTT = 2,
} weft_synth_net_fault_model_t;

typedef struct weft_synth_net_fault_cfg {
    int enabled;                 /* 0 = clean wire (hooks ~free)           */
    weft_synth_net_fault_model_t model;
    double drop_p;               /* BERNOULLI: per-packet drop probability */
    double ge_p_g2b;             /* GE: good -> bad transition per packet  */
    double ge_p_b2g;             /* GE: bad -> good transition per packet  */
    double ge_drop_good;         /* GE: drop probability in GOOD state    */
    double ge_drop_bad;          /* GE: drop probability in BAD state     */
    double jitter_mean_ns;       /* gaussian mean (>= 0)                  */
    double jitter_std_ns;        /* gaussian sigma (0 = deterministic)    */
    double jitter_max_ns;        /* hard clamp; must keep delay < wheel   */
    double reorder_p;            /* P(extra delay -> out-of-order)        */
    double reorder_extra_mean_ns;/* exponential mean of the extra delay   */
    double bitflip_p;            /* per-packet corruption probability     */
    uint32_t bitflip_bits_min;   /* bits flipped per corrupted packet     */
    uint32_t bitflip_bits_max;
} weft_synth_net_fault_cfg_t;

/// Clean wire by default (the chaos profile is opt-in per scenario).
void weft_synth_net_fault_defaults(weft_synth_net_fault_cfg_t *f);

/* --- fabric configuration + statistics ------------------------------------ */

typedef struct weft_synth_net_cfg {
    uint32_t n_nodes;            /* 2..MAX_NODES                          */
    uint32_t seed;
    uint32_t rx_capacity;        /* 1..RX_CAP per node                    */
    uint32_t pool_capacity;      /* 1..POOL_CAP in-flight packets         */
    weft_synth_net_fault_cfg_t fault;
} weft_synth_net_cfg_t;

typedef struct weft_synth_net_stats {
    uint64_t sent;               /* handed to the injector                */
    uint64_t delivered;          /* placed into a dst RX ring             */
    uint64_t delivered_corrupt;  /* delivered with flipped payload        */
    uint64_t dropped_bernoulli;
    uint64_t dropped_ge;
    uint64_t dropped_partition;
    uint64_t dropped_rx_full;    /* bounded ring honest overflow          */
    uint64_t dropped_pool;       /* bounded pool honest overflow          */
    uint64_t corrupted_injected; /* bit-flip decisions at send time       */
    uint64_t rejected_crc;       /* recv()-side CRC rejections            */
    uint64_t jittered;           /* packets delayed by jitter draws       */
    uint64_t reordered_events;   /* deliveries behind a newer seq         */
    uint64_t ticks;              /* virtual microseconds elapsed          */
    int ge_bad_state;            /* final shared-channel GE state         */
} weft_synth_net_stats_t;

/* --- packet --------------------------------------------------------------- */

typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_net_pkt {
    uint32_t crc;                /* 0 = chaos-off packet (no CRC run)     */
    uint32_t src;
    uint32_t dst;
    uint64_t seq;                /* sender monotonic                      */
    uint8_t  kind;
    uint8_t  corrupted;          /* injector flag — CROSS-CHECK ONLY; the
                                    receiver's proof is the CRC mismatch  */
    uint16_t payload_len;
    uint64_t deliver_at_tick;    /* virtual us at which it lands          */
    uint32_t next_idx;           /* intrusive timing-wheel link           */
    uint8_t  payload[WEFT_SYNTH_NET_PKT_PAYLOAD];
} weft_synth_net_pkt_t;

/* --- the fabric (caller-allocated, ~3 MB with default capacities) -------- */

typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_net {
    weft_synth_net_cfg_t cfg;
    uint64_t rng;                /* fate stream (Law 2)                   */
    uint64_t trace_hash;        /* deterministic fingerprint of decisions */
    uint64_t tick_now;           /* virtual clock, 1 tick = 1 us          */
    int ge_bad;                  /* shared GE channel state               */
    uint32_t wheel_head[WEFT_SYNTH_NET_WHEEL_SPAN];
    uint32_t wheel_tail[WEFT_SYNTH_NET_WHEEL_SPAN];  /* FIFO buckets: a
                                     wire must not reorder same-tick
                                     packets by data-structure accident */
    uint32_t pool_free[WEFT_SYNTH_NET_POOL_CAP];
    uint32_t pool_free_top;      /* free indices on the stack             */
    weft_synth_net_pkt_t pool[WEFT_SYNTH_NET_POOL_CAP];
    struct {
        uint32_t head, tail, count;
        weft_synth_net_pkt_t ring[WEFT_SYNTH_NET_RX_CAP];
    } rx[WEFT_SYNTH_NET_MAX_NODES];
    uint64_t seq_ctr[WEFT_SYNTH_NET_MAX_NODES];
    uint64_t last_seq_seen[WEFT_SYNTH_NET_MAX_NODES]
                          [WEFT_SYNTH_NET_MAX_NODES];
    uint32_t groups[WEFT_SYNTH_NET_MAX_NODES]; /* 0 = no partition group  */
    uint32_t groups_nonzero;    /* cached: nodes carrying a group id — the
                                  partition check folds into the single
                                  interceptor gate below               */
    int interceptor_armed;      /* cached fault.enabled — ONE branch prices
                                  the whole chaos interceptor when the lab
                                  is disabled (the < 5% mandate)        */
    weft_synth_net_stats_t stats;
} weft_synth_net_t;

/// Five-node, seed-1, clean-wire defaults. The consensus scenarios in the
/// battery install their own fault profiles on top.
int weft_synth_net_defaults(weft_synth_net_cfg_t *cfg);

/// Initialize the fabric (pool free-list, wheel, rings, PRNG). Fails
/// closed on out-of-range geometry or a jitter_max that would overshoot
/// the timing wheel horizon. NOTE: initializes the process-wide CRC table
/// on first use — call once before threads if you use CRC concurrently
/// (the fabric itself is single-threaded by contract).
int weft_synth_net_init(weft_synth_net_t *net,
                        const weft_synth_net_cfg_t *cfg);

/// Send one packet (hot path, zero heap). The injector decides fate NOW,
/// deterministically: partition -> drop model -> bit-flip -> jitter, then
/// schedules delivery at tick+1+delay. Pool exhaustion is counted and
/// reported as a normal chaos outcome (WEFT_SYNTH_OK — the send itself
/// succeeded; check stats.dropped_pool). Returns 0 / -INVALID.
int weft_synth_net_send(weft_synth_net_t *net, uint32_t from, uint32_t to,
                        uint8_t kind, const void *payload, uint32_t len);

/// Advance the virtual clock by 1 us and deliver everything due. The
/// minimum transit is one tick, so a send is visible to recv() no earlier
/// than the NEXT tick — that is the fabric's causality seam.
int weft_synth_net_tick(weft_synth_net_t *net);

/// Pop the oldest packet for a node. Returns 0 = empty, 1 = clean packet,
/// 2 = delivered but CRC-invalid (corruption detected at the receiver;
/// stats.rejected_crc bumped — the caller must treat it as garbage),
/// -INVALID on bad args.
int weft_synth_net_recv(weft_synth_net_t *net, uint32_t node,
                        weft_synth_net_pkt_t *out_pkt);

/// Standard CRC-32 (poly 0xEDB88320, reflected, init/xorout 0xFFFFFFFF)
/// over a contiguous buffer — the validation the bit-flip injector is
/// proven against.
uint32_t weft_synth_net_crc32(const void *data, size_t len);

/// Recompute + compare a packet's CRC. Chaos-off packets (crc == 0) are
/// valid by construction. Returns 1 valid / 0 invalid.
int weft_synth_net_pkt_valid(const weft_synth_net_pkt_t *pkt);

/// Split-brain partition: assign every node a group id (0 = ungrouped,
/// reachable by all). Sends between two nonzero, different groups are
/// blocked bidirectionally and counted. Deterministic (folds the group
/// vector into the trace hash).
void weft_synth_net_partition_set(weft_synth_net_t *net,
                                  const uint32_t groups[
                                      WEFT_SYNTH_NET_MAX_NODES]);

/// Heal the fabric: every group assignment clears (in-flight packets
/// already scheduled still land — that is honest wire state).
void weft_synth_net_partition_heal(weft_synth_net_t *net);

/// Determinism fingerprint of every chaos decision this fabric made.
uint64_t weft_synth_net_trace_hash(const weft_synth_net_t *net);

/* --- WCR1-style lease consensus reference --------------------------------- */
//
// Protocol (lease-based single-primary over the chaos fabric):
//   * followers time out (randomized [emin, emax]) and campaign with
//     epoch = own_epoch + 1 — epochs are STRICTLY monotonic per node and
//     globally unique per granted lease (majorities intersect).
//   * a node grants ONE vote per epoch (pkt.epoch > node.epoch), steps
//     down if it was primary, and resets its election timer.
//   * majority of votes -> PRIMARY with a lease of lease_ttl us; the
//     primary heartbeats every hb_period us and renews only while a
//     majority of HB_ACKs still arrives.
//   * lease expiry demotes EXACTLY at the expiry tick; election timeouts
//     (emin >= lease_ttl) guarantee any successor is elected only after
//     the old lease is dead — the structural no-dual-primary argument.
// Safety ledger (asserted by the battery after every scenario):
//   dual_primary_ticks == 0, epoch_reuse_violations == 0,
//   node_epoch_regressions == 0.

typedef struct weft_synth_consensus_cfg {
    uint32_t n_nodes;            /* 2..MAX_NODES (odd sizes avoid ties)    */
    uint32_t seed;
    uint64_t lease_ttl_ticks;    /* 175 us                                 */
    uint64_t hb_period_ticks;    /* 25 us — 7 rounds per lease window      */
    uint64_t election_min_ticks; /* 250 us — MUST exceed lease_ttl         */
    uint64_t election_max_ticks; /* 500 us                                 */
    uint32_t crash_mask;         /* bit i: node i is crashed (mute+deaf)   */
} weft_synth_consensus_cfg_t;

enum {
    WEFT_SYNTH_CONSENSUS_FOLLOWER = 0,
    WEFT_SYNTH_CONSENSUS_CANDIDATE = 1,
    WEFT_SYNTH_CONSENSUS_PRIMARY = 2,
};

typedef struct weft_synth_cnode {
    int role;
    uint64_t epoch;              /* highest epoch seen (monotonic)        */
    uint64_t voted_epoch;        /* epoch this node last voted for        */
    uint64_t campaign_epoch;     /* own campaign while CANDIDATE          */
    uint64_t lease_epoch;        /* epoch of the lease this node holds    */
    uint64_t lease_expires;      /* virtual tick                          */
    uint64_t election_timeout_at;
    uint64_t next_hb_at;
    uint32_t vote_mask;          /* grants received for campaign_epoch    */
    uint32_t ack_mask;           /* HB_ACKs received since last renewal   */
    uint64_t prev_epoch;         /* regression tripwire mirror            */
} weft_synth_cnode_t;

typedef struct WEFT_SYNTH_ALIGNED(64) weft_synth_consensus {
    weft_synth_consensus_cfg_t cfg;
    uint64_t rng;                /* timeout lottery (Law 2)               */
    uint64_t tick;               /* virtual clock mirror                  */
    uint64_t trace_hash;         /* role/grant event fingerprint          */
    weft_synth_cnode_t nodes[WEFT_SYNTH_NET_MAX_NODES];
    /* safety + liveness ledger */
    uint64_t grants;             /* leases ever granted                   */
    uint64_t stepdowns;
    uint64_t votes_sent;
    uint64_t hbs_sent;
    uint64_t msgs_received;
    uint64_t invalid_msgs;       /* CRC-rejected consensus messages       */
    uint64_t dual_primary_ticks; /* ticks with >1 valid-lease primary     */
    uint64_t epoch_reuse_violations;
    uint64_t node_epoch_regressions;
    uint64_t last_granted_epoch;
    uint64_t has_grant;
    uint32_t last_granted_node;
    uint64_t max_epoch;
    uint64_t ticks_with_primary; /* availability numerator                */
    uint64_t ticks_elapsed;
} weft_synth_consensus_t;

int weft_synth_consensus_defaults(weft_synth_consensus_cfg_t *cfg);

/// Initialize all followers with staggered randomized election timeouts.
/// Fails closed if election_min <= lease_ttl (the no-dual-primary
/// structural argument would be void) or geometry is out of range.
int weft_synth_consensus_init(weft_synth_consensus_t *cs,
                              const weft_synth_consensus_cfg_t *cfg);

/// Change the crash mask live (crash / resurrect nodes mid-scenario —
/// process-restart modeling). Returns the previous mask.
uint32_t weft_synth_consensus_set_crash_mask(weft_synth_consensus_t *cs,
                                             uint32_t mask);

/// One consensus step: drain every node's inbox through the fabric (CRC
/// garbage is counted and dropped), run elections/leases/heartbeats, then
/// scan the safety ledger. Drive AFTER weft_synth_net_tick() each virtual
/// microsecond. Returns 0 / -INVALID.
int weft_synth_consensus_tick(weft_synth_consensus_t *cs,
                              weft_synth_net_t *net);

/// The node currently holding a valid (unexpired) primary lease, or -1.
/// At most one exists at any tick — that is the invariant the ledger
/// counts violations of.
int weft_synth_consensus_primary_node(const weft_synth_consensus_t *cs);

/// Determinism fingerprint of the consensus trajectory.
uint64_t weft_synth_consensus_trace_hash(const weft_synth_consensus_t *cs);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_SYNTH__NET_H_ */
