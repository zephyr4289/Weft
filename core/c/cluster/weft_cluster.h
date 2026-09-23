// weft_cluster.h — WCR1 (Weft Cluster Ring v1) protocol engine, distributed
// seqlock, and ring-embedded consensus. RFC 0018 (normative).
//
// Scope (Pillar 3, Senior Systems Engineer 1 — protocol engine, memory
// layout, multi-node ring, distributed consensus):
//   - WCR1 128-byte ring header + 64-byte slot header, little-endian,
//     64-byte cacheline bracketed, one-sided-RDMA (ibv_reg_mr) ready.
//   - Distributed circular queue synchronization: monotonic sequence
//     accounting, watermarked commit registers, and PURE-ARITHMETIC remote
//     slot addressing (zero roundtrip RPCs on the steady-state path).
//   - Lock-free distributed seqlock: two-store atomic publish (producer_seq
//     hi/lo split) compatible with hardware DMA write completion; wait-free
//     consumer acquire with automatic tear detection and bounded retry.
//   - Cross-node backpressure: in-band markers embedded in slot headers —
//     no TCP ACKs, no broker, no serialization.
//   - Ring-embedded Raft: leader election, lease expiry, term epochs and
//     fencing tokens living directly in the ring header; deterministic
//     split-brain arbitration without zookeeper/etcd.
//
// Weft Core Laws (enforced here):
//   Law 1 (zero heap on hot path): all ring regions, peer tables and
//        consensus scratch are allocated at ctx-create / attach time.
//        Steady-state publish/acquire/tick/poll perform 0 malloc/free —
//        enforced by the wcr1_alloc accounting hooks (see tests).
//   Law 2 (bounded, deterministic): every retry loop in this header has a
//        compile-time-visible bound; all wire fields are explicit LE;
//        all structures are 64-byte cacheline bracketed.
//   Law 3 (byte-frozen kernel): core/c/weft.{c,h} is untouched. Everything
//        lives under core/c/cluster/. No dependency on the frozen kernel.
//   Law 4 (honest boundaries): every failure mode returns a UNIQUE NAMED
//        code from weft_cluster_err_t; nothing fails silently.
//
// Threading / ownership contract (normative, RFC 0018 §7):
//   Per ring R(P→H): produced by node P, hosted (consumed) by node H.
//   - seq_hi/seq_lo          : written ONLY by P (two-store protocol)
//   - cons_hi/cons_lo        : written ONLY by H (watermark, two-store)
//   - consensus block (term, lease_expire, leader, cflags, fencing_token,
//     heartbeat_seq, quorum_mask): written ONLY by P (seqlock publisher)
//   - identity cacheline     : written ONLY by H at create; immutable after
//   One producer thread per producing view; one consumer thread per hosted
//   view. Cross-thread multi-consumer coordination is Pillar 2 (weft-tensor)
//   local-ring territory; WCR1 v1 is one actor per role per ring.
//
// Memory model: all shared fields are plain uint32_t/uint64_t accessed via
// __atomic builtins (lock-free on x86-64/aarch64). Over the fabric the
// transport must preserve ISSUE ORDER of stores from one producer to one
// destination ring (RC QP write ordering; the local transport gets this
// from program order + TSO). WCR1 assumes aligned <=8-byte stores of a
// single field are atomic-on-arrival (x86-64/TSO and single DMA TLP);
// cross-field consistency is provided by the two-store parity protocol and
// the heartbeat_seq consensus seqlock, never by assuming multi-field
// atomicity.

#ifndef WEFT_CLUSTER_H
#define WEFT_CLUSTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version / identity constants
// ---------------------------------------------------------------------------

#define WCR1_MAGIC             0x31524357u   /* LE bytes 'W','C','R','1' */
#define WCR1_LAYOUT_MAJOR      1u
#define WCR1_LAYOUT_MINOR      0u

