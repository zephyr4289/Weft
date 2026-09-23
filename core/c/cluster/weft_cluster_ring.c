// weft_cluster_ring.c — WCR1 ring engine (RFC 0018 §3-§5).
//
// Implements: little-endian wire formatting, CRC-32/IEEE, engine allocation
// hooks (Law 1 accounting), region creation (128B-aligned, mlock
// best-effort), the attach validation ladder (Law 4), the two-store
// seqlock publish, the wait-free bounded-retry acquire with tear/overrun/
// alias detection, the consumer watermark protocol, in-band backpressure,
// and the pure-arithmetic remote slot addressing used for zero-roundtrip
// one-sided RDMA writes.
//
// Ordering discipline (see weft_cluster.h header comment):
//   payload/header stores -> release fence -> seq_lo store -> seq_hi store
// (the FINAL seq_hi store is the commit point; everything before it is
// invisible to a correct reader). Over the fabric the transport preserves
// issue order (RC QP); locally, program order + TSO.

#include "weft_cluster.h"
#include "weft_cluster_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "WCR1 requires a little-endian host (RFC 0018 section 2)"
#endif

// ---------------------------------------------------------------------------
// Little-endian formatting (identity on WCR1-legal hosts; memcpy keeps the
// compiler from inventing anything cleverer or UB-er)
// ---------------------------------------------------------------------------

void wcr1_le32_put(void *dst, uint32_t v) { memcpy(dst, &v, sizeof v); }
void wcr1_le64_put(void *dst, uint64_t v) { memcpy(dst, &v, sizeof v); }
uint32_t wcr1_le32_get(const void *src) { uint32_t v; memcpy(&v, src, sizeof v); return v; }
uint64_t wcr1_le64_get(const void *src) { uint64_t v; memcpy(&v, src, sizeof v); return v; }

// ---------------------------------------------------------------------------
// CRC-32/IEEE (poly 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF,
// reflected). Table computed once under pthread_once — zero heap, typo-proof
// (verified against the standard check value in cluster_ring_test.c R1).
// ---------------------------------------------------------------------------

static uint32_t wcr1_crc_table[256];
static pthread_once_t wcr1_crc_once = PTHREAD_ONCE_INIT;

static void wcr1_crc_table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        wcr1_crc_table[i] = c;
    }
}

uint32_t wcr1_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = wcr1_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t wcr1_crc32(const void *data, size_t len)
{
    pthread_once(&wcr1_crc_once, wcr1_crc_table_init);
    return wcr1_crc32_update(0, data, len);
}

// ---------------------------------------------------------------------------
// Engine allocation hooks (Law 1: attach-time only; steady-state ops never
// allocate — enforced by tests counting through these hooks)
// ---------------------------------------------------------------------------

static void *wcr1_default_alloc(size_t size, size_t alignment, void *user)
{
    (void)user;
    void *p = NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}

static void wcr1_default_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}

static wcr1_alloc_fn g_alloc = wcr1_default_alloc;
static wcr1_free_fn  g_free  = wcr1_default_free;
static void         *g_alloc_user = NULL;

void wcr1_set_alloc_hooks(wcr1_alloc_fn a, wcr1_free_fn f, void *user)
{
    g_alloc = a ? a : wcr1_default_alloc;
    g_free  = f ? f : wcr1_default_free;
    g_alloc_user = user;
}

void *wcr1_engine_alloc(size_t size, size_t alignment)
{
    return g_alloc(size, alignment, g_alloc_user);
}

void wcr1_engine_free(void *ptr)
{
    if (ptr) g_free(ptr, g_alloc_user);
}

// ---------------------------------------------------------------------------
// Error ladder names (Law 4)
// ---------------------------------------------------------------------------