#define WCR1_HEADER_SIZE       128u          /* ring header, 2 cachelines  */
#define WCR1_SLOT_HEADER_SIZE  64u           /* slot header, 1 cacheline   */
#define WCR1_MIN_SLOT_SIZE     128u          /* 64B header + >=64B payload */
#define WCR1_MIN_CAPACITY      2u
#define WCR1_MAX_CAPACITY      (1u << 20)
#define WCR1_MAX_NODES         64u           /* votes_mask / quorum width  */

#define WCR1_DEFAULT_LEASE_NS        (10ull * 1000 * 1000)   /* 10 ms      */
#define WCR1_MAX_CLOCK_SKEW_NS       (250ull * 1000)         /* 250 us     */
#define WCR1_MIN_LEASE_NS            (4ull * WCR1_MAX_CLOCK_SKEW_NS + 2)
#define WCR1_DEFAULT_ACQUIRE_RETRIES 64u

/* Backpressure policy (normative, RFC 0018 §5): high-water = capacity/2
 * in-flight slots (lag) — slot is published with F_BP_MARK; at lag >=
 * capacity the publish is REFUSED (E_BACKPRESSURE) rather than overwriting
 * unconsumed data. The consumer watermark is the only "ACK" equivalent. */
#define WCR1_BP_HIGH_WATER_DIV  2u

// ---------------------------------------------------------------------------
// Error ladder (Law 4 — every failure has a unique, named code)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_CLUSTER_OK                 = 0,

    /* --- attach / validation ladder ----------------------------------- */
    WEFT_CLUSTER_E_MAGIC            = 1,  /* magic != WCR1                    */
    WEFT_CLUSTER_E_LAYOUT           = 2,  /* layout major/minor unsupported   */
    WEFT_CLUSTER_E_CLUSTER_ID       = 3,  /* cluster UUID mismatch            */
    WEFT_CLUSTER_E_CFG_CRC          = 4,  /* config CRC-32/IEEE mismatch      */
    WEFT_CLUSTER_E_CAPACITY         = 5,  /* capacity not pow2 / out of range */
    WEFT_CLUSTER_E_SLOT_SIZE        = 6,  /* slot_size not 64-mult / < 128    */
    WEFT_CLUSTER_E_ALIGN            = 7,  /* region or slot misaligned        */
    WEFT_CLUSTER_E_REGION_SIZE      = 8,  /* region != 128 + cap*slot_size    */
    WEFT_CLUSTER_E_EPOCH            = 9,  /* epoch regression / mismatch      */
    WEFT_CLUSTER_E_NODE_ID          = 10, /* bad/duplicate node id            */
    WEFT_CLUSTER_E_NODE_EVICTED     = 11, /* node marked evicted (cflags)     */
    WEFT_CLUSTER_E_NODE_LIMIT       = 12, /* peer table full (WCR1_MAX_NODES) */
    WEFT_CLUSTER_E_CLOCK_SKEW       = 13, /* peer skew > WCR1_MAX_CLOCK_SKEW  */

    /* --- ring hot-path ladder ----------------------------------------- */
    WEFT_CLUSTER_E_SEQ_TORN         = 14, /* seqlock tear after bounded retry */
    WEFT_CLUSTER_E_RETRY_EXHAUSTED  = 15, /* bounded retry budget exhausted   */
    WEFT_CLUSTER_E_NOT_PUBLISHED    = 16, /* cursor+1 not committed yet       */
    WEFT_CLUSTER_E_SEQ_OVERRUN      = 17, /* consumer lagged >= capacity      */
    WEFT_CLUSTER_E_SEQ_CORRUPT      = 18, /* slot message_seq inconsistent    */
    WEFT_CLUSTER_E_BACKPRESSURE     = 19, /* ring full: slow consumer, refuse */
    WEFT_CLUSTER_E_PAYLOAD          = 20, /* payload > payload_capacity       */
    WEFT_CLUSTER_E_CRC              = 21, /* opt-in slot CRC mismatch         */
    WEFT_CLUSTER_E_ARG              = 22, /* invalid argument                 */
    WEFT_CLUSTER_E_STATE            = 23, /* ctx/view state violation         */

    /* --- consensus ladder ---------------------------------------------- */
    WEFT_CLUSTER_E_CONSENSUS_TORN   = 24, /* consensus seqlock tear           */
    WEFT_CLUSTER_E_LEASE_EXPIRED    = 25, /* leader lease expired             */
    WEFT_CLUSTER_E_NOT_LEADER       = 26, /* op requires leadership           */
    WEFT_CLUSTER_E_QUORUM_LOST      = 27, /* leader lost majority, stepped dn */
    WEFT_CLUSTER_E_TERM_STALE       = 28, /* observed term < local term       */
    WEFT_CLUSTER_E_FENCED           = 29, /* op fenced by newer token         */
    WEFT_CLUSTER_E_VOTE_GRANTED     = 30, /* already voted this term          */
    WEFT_CLUSTER_E_ELECTION_BUSY    = 31, /* election in progress             */
    WEFT_CLUSTER_E_SPLIT_BRAIN      = 32, /* two valid leaders same term      */

    /* --- transport / lifecycle ----------------------------------------- */
    WEFT_CLUSTER_E_PARTITION        = 33, /* link unavailable                 */
    WEFT_CLUSTER_E_TRANSPORT        = 34, /* remote store failed              */
    WEFT_CLUSTER_E_NOMEM            = 35, /* attach-time allocation failure   */
} weft_cluster_err_t;

const char *wcr1_err_name(int err);   /* unique name per code, never NULL */

// ---------------------------------------------------------------------------
// Two-store seqlock encoding (RFC 0018 §4 — normative)
// ---------------------------------------------------------------------------
// A 64-bit monotonic sequence S is published as TWO ordered 32-bit stores:
//   store #1: seq_lo = (uint32_t)S                       (data half)
//   store #2: seq_hi = hi_word(S)                        (COMMIT — final)
// where hi_word(S) = ((S >> 32) & 0x7FFFFFFF) | ((S & 1) << 31).
// Bit 31 of seq_hi MIRRORS bit 0 of S (the per-increment parity).
//
// Tear detection: the transient window between the two stores exposes
// (old_hi, new_lo); since S increments by exactly 1, old/new parity ALWAYS
// differ, so the mirror bit mismatches -> reader sees a torn pair. The
// reader double-reads both halves; a pair is stable+valid iff
//   hi1==hi2 && lo1==lo2 && (hi>>31)==(lo&1)  &&  hi's bit31==parity(lo).
// A writer crashing between the two stores leaves a permanently torn pair
// -> bounded retries exhaust -> WEFT_CLUSTER_E_SEQ_TORN (honest refusal).
//
// The same encoding is used for the consumer watermark (cons_hi/cons_lo):
// single home-node writer, remote-producer reader.
//
// Sequence range: bits 0..62 (2^63-1). Wrapping is not supported (at 1e9
// ops/s that is 292 years); the refusal ladder reserves E_SEQ_CORRUPT.

static inline uint32_t wcr1_seq_hi_word(uint64_t seq)
{
    return (uint32_t)(((seq >> 32) & 0x7FFFFFFFu) | ((seq & 1u) << 31));
}

static inline uint64_t wcr1_seq_from_halves(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)(hi & 0x7FFFFFFFu) << 32) | (uint64_t)lo;
}

static inline bool wcr1_seq_parity_ok(uint32_t hi, uint32_t lo)
{
    return ((hi >> 31) & 1u) == (lo & 1u);
}

// ---------------------------------------------------------------------------
// WCR1 ring header — 128 bytes, 64-byte aligned (RFC 0018 §3, normative)
// ---------------------------------------------------------------------------
// Cacheline 0 (offsets 0x00-0x3F): identity + geometry. Immutable after
// create. config_crc32 covers bytes [0x00,0x20).
// Cacheline 1 (offsets 0x40-0x7F): synchronization + consensus. Field
// ownership per the contract at the top of this file.