const char *wcr1_err_name(int err)
{
    switch (err) {
    case WEFT_CLUSTER_OK:                 return "WEFT_CLUSTER_OK";
    case WEFT_CLUSTER_E_MAGIC:            return "WEFT_CLUSTER_E_MAGIC";
    case WEFT_CLUSTER_E_LAYOUT:           return "WEFT_CLUSTER_E_LAYOUT";
    case WEFT_CLUSTER_E_CLUSTER_ID:       return "WEFT_CLUSTER_E_CLUSTER_ID";
    case WEFT_CLUSTER_E_CFG_CRC:          return "WEFT_CLUSTER_E_CFG_CRC";
    case WEFT_CLUSTER_E_CAPACITY:         return "WEFT_CLUSTER_E_CAPACITY";
    case WEFT_CLUSTER_E_SLOT_SIZE:        return "WEFT_CLUSTER_E_SLOT_SIZE";
    case WEFT_CLUSTER_E_ALIGN:            return "WEFT_CLUSTER_E_ALIGN";
    case WEFT_CLUSTER_E_REGION_SIZE:      return "WEFT_CLUSTER_E_REGION_SIZE";
    case WEFT_CLUSTER_E_EPOCH:            return "WEFT_CLUSTER_E_EPOCH";
    case WEFT_CLUSTER_E_NODE_ID:          return "WEFT_CLUSTER_E_NODE_ID";
    case WEFT_CLUSTER_E_NODE_EVICTED:     return "WEFT_CLUSTER_E_NODE_EVICTED";
    case WEFT_CLUSTER_E_NODE_LIMIT:       return "WEFT_CLUSTER_E_NODE_LIMIT";
    case WEFT_CLUSTER_E_CLOCK_SKEW:       return "WEFT_CLUSTER_E_CLOCK_SKEW";
    case WEFT_CLUSTER_E_SEQ_TORN:         return "WEFT_CLUSTER_E_SEQ_TORN";
    case WEFT_CLUSTER_E_RETRY_EXHAUSTED:  return "WEFT_CLUSTER_E_RETRY_EXHAUSTED";
    case WEFT_CLUSTER_E_NOT_PUBLISHED:    return "WEFT_CLUSTER_E_NOT_PUBLISHED";
    case WEFT_CLUSTER_E_SEQ_OVERRUN:      return "WEFT_CLUSTER_E_SEQ_OVERRUN";
    case WEFT_CLUSTER_E_SEQ_CORRUPT:      return "WEFT_CLUSTER_E_SEQ_CORRUPT";
    case WEFT_CLUSTER_E_BACKPRESSURE:     return "WEFT_CLUSTER_E_BACKPRESSURE";
    case WEFT_CLUSTER_E_PAYLOAD:          return "WEFT_CLUSTER_E_PAYLOAD";
    case WEFT_CLUSTER_E_CRC:              return "WEFT_CLUSTER_E_CRC";
    case WEFT_CLUSTER_E_ARG:              return "WEFT_CLUSTER_E_ARG";
    case WEFT_CLUSTER_E_STATE:            return "WEFT_CLUSTER_E_STATE";
    case WEFT_CLUSTER_E_CONSENSUS_TORN:   return "WEFT_CLUSTER_E_CONSENSUS_TORN";
    case WEFT_CLUSTER_E_LEASE_EXPIRED:    return "WEFT_CLUSTER_E_LEASE_EXPIRED";
    case WEFT_CLUSTER_E_NOT_LEADER:       return "WEFT_CLUSTER_E_NOT_LEADER";
    case WEFT_CLUSTER_E_QUORUM_LOST:      return "WEFT_CLUSTER_E_QUORUM_LOST";
    case WEFT_CLUSTER_E_TERM_STALE:       return "WEFT_CLUSTER_E_TERM_STALE";
    case WEFT_CLUSTER_E_FENCED:           return "WEFT_CLUSTER_E_FENCED";
    case WEFT_CLUSTER_E_VOTE_GRANTED:     return "WEFT_CLUSTER_E_VOTE_GRANTED";
    case WEFT_CLUSTER_E_ELECTION_BUSY:    return "WEFT_CLUSTER_E_ELECTION_BUSY";
    case WEFT_CLUSTER_E_SPLIT_BRAIN:      return "WEFT_CLUSTER_E_SPLIT_BRAIN";
    case WEFT_CLUSTER_E_PARTITION:        return "WEFT_CLUSTER_E_PARTITION";
    case WEFT_CLUSTER_E_TRANSPORT:        return "WEFT_CLUSTER_E_TRANSPORT";
    case WEFT_CLUSTER_E_NOMEM:            return "WEFT_CLUSTER_E_NOMEM";
    default:                              return "WEFT_CLUSTER_E_UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Default local transport — direct stores into mapped memory (SHM /
// simulated fabric). Production fabric: Engineer 2 swaps the vtable with an
// RC-QP-backed ibverbs transport honoring the seam contract.
// ---------------------------------------------------------------------------

static bool wcr1_local_store32(wcr1_transport_t *t, volatile uint32_t *dest,
                               uint32_t v)
{
    (void)t;
    __atomic_store_n(dest, v, __ATOMIC_RELAXED);
    return true;
}

static bool wcr1_local_store64(wcr1_transport_t *t, volatile uint64_t *dest,
                               uint64_t v)
{
    (void)t;
    __atomic_store_n(dest, v, __ATOMIC_RELAXED);
    return true;
}

static bool wcr1_local_store_buf(wcr1_transport_t *t, void *dest,
                                 const void *src, size_t len)
{
    (void)t;
    memcpy(dest, src, len);
    return true;
}

wcr1_transport_t WCR1_TRANSPORT_LOCAL = {
    .user      = NULL,
    .store32   = wcr1_local_store32,
    .store64   = wcr1_local_store64,
    .store_buf = wcr1_local_store_buf,
};

// ---------------------------------------------------------------------------
// Geometry + pure remote addressing
// ---------------------------------------------------------------------------

uint64_t wcr1_region_size(uint64_t capacity_slots, uint64_t slot_size)
{
    return WCR1_HEADER_SIZE + capacity_slots * slot_size;
}

uint64_t wcr1_slot_byte_offset(uint64_t seq, uint64_t capacity,
                               uint64_t slot_size)
{
    return WCR1_HEADER_SIZE + (seq & (capacity - 1)) * slot_size;
}

const void *wcr1_remote_slot_addr(const void *mr_base, uint64_t seq,
                                  uint64_t capacity, uint64_t slot_size)
{
    const uint8_t *base = (const uint8_t *)mr_base;
    return base + wcr1_slot_byte_offset(seq, capacity, slot_size);
}

// ---------------------------------------------------------------------------
// Internal: stable two-store register reads (bounded — Law 2)
// ---------------------------------------------------------------------------

/* Read a (hi,lo) register pair into *out with double-read + parity check.
   Returns WEFT_CLUSTER_OK or WEFT_CLUSTER_E_SEQ_TORN. */
int wcr1_seqreg_read(const volatile uint32_t *hi, const volatile uint32_t *lo,
                     uint64_t max_retries, uint64_t *out)
{
    if (max_retries == 0) max_retries = WCR1_DEFAULT_ACQUIRE_RETRIES;
    for (uint64_t r = 0;; r++) {
        if (r >= max_retries) return WEFT_CLUSTER_E_SEQ_TORN;
        uint32_t h1 = __atomic_load_n(hi, __ATOMIC_ACQUIRE);
        uint32_t l1 = __atomic_load_n(lo, __ATOMIC_ACQUIRE);
        uint32_t h2 = __atomic_load_n(hi, __ATOMIC_ACQUIRE);
        uint32_t l2 = __atomic_load_n(lo, __ATOMIC_ACQUIRE);
        if (h1 == h2 && l1 == l2 && wcr1_seq_parity_ok(h1, l1)) {
            *out = wcr1_seq_from_halves(h1, l1);
            return WEFT_CLUSTER_OK;
        }
    }
}

/* Publish a (hi,lo) register pair: lo first (data half), hi last (COMMIT). */
int wcr1_seqreg_publish(const wcr1_transport_t *t, volatile uint32_t *hi,
                        volatile uint32_t *lo, uint64_t seq)
{
    const wcr1_transport_t *tr = t ? t : &WCR1_TRANSPORT_LOCAL;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!tr->store32((wcr1_transport_t *)tr, lo, (uint32_t)seq))
        return WEFT_CLUSTER_E_TRANSPORT;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!tr->store32((wcr1_transport_t *)tr, hi, wcr1_seq_hi_word(seq)))
        return WEFT_CLUSTER_E_TRANSPORT;
    return WEFT_CLUSTER_OK;
}