/* Slot flags (slot header +0x24) */
#define WCR1_SLOT_F_HEARTBEAT    (1u << 0)  /* liveness/lease slot (header-only) */
#define WCR1_SLOT_F_VOTE_REQUEST (1u << 1)  /* in-band election: request votes   */
#define WCR1_SLOT_F_VOTE_GRANT   (1u << 2)  /* in-band election: grant a vote    */
#define WCR1_SLOT_F_BP_MARK      (1u << 3)  /* published at/above high-water     */
#define WCR1_SLOT_F_WITH_CRC     (1u << 4)  /* opt-in payload+header CRC         */
#define WCR1_SLOT_F_STALE_TOKEN  (1u << 5)  /* set by reader: fenced on arrival  */

/* Consensus flags (ring header +0x64, producer-written, INSIDE the
   heartbeat_seq seqlock bracket only — never written bare). Election
   visibility rides the data plane: VOTE_REQUEST slots are the marker. */
#define WCR1_CFLAG_NODE_EVICTED  (1u << 0)  /* this ring's HOME node is evicted */

/* Publish flag passthrough (wcr1_publish) */
#define WCR1_PUBLISH_F_WITH_CRC  WCR1_SLOT_F_WITH_CRC

typedef struct wcr1_ring_header {
    /* --- cacheline 0: identity + geometry (immutable) ------------------ */
    uint32_t magic;              /* +0x00 WCR1_MAGIC, LE                    */
    uint8_t  layout_major;       /* +0x04 WCR1_LAYOUT_MAJOR                 */
    uint8_t  layout_minor;       /* +0x05 WCR1_LAYOUT_MINOR                 */
    uint16_t hdr_flags;          /* +0x06 reserved, 0                       */
    uint8_t  cluster_id[16];     /* +0x08 UUIDv4, canonical byte order      */
    uint32_t home_node_id;       /* +0x18 host/consumer node                */
    uint32_t producer_node_id;   /* +0x1C the single remote producer        */
    uint32_t config_crc32;       /* +0x20 CRC-32/IEEE over [0x00,0x20)      */
    uint32_t reserved0;          /* +0x24 zero                              */
    uint64_t epoch;              /* +0x28 cluster config epoch, monotonic   */
    uint64_t capacity_slots;     /* +0x30 power of two                      */
    uint64_t slot_size;          /* +0x38 multiple of 64, >= 128            */

    /* --- cacheline 1: synchronization + consensus (mutable) ------------- */
    uint32_t seq_hi;             /* +0x40 producer_seq hi|parity-mirror     */
    uint32_t seq_lo;             /* +0x44 producer_seq lo — COMMIT store    */
    uint32_t cons_hi;            /* +0x48 consumer watermark hi|parity      */
    uint32_t cons_lo;            /* +0x4C consumer watermark lo — COMMIT    */
    uint64_t term;               /* +0x50 cluster raft term, monotonic      */
    uint64_t lease_expire_ns;    /* +0x58 leader lease expiry (mono clock)  */
    uint32_t leader_node_id;     /* +0x60 current leader (0 = none)         */
    uint32_t cflags;             /* +0x64 WCR1_CFLAG_*                      */
    uint64_t fencing_token;      /* +0x68 (term<<32)|leader — LAST field    */
    uint64_t heartbeat_seq;      /* +0x70 consensus seqlock: +=2 per pub    */
    uint64_t quorum_mask;        /* +0x78 observability: refresh ack bitmap */
} wcr1_ring_header_t;

/* Fencing token (RFC 0018 §6.4): strictly ordered; higher term wins; the
 * pair (term, leader) is embedded so a torn consensus block can NEVER pass
 * validation (reader checks token>>32==term && (token&0xffffffff)==leader). */
static inline uint64_t wcr1_fencing_token(uint64_t term, uint32_t leader_node)
{
    return (term << 32) | (uint64_t)leader_node;
}

// ---------------------------------------------------------------------------
// WCR1 slot header — 64 bytes (RFC 0018 §3.3, normative)
// ---------------------------------------------------------------------------
// Slot i lives at region offset 128 + i*slot_size; payload at slot+64.
// payload_capacity = slot_size - 64.

typedef struct wcr1_slot_header {
    uint64_t message_seq;        /* +0x00 seq assigned by producer          */
    uint64_t timestamp_ns;       /* +0x08 producer monotonic clock          */
    uint64_t fencing_token;      /* +0x10 publisher token at publish        */
    uint64_t producer_epoch;     /* +0x18 cluster epoch at publish          */
    uint32_t payload_bytes;      /* +0x20 actual payload length             */
    uint32_t flags;              /* +0x24 WCR1_SLOT_F_*                     */
    uint64_t bp_lag;             /* +0x28 in-band producer lag snapshot     */
    uint32_t payload_crc32;      /* +0x30 iff F_WITH_CRC                    */
    uint32_t header_crc32;       /* +0x34 CRC over [0x00,0x34), iff CRC     */
    uint32_t producer_node_id;   /* +0x38 self-describing producer          */
    uint32_t reserved0;          /* +0x3C zero                              */
} wcr1_slot_header_t;

/* Static layout proofs (compile-time, Law 2). */
_Static_assert(sizeof(wcr1_ring_header_t) == 128, "WCR1 header must be 128B");
_Static_assert(sizeof(wcr1_slot_header_t) == 64, "WCR1 slot header must be 64B");
_Static_assert(offsetof(wcr1_ring_header_t, seq_hi) == 0x40, "seq_hi @0x40");
_Static_assert(offsetof(wcr1_ring_header_t, seq_lo) == 0x44, "seq_lo @0x44");
_Static_assert(offsetof(wcr1_ring_header_t, cons_hi) == 0x48, "cons_hi @0x48");
_Static_assert(offsetof(wcr1_ring_header_t, term) == 0x50, "term @0x50");
_Static_assert(offsetof(wcr1_ring_header_t, fencing_token) == 0x68, "token @0x68");
_Static_assert(offsetof(wcr1_ring_header_t, heartbeat_seq) == 0x70, "hb @0x70");
_Static_assert(offsetof(wcr1_slot_header_t, payload_crc32) == 0x30, "pcrc @0x30");

// ---------------------------------------------------------------------------
// Little-endian store/load (Law 2 — explicit on-wire formatting)
// ---------------------------------------------------------------------------

void wcr1_le32_put(void *dst, uint32_t v);
void wcr1_le64_put(void *dst, uint64_t v);
uint32_t wcr1_le32_get(const void *src);
uint64_t wcr1_le64_get(const void *src);

// ---------------------------------------------------------------------------
// CRC-32/IEEE (poly 0xEDB88320, init/xorout 0xFFFFFFFF, reflected)
// ---------------------------------------------------------------------------

uint32_t wcr1_crc32(const void *data, size_t len);
/* Incremental (attach-time only; never on the hot path). */
uint32_t wcr1_crc32_update(uint32_t crc, const void *data, size_t len);

// ---------------------------------------------------------------------------
// Transport seam (Engineer 2 integration point — RFC 0018 §8)
// ---------------------------------------------------------------------------
// The protocol engine issues every REMOTE write through this vtable. The
// default (local) transport performs direct stores — semantics identical to
// RDMA one-sided writes landing in mapped memory. Engineer 2's
// libibverbs/XDP/io_uring layer replaces the vtable with an RC-QP-backed
// transport; the contract it must honor:
//   1. Stores from ONE producer to ONE destination ring are delivered in
//      ISSUE ORDER (RC QP write ordering / fenced WRs).
//   2. store_buf of a slot payload completes before subsequent store32/64
//      to the same ring (payload before commit register).
//   3. Return true iff the store is KNOWN delivered; false = undelivered
//      (used for quorum accounting only — never for correctness of a
//      single published message, which is one-sided/fire-and-forget).

typedef struct wcr1_transport wcr1_transport_t;
struct wcr1_transport {
    void *user;
    bool (*store32)(wcr1_transport_t *t, volatile uint32_t *dest, uint32_t v);
    bool (*store64)(wcr1_transport_t *t, volatile uint64_t *dest, uint64_t v);
    bool (*store_buf)(wcr1_transport_t *t, void *dest, const void *src,
                      size_t len);
};