int wcr1_producer_seq_read(const wcr1_ring_view_t *view,
                           uint64_t max_retries, uint64_t *out_seq)
{
    if (!view || !out_seq) return WEFT_CLUSTER_E_ARG;
    return wcr1_seqreg_read(&view->hdr->seq_hi, &view->hdr->seq_lo,
                            max_retries, out_seq);
}

int wcr1_consumer_seq_read(const wcr1_ring_view_t *view,
                           uint64_t max_retries, uint64_t *out_seq)
{
    if (!view || !out_seq) return WEFT_CLUSTER_E_ARG;
    return wcr1_seqreg_read(&view->hdr->cons_hi, &view->hdr->cons_lo,
                            max_retries, out_seq);
}

// ---------------------------------------------------------------------------
// Geometry validation (shared by create + attach)
// ---------------------------------------------------------------------------

static int wcr1_validate_geometry(uint64_t capacity, uint64_t slot_size)
{
    if (capacity < WCR1_MIN_CAPACITY || capacity > WCR1_MAX_CAPACITY)
        return WEFT_CLUSTER_E_CAPACITY;
    if ((capacity & (capacity - 1)) != 0)
        return WEFT_CLUSTER_E_CAPACITY;
    if (slot_size < WCR1_MIN_SLOT_SIZE || (slot_size % 64) != 0)
        return WEFT_CLUSTER_E_SLOT_SIZE;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Ring create (HOME side) — identity cacheline + null consensus state
// ---------------------------------------------------------------------------

int wcr1_ring_create(const wcr1_cfg_t *cfg, uint32_t producer_node_id,
                     wcr1_ring_view_t *view, void **region_out,
                     uint64_t *region_len_out)
{
    if (!cfg || !view) return WEFT_CLUSTER_E_ARG;
    if (cfg->node_id == 0 || cfg->node_id > WCR1_MAX_NODES ||
        producer_node_id == 0 || producer_node_id > WCR1_MAX_NODES ||
        cfg->node_id == producer_node_id)
        return WEFT_CLUSTER_E_NODE_ID;

    int e = wcr1_validate_geometry(cfg->capacity_slots, cfg->slot_size);
    if (e) return e;

    uint64_t rlen = wcr1_region_size(cfg->capacity_slots, cfg->slot_size);
    uint8_t *region = (uint8_t *)wcr1_engine_alloc((size_t)rlen, 128);
    if (!region) return WEFT_CLUSTER_E_NOMEM;
    memset(region, 0, (size_t)rlen);

    /* Page-lock best-effort (Honesty: in containers without CAP_IPC_LOCK
       this fails; the ring still works, it just is not DMA-pinned. The
       RFC lists this as a deployment prerequisite, not an engine bug.) */
    (void)mlock(region, (size_t)rlen);

    wcr1_ring_header_t *h = (wcr1_ring_header_t *)region;

    /* Cacheline 0 — identity + geometry, LE-formatted, then CRC. */
    wcr1_le32_put(&h->magic, WCR1_MAGIC);
    h->layout_major = WCR1_LAYOUT_MAJOR;
    h->layout_minor = WCR1_LAYOUT_MINOR;
    wcr1_le32_put(&h->hdr_flags, 0);
    memcpy(h->cluster_id, cfg->cluster_id, 16);
    wcr1_le32_put(&h->home_node_id, cfg->node_id);
    wcr1_le32_put(&h->producer_node_id, producer_node_id);
    wcr1_le32_put(&h->config_crc32, wcr1_crc32(region, 0x20));
    wcr1_le32_put(&h->reserved0, 0);
    wcr1_le64_put(&h->epoch, cfg->epoch);
    wcr1_le64_put(&h->capacity_slots, cfg->capacity_slots);
    wcr1_le64_put(&h->slot_size, cfg->slot_size);

    /* Cacheline 1 — zeroed above: seq=0, watermark=0, term=0, no leader,
       null fencing token, heartbeat_seq=0 (even = stable null state). */

    view->hdr              = h;
    view->slots            = region + WCR1_HEADER_SIZE;
    view->capacity         = cfg->capacity_slots;
    view->slot_size        = cfg->slot_size;
    view->payload_capacity = cfg->slot_size - WCR1_SLOT_HEADER_SIZE;
    view->home_node        = cfg->node_id;
    view->producer_node    = producer_node_id;
    view->my_producer_seq  = 0;
    view->my_cursor        = 0;

    if (region_out)     *region_out = region;
    if (region_len_out) *region_len_out = rlen;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Attach validation ladder (Law 4 — distinct refusal per failure class)
// ---------------------------------------------------------------------------

int wcr1_ring_attach(void *region, uint64_t region_len,
                     const uint8_t cluster_id[16], uint64_t epoch,
                     uint32_t expected_home, uint32_t expected_producer,
                     uint64_t expected_capacity, uint64_t expected_slot_size,
                     wcr1_ring_view_t *view)
{
    if (!region || !view || !cluster_id) return WEFT_CLUSTER_E_ARG;
    if (((uintptr_t)region % 64) != 0)   return WEFT_CLUSTER_E_ALIGN;

    wcr1_ring_header_t *h = (wcr1_ring_header_t *)region;

    /* Ladder order: magic -> layout -> identity -> geometry -> state. */
    if (wcr1_le32_get(&h->magic) != WCR1_MAGIC)
        return WEFT_CLUSTER_E_MAGIC;
    if (h->layout_major != WCR1_LAYOUT_MAJOR ||
        h->layout_minor > WCR1_LAYOUT_MINOR)
        return WEFT_CLUSTER_E_LAYOUT;
    if (memcmp(h->cluster_id, cluster_id, 16) != 0)
        return WEFT_CLUSTER_E_CLUSTER_ID;
    if (wcr1_le32_get(&h->home_node_id) != expected_home ||
        wcr1_le32_get(&h->producer_node_id) != expected_producer)
        return WEFT_CLUSTER_E_NODE_ID;
    if (wcr1_le32_get(&h->config_crc32) != wcr1_crc32(region, 0x20))
        return WEFT_CLUSTER_E_CFG_CRC;
    if (wcr1_le64_get(&h->epoch) != epoch)
        return WEFT_CLUSTER_E_EPOCH;

    uint64_t cap  = wcr1_le64_get(&h->capacity_slots);
    uint64_t ssz  = wcr1_le64_get(&h->slot_size);
    int e = wcr1_validate_geometry(cap, ssz);
    if (e == WEFT_CLUSTER_E_CAPACITY || e == WEFT_CLUSTER_E_SLOT_SIZE)
        return e;
    if (cap != expected_capacity) return WEFT_CLUSTER_E_CAPACITY;
    if (ssz != expected_slot_size) return WEFT_CLUSTER_E_SLOT_SIZE;
    if (region_len != wcr1_region_size(cap, ssz))
        return WEFT_CLUSTER_E_REGION_SIZE;
    if (wcr1_le32_get(&h->cflags) & WCR1_CFLAG_NODE_EVICTED)
        return WEFT_CLUSTER_E_NODE_EVICTED;

    view->hdr              = h;
    view->slots            = (uint8_t *)region + WCR1_HEADER_SIZE;
    view->capacity         = cap;
    view->slot_size        = ssz;
    view->payload_capacity = ssz - WCR1_SLOT_HEADER_SIZE;
    view->home_node        = expected_home;
    view->producer_node    = expected_producer;

    /* Seed producer/consumer caches from the live registers (bounded). */
    uint64_t seq = 0;
    e = wcr1_producer_seq_read(view, 0, &seq);
    if (e) return e;
    view->my_producer_seq = seq;
    e = wcr1_consumer_seq_read(view, 0, &seq);
    if (e) return e;
    view->my_cursor = seq;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Publish — two-store commit (RFC 0018 §4)
// ---------------------------------------------------------------------------

int wcr1_publish(wcr1_ring_view_t *view, const wcr1_transport_t *transport,
                 const void *payload, uint32_t payload_bytes, uint32_t flags,
                 uint64_t now_ns, uint64_t fencing_token, uint64_t epoch,
                 uint64_t *out_seq)
{
    if (!view || !view->hdr) return WEFT_CLUSTER_E_ARG;
    if (payload_bytes > view->payload_capacity) return WEFT_CLUSTER_E_PAYLOAD;
    if (payload_bytes > 0 && !payload) return WEFT_CLUSTER_E_ARG;
    const wcr1_transport_t *tr = transport ? transport : &WCR1_TRANSPORT_LOCAL;

    wcr1_ring_header_t *h = view->hdr;

    /* 1. Committed watermark (our own prior commits — single producer). */
    uint64_t committed = 0;
    int e = wcr1_producer_seq_read(view, 0, &committed);
    if (e) return e;
    if (view->my_producer_seq > committed) committed = view->my_producer_seq;
    uint64_t new_seq = committed + 1;

    /* 2. Backpressure: refuse rather than overwrite unconsumed data.
          Watermark read is advisory — a torn read falls back to the last
          known value (worst case: over-conservative refusal). */
    uint64_t consumed = 0;
    if (wcr1_consumer_seq_read(view, 0, &consumed) != WEFT_CLUSTER_OK)
        consumed = view->my_cursor;   /* last known watermark (cached) */
    uint64_t lag = committed - (consumed <= committed ? consumed : committed);
    if (lag >= view->capacity) {
        if (out_seq) *out_seq = committed;
        return WEFT_CLUSTER_E_BACKPRESSURE;
    }

    /* 3. Slot addressing — pure arithmetic, zero roundtrips. */
    uint64_t slot_off = wcr1_slot_byte_offset(new_seq, view->capacity,
                                              view->slot_size);
    uint8_t *slot     = (uint8_t *)view->hdr + slot_off;

    uint32_t slot_flags = flags & (0xFFFFu);
    uint64_t lag_after  = lag + 1;
    if (lag_after >= view->capacity / WCR1_BP_HIGH_WATER_DIV)
        slot_flags |= WCR1_SLOT_F_BP_MARK;

    /* 4. Build the slot header on the stack (Law 1: no allocation), then
          ship it through the transport as ONE 64-byte write — every remote
          write traverses the seam (the partition gate and Engineer 2's
          ibverbs layer see the whole header, never torn direct stores).
          Nothing is visible until the commit register advances. */
    wcr1_slot_header_t sh;
    memset(&sh, 0, sizeof sh);
    uint32_t with_crc = (flags & WCR1_PUBLISH_F_WITH_CRC) ? 1u : 0u;
    wcr1_le64_put(&sh.message_seq, new_seq);
    wcr1_le64_put(&sh.timestamp_ns, now_ns);
    wcr1_le64_put(&sh.fencing_token, fencing_token);
    wcr1_le64_put(&sh.producer_epoch, epoch);
    wcr1_le32_put(&sh.payload_bytes, payload_bytes);
    wcr1_le32_put(&sh.flags, slot_flags);
    wcr1_le64_put(&sh.bp_lag, lag_after);
    wcr1_le32_put(&sh.payload_crc32,
                  with_crc ? wcr1_crc32(payload, payload_bytes) : 0);
    wcr1_le32_put(&sh.producer_node_id, wcr1_le64_get(&h->producer_node_id) & 0xFFFFFFFFu);
    wcr1_le32_put(&sh.reserved0, 0);
    if (with_crc)
        wcr1_le32_put(&sh.header_crc32, wcr1_crc32(&sh, 0x34));
    else
        wcr1_le32_put(&sh.header_crc32, 0);

    if (!tr->store_buf((wcr1_transport_t *)tr, slot, &sh, sizeof sh))
        return WEFT_CLUSTER_E_TRANSPORT;
    if (payload_bytes > 0 && !tr->store_buf((wcr1_transport_t *)tr,
                                            slot + WCR1_SLOT_HEADER_SIZE,
                                            payload, payload_bytes))
        return WEFT_CLUSTER_E_TRANSPORT;

    /* 6. TWO-STORE COMMIT: release fence, seq_lo, then seq_hi (final). */
    e = wcr1_seqreg_publish(tr, &h->seq_hi, &h->seq_lo, new_seq);
    if (e) return e;

    view->my_producer_seq = new_seq;
    if (out_seq) *out_seq = new_seq;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Acquire — wait-free, bounded retry, tear + overrun + alias detection
// ---------------------------------------------------------------------------

int wcr1_acquire_next(wcr1_ring_view_t *view, uint64_t max_retries,
                      wcr1_slot_view_t *out)
{
    if (!view || !view->hdr || !out) return WEFT_CLUSTER_E_ARG;

    uint64_t committed = 0;
    int e = wcr1_producer_seq_read(view, max_retries, &committed);
    if (e) return e;

    uint64_t want = view->my_cursor + 1;
    if (committed < want) return WEFT_CLUSTER_E_NOT_PUBLISHED;
    if (committed - want >= view->capacity) return WEFT_CLUSTER_E_SEQ_OVERRUN;

    uint64_t   slot_off = wcr1_slot_byte_offset(want, view->capacity,
                                                view->slot_size);
    const uint8_t *slot = (const uint8_t *)view->hdr + slot_off;
    const wcr1_slot_header_t *sh = (const wcr1_slot_header_t *)slot;

    /* Acquire-order load pairs with the producer's release-fenced commit. */
    uint64_t msg_seq = wcr1_le64_get(&sh->message_seq);
    if (msg_seq == want) {
        /* fresh slot */
    } else if (view->capacity < want && msg_seq + view->capacity == want) {
        /* slot already recycled past us (TOCTOU wrap) */
        return WEFT_CLUSTER_E_SEQ_OVERRUN;
    } else if (msg_seq == 0 && want == 1) {
        return WEFT_CLUSTER_E_SEQ_CORRUPT;   /* never published */
    } else {
        return WEFT_CLUSTER_E_SEQ_CORRUPT;
    }

    out->hdr           = sh;
    out->payload       = slot + WCR1_SLOT_HEADER_SIZE;
    out->payload_bytes = wcr1_le32_get(&sh->payload_bytes);
    out->seq           = want;
    out->fencing_token = wcr1_le64_get(&sh->fencing_token);
    out->flags         = wcr1_le32_get(&sh->flags);
    if (out->payload_bytes > view->payload_capacity)
        return WEFT_CLUSTER_E_SEQ_CORRUPT;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Consume — home-node watermark commit (two-store, cursor advance)
// ---------------------------------------------------------------------------

int wcr1_consume(wcr1_ring_view_t *view, uint64_t seq)
{
    if (!view || !view->hdr) return WEFT_CLUSTER_E_ARG;
    if (seq < view->my_cursor) return WEFT_CLUSTER_E_ARG;

    /* Release: the consumer's payload reads happen-before the watermark
       commit the producer will acquire-load before overwriting. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wcr1_ring_header_t *h = view->hdr;
    __atomic_store_n(&h->cons_lo, (uint32_t)seq, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&h->cons_hi, wcr1_seq_hi_word(seq), __ATOMIC_RELEASE);

    view->my_cursor = seq;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Opt-in slot integrity (Law 4: named refusal, never silent)
// ---------------------------------------------------------------------------

int wcr1_slot_crc_check(const wcr1_slot_view_t *view)
{
    if (!view || !view->hdr) return WEFT_CLUSTER_E_ARG;
    if (!(view->flags & WCR1_SLOT_F_WITH_CRC)) return WEFT_CLUSTER_OK;

    const uint8_t *slot = (const uint8_t *)view->hdr;
    uint32_t pc = wcr1_crc32(view->payload, (size_t)view->payload_bytes);
    if (pc != wcr1_le32_get(&view->hdr->payload_crc32))
        return WEFT_CLUSTER_E_CRC;
    if (wcr1_crc32(slot, 0x34) != wcr1_le32_get(&view->hdr->header_crc32))
        return WEFT_CLUSTER_E_CRC;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// Deterministic election timeout (RFC 0018 §6.2)
// ---------------------------------------------------------------------------

uint64_t wcr1_election_timeout_ns(uint32_t node_id, uint64_t lease_ns)
{
    /* 2x lease + node-id staggered jitter (deterministic, no RNG):
       nodes check in different order every election window, breaking
       dueling-candidate symmetry without random timers. */
    uint64_t stagger = (uint64_t)(node_id % 8u) * (lease_ns / 4u);
    return 2u * lease_ns + stagger;
}