/* Default: direct local stores (SHM / simulated fabric). Always true. */
extern wcr1_transport_t WCR1_TRANSPORT_LOCAL;

// ---------------------------------------------------------------------------
// Zero-allocation accounting hooks (Law 1 proof — test/audit surface)
// ---------------------------------------------------------------------------
// All engine allocations go through these. Install counting hooks, run
// steady-state ops, assert the counter did not move. NOT hot-path API.

typedef void *(*wcr1_alloc_fn)(size_t size, size_t alignment, void *user);
typedef void  (*wcr1_free_fn)(void *ptr, void *user);
void wcr1_set_alloc_hooks(wcr1_alloc_fn a, wcr1_free_fn f, void *user);

// ---------------------------------------------------------------------------
// Configuration + views
// ---------------------------------------------------------------------------

typedef uint64_t (*wcr1_clock_fn)(void *user);

typedef struct {
    uint8_t          cluster_id[16];  /* UUIDv4, shared by all members     */
    uint32_t         node_id;         /* this node, 1..WCR1_MAX_NODES      */
    uint32_t         cluster_size;    /* total nodes (quorum = N/2+1)       */
    uint64_t         epoch;           /* cluster config epoch               */
    uint64_t         capacity_slots;  /* per ring, power of two             */
    uint64_t         slot_size;       /* multiple of 64, >= 128             */
    uint64_t         lease_ns;        /* leader lease (>= WCR1_MIN_LEASE_NS)*/
    uint32_t         peer_ids[WCR1_MAX_NODES];   /* expected peers (ids)    */
    uint32_t         peer_count;                     /* <= WCR1_MAX_NODES-1 */
    uint64_t         max_acquire_retries;           /* 0 => default (64)     */
    wcr1_clock_fn    clock_fn;        /* NULL => CLOCK_MONOTONIC            */
    void            *clock_user;
    wcr1_transport_t *transport;      /* NULL => WCR1_TRANSPORT_LOCAL       */
    /* Optional data-plane delivery callback (hosted rings, poll). */
    void (*on_data)(void *user, uint64_t seq, const wcr1_slot_header_t *hdr,
                    const void *payload);
    void *on_data_user;
} wcr1_cfg_t;

/* A validated handle onto ONE ring (either hosted-here or produced-by-me). */
typedef struct {
    wcr1_ring_header_t *hdr;        /* mapped 128B header                   */
    uint8_t            *slots;      /* slot 0 base == region + 128          */
    uint64_t            capacity;
    uint64_t            slot_size;
    uint64_t            payload_capacity;
    uint32_t            home_node;
    uint32_t            producer_node;
    /* producer-side cache (only touched when producing into this ring) */
    uint64_t            my_producer_seq;
    /* consumer-side cursor (only touched when hosting this ring) */
    uint64_t            my_cursor;
} wcr1_ring_view_t;

/* Zero-copy acquired slot view (all pointers INTO the ring region). */
typedef struct {
    const wcr1_slot_header_t *hdr;
    const void               *payload;
    uint64_t                  payload_bytes;
    uint64_t                  seq;
    uint64_t                  fencing_token;
    uint32_t                  flags;
} wcr1_slot_view_t;

/* Consensus snapshot (stable read of one ring header's consensus block). */
typedef struct {
    uint64_t term;
    uint64_t lease_expire_ns;
    uint32_t leader_node;
    uint32_t cflags;
    uint64_t fencing_token;
    uint64_t heartbeat_seq;
    uint64_t quorum_mask;
} wcr1_consensus_view_t;

/* Node roles. */
enum { WCR1_ROLE_FOLLOWER = 0, WCR1_ROLE_CANDIDATE = 1, WCR1_ROLE_LEADER = 2 };

/* Deterministic election timeout (RFC 0018 §6.2 — node-id staggered). */
uint64_t wcr1_election_timeout_ns(uint32_t node_id, uint64_t lease_ns);

/* Engine statistics (pre-allocated in ctx; zero-alloc counting). */
typedef struct {
    uint64_t publishes;
    uint64_t acquires;
    uint64_t consumes;
    uint64_t torn_seq_reads;
    uint64_t torn_consensus_reads;
    uint64_t bp_refusals;
    uint64_t fenced_slots;
    uint64_t seq_overruns;         /* consumer lagged >= capacity */
    uint64_t gap_resyncs;          /* post-overrun cursor resyncs */
    uint64_t vote_refusals;        /* stale/duplicate vote requests */
    uint64_t malformed_control;    /* control slot w/ bad identity */
    uint64_t elections_started;
    uint64_t elections_won;
    uint64_t lease_refreshes;
    uint64_t quorum_losses;
    uint64_t split_brain_refusals;
    uint64_t dropped_stores;       /* transport reported undelivered */
} wcr1_stats_t;

// ---------------------------------------------------------------------------
// Opaque node context (fixed-size internally; created at attach time)
// ---------------------------------------------------------------------------

typedef struct wcr1_ctx wcr1_ctx_t;

// ---------------------------------------------------------------------------
// Ring engine (weft_cluster_ring.c)
// ---------------------------------------------------------------------------

/* Region size for a ring (exact): 128 + capacity * slot_size. */
uint64_t wcr1_region_size(uint64_t capacity_slots, uint64_t slot_size);

/* Pure-arith remote addressing — Node A computes Node B's slot address with
 * ZERO roundtrip RPCs (the MR base is exchanged once, out-of-band, at
 * attach). Both functions are side-effect free. */
uint64_t wcr1_slot_byte_offset(uint64_t seq, uint64_t capacity,
                               uint64_t slot_size);
const void *wcr1_remote_slot_addr(const void *mr_base, uint64_t seq,
                                  uint64_t capacity, uint64_t slot_size);

/* Create a hosted ring region (HOME side). Allocates via engine hooks,
 * 128-byte aligned, mlock'd best-effort (failure recorded, see RFC §9
 * honesty notes), header written LE, CRC computed. Fills *view. */
int wcr1_ring_create(const wcr1_cfg_t *cfg, uint32_t producer_node_id,
                     wcr1_ring_view_t *view, void **region_out,
                     uint64_t *region_len_out);

/* Attach to an existing ring region (PRODUCER side or second host-side
 * mapping). Full validation ladder — every refusal is a distinct code. */
int wcr1_ring_attach(void *region, uint64_t region_len,
                     const uint8_t cluster_id[16], uint64_t epoch,
                     uint32_t expected_home, uint32_t expected_producer,
                     uint64_t expected_capacity, uint64_t expected_slot_size,
                     wcr1_ring_view_t *view);

/* Publish one message into a ring this node produces (two-store commit).
 * Payload length must be <= payload_capacity. flags: WCR1_PUBLISH_F_*.
 * Stamp: token/epoch/producer id from the view's header snapshot are
 * re-read at publish time. Returns OK + *out_seq, or the named refusal. */
int wcr1_publish(wcr1_ring_view_t *view, const wcr1_transport_t *transport,
                 const void *payload, uint32_t payload_bytes, uint32_t flags,
                 uint64_t now_ns, uint64_t fencing_token, uint64_t epoch,
                 uint64_t *out_seq);

/* Wait-free acquire of cursor+1. Bounded retries; tear -> E_SEQ_TORN.
 * E_NOT_PUBLISHED when the watermark hasn't reached cursor+1 (honest
 * non-blocking miss — never spins). E_SEQ_OVERRUN when the producer has
 * wrapped past the consumer (alias check via slot message_seq). */
int wcr1_acquire_next(wcr1_ring_view_t *view, uint64_t max_retries,
                      wcr1_slot_view_t *out);

/* Home-node consumer watermark commit (two-store) + cursor advance. */
int wcr1_consume(wcr1_ring_view_t *view, uint64_t seq);

/* Stable read of the committed producer watermark (producer or observer). */
int wcr1_producer_seq_read(const wcr1_ring_view_t *view,
                           uint64_t max_retries, uint64_t *out_seq);

/* Stable read of the consumer watermark (producer side, lag accounting). */
int wcr1_consumer_seq_read(const wcr1_ring_view_t *view,
                           uint64_t max_retries, uint64_t *out_seq);

/* Opt-in slot integrity check (flags & F_WITH_CRC). */
int wcr1_slot_crc_check(const wcr1_slot_view_t *view);

// ---------------------------------------------------------------------------
// Consensus engine (weft_cluster_consensus.c)
// ---------------------------------------------------------------------------

/* Cluster node context: hosts one ring per peer producer (allocated at
 * create — Law 1), plus producer views registered during attach. */
int wcr1_ctx_create(const wcr1_cfg_t *cfg, wcr1_ctx_t **out);
void wcr1_ctx_destroy(wcr1_ctx_t *ctx);

/* Register a peer-hosted ring I produce into (attach-validated).
 * peer_clock_offset_ns: observed clock delta for skew gating. */
int wcr1_ctx_register_peer(wcr1_ctx_t *ctx, uint32_t peer_node_id,
                           void *region, uint64_t region_len,
                           int64_t peer_clock_offset_ns);

/* Ring view accessors (indices are registration order). */
int wcr1_ctx_hosted_ring(wcr1_ctx_t *ctx, uint32_t producer_node_id,
                         wcr1_ring_view_t **out);
int wcr1_ctx_producer_ring(wcr1_ctx_t *ctx, uint32_t home_node_id,
                           wcr1_ring_view_t **out);

/* One bounded protocol step (Law 2: deterministic, clock-driven):
 *   LEADER   -> lease refresh when due (replicate consensus block to all
 *               producer rings; step down on quorum loss);
 *   FOLLOWER -> start election when the lease has been expired past this
 *               node's deterministic election timeout;
 *   CANDIDATE-> re-elect (term+1) when votes don't converge in time. */
int wcr1_node_tick(wcr1_ctx_t *ctx);

/* Drain hosted rings (bounded budget per ring): dispatch control slots
 * (vote requests/grants), advance watermarks, deliver data slots to
 * cfg.on_data (with stale-token fencing), and ADOPT the max-token stable
 * consensus block seen across hosted rings. */
int wcr1_node_poll(wcr1_ctx_t *ctx, uint32_t budget_per_ring);

/* Introspection. */
int    wcr1_ctx_role(const wcr1_ctx_t *ctx);
uint64_t wcr1_ctx_term(const wcr1_ctx_t *ctx);
uint64_t wcr1_ctx_token(const wcr1_ctx_t *ctx);
uint64_t wcr1_ctx_lease_expire(const wcr1_ctx_t *ctx);
uint32_t wcr1_ctx_votes_mask(const wcr1_ctx_t *ctx);
const wcr1_stats_t *wcr1_ctx_stats(const wcr1_ctx_t *ctx);

/* Stable consensus read from ONE ring view (bounded retry, seqlock via
 * heartbeat_seq parity + fencing-token pairing). */
int wcr1_consensus_read(const wcr1_ring_view_t *view, uint64_t max_retries,
                        wcr1_consensus_view_t *out);

/* Fencing validation: refuse if op_token < the ctx's max adopted token. */
int wcr1_fence_check(const wcr1_ctx_t *ctx, uint64_t op_token);

/* Leader-gated publish (data plane under consensus): requires
 * role==LEADER and a live lease, else E_NOT_LEADER / E_LEASE_EXPIRED. */
int wcr1_publish_as_leader(wcr1_ctx_t *ctx, uint32_t home_node_id,
                           const void *payload, uint32_t payload_bytes,
                           uint32_t flags, uint64_t *out_seq);

/* Test/adversarial surface: poke a consensus block directly into a ring
 * header WITHOUT the seqlock protocol (for tear/Byzantine injection). */
void wcr1_debug_poke_consensus(wcr1_ring_view_t *view, uint64_t term,
                               uint32_t leader, uint64_t lease_expire,
                               uint64_t token, uint64_t heartbeat_seq);

#ifdef __cplusplus
}
#endif

#endif /* WEFT_CLUSTER_H */
