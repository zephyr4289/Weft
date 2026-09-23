// weft_shm_inspector.c — the live shared-memory attach engine (Pillar 7).
//
// Implementation notes (the WHY behind each mechanism lives in the header;
// this block records the wire facts the parser leans on):
//   WFRM ring      : header@0 (128B, public rmw_ring_header_t), ctrl@128
//                    (64B, rmw_ring_ctrl_t), slots@4096 stride
//                    (64+payload+63)&~63, payload at slot+64.
//   WFRR registry  : page0 4096B (rmw_registry_header_t, header_size=4096),
//                    topics at +4096, count from header->max_topics, the
//                    file size must equal 4096 + max_topics*sizeof(topic).
//   WFSH session   : header 64B (magic/version/hdr_size/flags@8/payload@12/
//                    slot_count@16/ring_bytes@20 u64 UNALIGNED — memcpy/
//                    creator_pid@28/created@32/reserved@40 zero), ring at
//                    +64: latestSeq@0, publishes@8, slotSeq[k]@16+8k.
//   WFRE registry  : fixed 9984B, canonical name only (the entry layout is
//                    private to weft_ipc.c — we discover through the real
//                    API, never a re-declaration).
//
// Every load below is either acquire-fenced (control words, seqlock
// brackets) or deliberately relaxed (advisory counters), and NO code path
// in this file stores to a shared mapping, takes a lock, or parks.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* fstatat, shm_open under strict C11, DT_* */
#endif

#include "weft_inspector/weft_shm_inspector.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* the REAL cluster registry API (linked read-only; layout is private) */
#include "weft_ipc.h"

/* ------------------------------------------------------------------ */
/* common belt                                                          */
/* ------------------------------------------------------------------ */

int64_t weft_inspect_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

int weft_inspect_arena_init(weft_inspect_arena_t *a, void *base, size_t cap) {
    if (a == NULL || base == NULL) return WEFT_INSPECT_ERR_INVALID;
    if (((uintptr_t)base & 7u) != 0u) return WEFT_INSPECT_ERR_INVALID;
    a->base = (uint8_t *)base;
    a->cap = cap;
    a->used = 0;
    return WEFT_INSPECT_OK;
}

void *weft_inspect_arena_alloc(weft_inspect_arena_t *a, size_t bytes) {
    if (a == NULL) return NULL;
    size_t need = (bytes + 7u) & ~(size_t)7u;
    if (need > a->cap - a->used) return NULL;  /* exhausted: honest NULL */
    void *p = a->base + a->used;
    a->used += need;
    return p;
}

size_t weft_inspect_arena_free(const weft_inspect_arena_t *a) {
    return (a == NULL) ? 0u : a->cap - a->used;
}

uintptr_t weft_inspect_line_base(const void *p) {
    return ((uintptr_t)p) & ~(uintptr_t)(WEFT_INSPECT_CACHE_LINE - 1u);
}

int weft_inspect_same_line(const void *a, const void *b) {
    return weft_inspect_line_base(a) == weft_inspect_line_base(b);
}

/* ------------------------------------------------------------------ */
/* internal helpers                                                     */
/* ------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* atomic views over the WFSH ring ctrl words (base+64, 8-aligned) */
static const _Atomic uint64_t *wfsh_latest(const weft_inspect_segment_t *s) {
    return (const _Atomic uint64_t *)(const void *)(s->base + 64u);
}

static const _Atomic uint64_t *wfsh_publishes(const weft_inspect_segment_t *s) {
    return (const _Atomic uint64_t *)(const void *)(s->base + 64u + 8u);
}

static const rmw_ring_ctrl_t *wfrm_ctrl(const weft_inspect_segment_t *s) {
    return (const rmw_ring_ctrl_t *)(const void *)(s->base +
                                                   RMW_WEFT_RING_HEADER_BYTES);
}

static const rmw_ring_slot_t *wfrm_slot(const weft_inspect_segment_t *s,
                                        uint64_t absolute_idx) {
    uint64_t k = absolute_idx & (uint64_t)(s->slot_count - 1u);
    return (const rmw_ring_slot_t *)(const void *)(s->base +
            RMW_WEFT_RING_SLOTS_OFFSET + (size_t)k * s->slot_stride);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

int weft_inspect_init(weft_inspect_ctx_t *ctx,
                      weft_inspect_segment_t *table, unsigned cap) {
    if (ctx == NULL || table == NULL || cap == 0u) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->segs = table;
    ctx->cap = cap;
    ctx->count = 0u;
    return WEFT_INSPECT_OK;
}

void weft_inspect_destroy(weft_inspect_ctx_t *ctx) {
    if (ctx == NULL) return;
    for (unsigned i = 0; i < ctx->count; i++) {
        weft_inspect_segment_t *s = &ctx->segs[i];
        if (s->attached && s->base != NULL) {
            (void)munmap(s->base, s->map_bytes);
        }
        if (s->fd >= 0) (void)close(s->fd);
        memset(s, 0, sizeof *s);
        s->fd = -1;
    }
    ctx->count = 0u;
}

int weft_inspect_find(const weft_inspect_ctx_t *ctx, const char *name) {
    if (ctx == NULL || name == NULL) return -1;
    for (unsigned i = 0; i < ctx->count; i++) {
        if (strncmp(ctx->segs[i].name, name, WEFT_INSPECT_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int weft_inspect_detach(weft_inspect_ctx_t *ctx, unsigned idx) {
    if (ctx == NULL || idx >= ctx->count) return WEFT_INSPECT_ERR_INVALID;
    weft_inspect_segment_t *s = &ctx->segs[idx];
    if (s->attached && s->base != NULL) (void)munmap(s->base, s->map_bytes);
    if (s->fd >= 0) (void)close(s->fd);
    /* compact: move the tail into the hole — indices shift, by contract */
    if (idx + 1u < ctx->count) {
        ctx->segs[idx] = ctx->segs[ctx->count - 1u];
    }
    memset(&ctx->segs[ctx->count - 1u], 0,
           sizeof(ctx->segs[ctx->count - 1u]));
    ctx->segs[ctx->count - 1u].fd = -1;
    ctx->count--;
    return WEFT_INSPECT_OK;
}

/* ------------------------------------------------------------------ */
/* attach: open O_RDONLY, map PROT_READ, validate the family contract    */
/* ------------------------------------------------------------------ */

static int wfrm_validate(const uint8_t *base, size_t bytes,
                         weft_inspect_segment_t *s) {
    const rmw_ring_header_t *h = (const rmw_ring_header_t *)(const void *)base;
    if (h->magic != RMW_WEFT_RING_MAGIC) return -1;
    if (h->version != RMW_WEFT_RING_VERSION) return -1;
    if (h->header_size != RMW_WEFT_RING_HEADER_BYTES) return -1;
    if (h->slot_count == 0u || (h->slot_count & (h->slot_count - 1u)) != 0u) {
        return -1;  /* power of two, by ring contract */
    }
    if (h->payload_bytes == 0u) return -1;
    uint32_t stride = (uint32_t)(((uint64_t)RMW_WEFT_SLOT_HDR_BYTES +
                                  h->payload_bytes + 63u) & ~63ull);
    if (h->slot_stride != stride) return -1;
    if (h->mapping_bytes != (uint64_t)bytes) return -1;
    if ((uint64_t)bytes < (uint64_t)RMW_WEFT_RING_SLOTS_OFFSET +
                              (uint64_t)h->slot_count * stride) {
        return -1;
    }
    for (size_t i = 0; i < sizeof h->reserved; i++) {
        if (h->reserved[i] != 0u) return -1;  /* unknown bits reject */
    }
    s->slot_count = h->slot_count;
    s->payload_bytes = h->payload_bytes;
    s->slot_stride = stride;
    s->mapping_bytes_hdr = h->mapping_bytes;
    s->creator_pid = h->creator_pid;
    s->created_unix_ns = h->created_unix_ns;
    s->sub_instance = h->sub_instance;
    s->reliability = h->reliability;
    return 0;
}

static int wfrr_validate(const uint8_t *base, size_t bytes,
                         weft_inspect_segment_t *s) {
    const rmw_registry_header_t *h =
        (const rmw_registry_header_t *)(const void *)base;
    if (h->magic != RMW_WEFT_REGISTRY_MAGIC) return -1;
    if (h->version != RMW_WEFT_REGISTRY_VERSION) return -1;
    if (h->header_size != 4096u) return -1;
    if (h->max_topics == 0u || h->max_topics > 4096u) return -1;
    size_t expect = 4096u + (size_t)h->max_topics * sizeof(rmw_registry_topic_t);
    if (bytes != expect) return -1;
    s->slot_count = 0u;
    s->reg_topics_active = 0u;
    return 0;
}

static int wfsh_validate(const uint8_t *base, size_t bytes,
                         weft_inspect_segment_t *s) {
    if (bytes < 64u) return -1;
    if (rd_u32(base + 0) != 0x48534657u) return -1;       /* "WFSH" */
    uint16_t ver, hsize;
    memcpy(&ver, base + 4, sizeof ver);
    memcpy(&hsize, base + 6, sizeof hsize);
    if (ver != 1u) return -1;                             /* version  */
    if (hsize != 64u) return -1;                          /* hdr size */
    if (rd_u32(base + 8) != 0u) return -1;                /* flags    */
    for (size_t i = 40; i < 64; i++) {
        if (base[i] != 0u) return -1;                     /* reserved */
    }
    uint32_t payload = rd_u32(base + 12);
    uint32_t slots = rd_u32(base + 16);
    uint64_t ring_bytes = rd_u64(base + 20);
    if (payload == 0u || (payload & 3u) != 0u) return -1;
    if (slots == 0u || slots > (1u << 24)) return -1;
    uint64_t expect_ring = 16u + (uint64_t)slots * 8u + (uint64_t)slots * payload;
    if (ring_bytes != expect_ring) return -1;
    if ((uint64_t)bytes != 64u + ring_bytes) return -1;
    s->slot_count = slots;
    s->payload_bytes = payload;
    s->slot_stride = 0u;               /* WFSH: payload-packed, no stride */
    s->ring_bytes_hdr = ring_bytes;
    s->creator_pid = rd_u32(base + 28);
    s->created_unix_ns = rd_u64(base + 32);
    return 0;
}

/* classify an open read-only mapping by magic; on success the segment is
 * fully attached (validated geometry + live mapping kept). The fd is
 * pre-opened by the caller (scan() opens by name; attach_fd() takes a
 * studio-held fd) — on failure the fd is closed here. */
static int classify_fd(const char *name, int fd, weft_inspect_segment_t *s) {
    uint8_t *base = NULL;
    size_t bytes = 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        (void)close(fd);
        return -1;
    }
    bytes = (size_t)st.st_size;
    void *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        (void)close(fd);
        return -1;
    }
    base = (uint8_t *)p;
    memset(s, 0, sizeof *s);
    s->fd = -1;
    snprintf(s->name, sizeof s->name, "%s", name);
    memcpy(s->probe_magic, base, 4);

    uint32_t magic = rd_u32(base);
    int ok = -1;
    if (magic == RMW_WEFT_RING_MAGIC) {
        s->family = WEFT_INSPECT_FAMILY_RMW_RING;
        ok = wfrm_validate(base, bytes, s);
    } else if (magic == RMW_WEFT_REGISTRY_MAGIC) {
        s->family = WEFT_INSPECT_FAMILY_RMW_REGISTRY;
        ok = wfrr_validate(base, bytes, s);
    } else if (magic == 0x48534657u) {  /* "WFSH" */
        s->family = WEFT_INSPECT_FAMILY_CLUSTER_RING;
        ok = wfsh_validate(base, bytes, s);
    } else if (magic == 0x45524657u) {  /* "WFRE" */
        s->family = WEFT_INSPECT_FAMILY_CLUSTER_REGISTRY;
        /* the entry layout is private to weft_ipc.c: only the canonical
         * object is discoverable through the real API */
        if (strcmp(name, "weft_registry_v1") == 0 && bytes == 9984u) {
            ok = 0;
        } else {
            ok = -1;
        }
    } else {
        s->family = WEFT_INSPECT_FAMILY_UNKNOWN;
        s->excluded = WEFT_INSPECT_EXCL_ABI;  /* flagged, never guessed */
        ok = 1;  /* recorded as UNKNOWN — no further validation */
    }

    if (ok == 0) {
        s->attached = 1u;
        s->base = base;
        s->map_bytes = bytes;
        s->fd = fd;
        return 0;
    }
    if (ok == 1) {  /* UNKNOWN: keep the record, release the mapping */
        (void)munmap(base, bytes);
        (void)close(fd);
        return 0;
    }
    /* family-known but contract-broken: flag ABI, release the mapping */
    s->excluded = WEFT_INSPECT_EXCL_ABI;
    s->attached = 0u;
    (void)munmap(base, bytes);
    (void)close(fd);
    return 0;
}

/* scan()'s by-name wrapper: open read-only, then classify. */
static int classify_and_attach(const char *name,
                               weft_inspect_segment_t *s) {
    char path[WEFT_INSPECT_NAME_MAX + 2];
    if (snprintf(path, sizeof path, "/%s", name) < 0) return -1;
    int fd = shm_open(path, O_RDONLY, 0400);
    if (fd < 0) {
        return -1;  /* vanished mid-scan or unreadable: skip silently, the
                     * producer may be mid-create; next scan retries */
    }
    return classify_fd(name, fd, s);
}

/* ------------------------------------------------------------------ */
/* scan                                                                 */
/* ------------------------------------------------------------------ */

int weft_inspect_scan(weft_inspect_ctx_t *ctx) {
    if (ctx == NULL) return WEFT_INSPECT_ERR_INVALID;

    /* existing entries start unfound; a scan re-validates liveness */
    for (unsigned i = 0; i < ctx->count; i++) {
        ctx->segs[i].scan_found = 0u;
    }

    DIR *d = opendir("/dev/shm");
    if (d == NULL) return WEFT_INSPECT_ERR_SYS;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "weft_", 5) != 0) continue;
        size_t nlen = strlen(e->d_name);
        if (nlen == 0u || nlen >= WEFT_INSPECT_NAME_MAX) {
            ctx->scan_overflow++;  /* not a house name; counted, skipped */
            continue;
        }
        int existing = weft_inspect_find(ctx, e->d_name);
        if (existing >= 0) {
            ctx->segs[existing].scan_found = 1u;  /* still present */
            continue;
        }
        if (ctx->count >= ctx->cap) {
            ctx->scan_overflow++;
            continue;
        }
        weft_inspect_segment_t fresh;
        if (classify_and_attach(e->d_name, &fresh) == 0) {
            ctx->segs[ctx->count++] = fresh;
            ctx->segs[ctx->count - 1u].scan_found = 1u;
        }
        /* classify_and_attach < 0: object vanished mid-scan — a live mesh
         * is allowed to destroy segments; the next scan re-attempts. */
    }
    (void)closedir(d);

    /* prune entries whose object vanished (unlinked by its owner) */
    unsigned w = 0u;
    for (unsigned i = 0; i < ctx->count; i++) {
        if (ctx->segs[i].scan_found != 0u) {
            if (w != i) ctx->segs[w] = ctx->segs[i];
            w++;
        } else {
            weft_inspect_segment_t *s = &ctx->segs[i];
            if (s->attached && s->base != NULL) (void)munmap(s->base, s->map_bytes);
            if (s->fd >= 0) (void)close(s->fd);
        }
    }
    for (unsigned i = w; i < ctx->count; i++) {
        memset(&ctx->segs[i], 0, sizeof ctx->segs[i]);
        ctx->segs[i].fd = -1;
    }
    ctx->count = w;
    return (int)ctx->count;
}

int weft_inspect_attach_fd(weft_inspect_ctx_t *ctx, int fd,
                           const char *name) {
    if (ctx == NULL || fd < 0 || name == NULL) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    if (weft_inspect_find(ctx, name) >= 0) {
        return WEFT_INSPECT_ERR_FULL;   /* name already watched */
    }
    if (ctx->count >= ctx->cap) return WEFT_INSPECT_ERR_FULL;
    weft_inspect_segment_t fresh;
    if (classify_fd(name, fd, &fresh) != 0) {
        return WEFT_INSPECT_ERR_SYS;    /* fd is closed by classify_fd */
    }
    ctx->segs[ctx->count++] = fresh;
    return (int)(ctx->count - 1u);
}

/* ------------------------------------------------------------------ */
/* scrape: one snapshot pass over every attached ring                   */
/* ------------------------------------------------------------------ */

int weft_inspect_scrape(weft_inspect_ctx_t *ctx) {
    if (ctx == NULL) return WEFT_INSPECT_ERR_INVALID;
    for (unsigned i = 0; i < ctx->count; i++) {
        weft_inspect_segment_t *s = &ctx->segs[i];
        if (!s->attached || s->base == NULL) continue;

        if (s->family == WEFT_INSPECT_FAMILY_RMW_RING) {
            const rmw_ring_ctrl_t *c = wfrm_ctrl(s);
            /* bracket on head: head is bumped AFTER the commit release
             * fence, so an unchanged head across our reads means no
             * commit interleaved — a consistent control snapshot. */
            for (int attempt = 0; attempt < 2; attempt++) {
                uint64_t h1 = atomic_load_explicit(&c->head,
                                                   memory_order_acquire);
                uint64_t tail = atomic_load_explicit(&c->tail_ack,
                                                     memory_order_acquire);
                uint64_t pub = atomic_load_explicit(&c->published_total,
                                                    memory_order_acquire);
                uint64_t drop = atomic_load_explicit(&c->dropped_total,
                                                     memory_order_acquire);
                uint32_t bell = atomic_load_explicit(&c->doorbell,
                                                     memory_order_relaxed);
                uint32_t wait = atomic_load_explicit(&c->waiters,
                                                     memory_order_relaxed);
                uint32_t st = atomic_load_explicit(&c->state,
                                                   memory_order_acquire);
                uint64_t h2 = atomic_load_explicit(&c->head,
                                                   memory_order_acquire);
                if (h1 == h2) {
                    s->head = h1;
                    s->tail_ack = tail;
                    s->published_total = pub;
                    s->dropped_total = drop;
                    s->doorbell = bell;
                    s->waiters = wait;
                    s->ring_state = st;
                    s->soft_snapshot = 0u;
                    break;
                }
                ctx->torn_ctrl_retries++;
                if (attempt == 1) {
                    /* still moving at 10M msg/s: record the best-effort
                     * words and MARK the snapshot soft (advisory) — the
                     * honest alternative to blocking or spinning. */
                    s->head = h2;
                    s->tail_ack = tail;
                    s->published_total = pub;
                    s->dropped_total = drop;
                    s->doorbell = bell;
                    s->waiters = wait;
                    s->ring_state = st;
                    s->soft_snapshot = 1u;
                    ctx->soft_snapshots++;
                }
            }
        } else if (s->family == WEFT_INSPECT_FAMILY_CLUSTER_RING) {
            const _Atomic uint64_t *lat = wfsh_latest(s);
            const _Atomic uint64_t *pub = wfsh_publishes(s);
            for (int attempt = 0; attempt < 2; attempt++) {
                uint64_t l1 = atomic_load_explicit(lat, memory_order_acquire);
                uint64_t p = atomic_load_explicit(pub, memory_order_acquire);
                uint64_t l2 = atomic_load_explicit(lat, memory_order_acquire);
                if (l1 == l2) {
                    s->latest_seq = l1;
                    s->publishes = p;
                    s->soft_snapshot = 0u;
                    break;
                }
                ctx->torn_ctrl_retries++;
                if (attempt == 1) {
                    s->latest_seq = l2;
                    s->publishes = p;
                    s->soft_snapshot = 1u;
                    ctx->soft_snapshots++;
                }
            }
        }
        /* registries: census counters refresh on topology passes only */
    }
    ctx->scrape_passes++;
    return WEFT_INSPECT_OK;
}

/* ------------------------------------------------------------------ */
/* read-only peek seam                                                  */
/* ------------------------------------------------------------------ */

int weft_inspect_peek_slot(const weft_inspect_ctx_t *ctx, unsigned idx,
                           uint64_t cursor, weft_inspect_slot_meta_t *out) {
    if (ctx == NULL || out == NULL || idx >= ctx->count) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    const weft_inspect_segment_t *s = &ctx->segs[idx];
    if (s->family != WEFT_INSPECT_FAMILY_RMW_RING || !s->attached) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    const rmw_ring_ctrl_t *c = wfrm_ctrl(s);
    uint64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
    if (cursor >= head) return 0;  /* not yet published at observation time */

    /* cast away const for the ctx telemetry counters: the CONTEXT is a
     * single-threaded studio-owned object; the SEGMENT mapping is never
     * written. (peek runs on the const api surface; counters mutate.) */
    weft_inspect_ctx_t *cx = (weft_inspect_ctx_t *)ctx;

    const rmw_ring_slot_t *slot = wfrm_slot(s, cursor);
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t v1 = atomic_load_explicit(&slot->version,
                                           memory_order_acquire);
        cx->peek_attempts++;
        if ((v1 & 1u) == 0u) {
            /* even: the loan invariant protects committed slots (the
             * writer cannot touch cursor until tail_ack passes it), so a
             * plain field read here is safe; the double version read is
             * the crash/tear tripwire (defense in depth, same as the
             * subscriber's own take path). */
            uint64_t seq = slot->seq_id;
            uint32_t size = slot->payload_size;
            uint32_t crc = slot->payload_crc;
            uint64_t ts = slot->source_ts_unix_ns;
#ifndef __SANITIZE_THREAD__
            /* hardware acquire fence; TSan's virtual machine tracks
             * happens-before via its atomic interceptors and warns on
             * inlined acquire fences — skip it under sanitization */
            atomic_thread_fence(memory_order_acquire);
#endif
            uint64_t v2 = atomic_load_explicit(&slot->version,
                                               memory_order_acquire);
            if (v1 == v2) {
                out->version = v1;
                out->seq_id = seq;
                out->payload_size = size;
                out->payload_crc = crc;
                out->source_ts_unix_ns = ts;
                out->observed_ns = weft_inspect_now_ns();
                out->recycled = (seq != cursor) ? 1u : 0u;
                return 1;
            }
        }
    }
    cx->peek_torn++;
    return WEFT_INSPECT_ERR_AGAIN;
}

int weft_inspect_slot_payload_ro(const weft_inspect_ctx_t *ctx,
                                 unsigned idx, uint64_t cursor,
                                 const uint8_t **payload_out,
                                 const uint64_t **version_out,
                                 uint32_t *size_out) {
    if (ctx == NULL || payload_out == NULL || version_out == NULL ||
        size_out == NULL || idx >= ctx->count) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    const weft_inspect_segment_t *s = &ctx->segs[idx];
    if (s->family != WEFT_INSPECT_FAMILY_RMW_RING || !s->attached) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    const rmw_ring_ctrl_t *c = wfrm_ctrl(s);
    uint64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
    if (cursor >= head) return 0;

    const rmw_ring_slot_t *slot = wfrm_slot(s, cursor);
    uint64_t v = atomic_load_explicit(&slot->version,
                                      memory_order_acquire);
    if (v & 1u) return WEFT_INSPECT_ERR_AGAIN;  /* mid-commit: retry later */

    *payload_out = (const uint8_t *)slot + RMW_WEFT_SLOT_HDR_BYTES;
    *version_out = (const uint64_t *)(const void *)&slot->version;
    *size_out = slot->payload_size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* topology                                                             */
/* ------------------------------------------------------------------ */

static void census_rmw_registry(weft_inspect_segment_t *s,
                                const weft_inspect_ctx_t *ctx,
                                weft_inspect_topic_row_t *rows,
                                unsigned row_cap, unsigned *rows_written,
                                weft_inspect_topology_t *t) {
    const rmw_registry_header_t *h =
        (const rmw_registry_header_t *)(const void *)s->base;
    const rmw_registry_topic_t *topics =
        (const rmw_registry_topic_t *)(const void *)(s->base + 4096u);
    uint32_t topics_active = 0u, pubs = 0u, subs = 0u;

    for (uint32_t i = 0; i < h->max_topics; i++) {
        const rmw_registry_topic_t *tp = &topics[i];
        uint32_t st = atomic_load_explicit(&tp->state, memory_order_acquire);
        if (st != RMW_WEFT_SLOT_ACTIVE) continue;
        topics_active++;
        weft_inspect_topic_row_t row;
        memset(&row, 0, sizeof row);
        snprintf(row.topic, sizeof row.topic, "%s", tp->name);
        row.type_hash = tp->type_hash;
        row.msg_size = tp->msg_size;

        for (uint32_t p = 0; p < RMW_WEFT_MAX_PUBS_PER_TOPIC; p++) {
            uint32_t ps = atomic_load_explicit(&tp->pubs[p].state,
                                               memory_order_acquire);
            if (ps == RMW_WEFT_SLOT_ACTIVE) {
                row.pubs++;
                pubs++;
            }
        }
        for (uint32_t sb = 0; sb < RMW_WEFT_MAX_SUBS_PER_TOPIC; sb++) {
            const rmw_registry_sub_record_t *sr = &tp->subs[sb];
            uint32_t ss = atomic_load_explicit(&sr->state,
                                               memory_order_acquire);
            if (ss == RMW_WEFT_SLOT_ACTIVE) {
                row.subs++;
                subs++;
                /* the sub record's ring may be attached in OUR table */
                if (weft_inspect_find(ctx, sr->ring_name) >= 0) {
                    row.rings_mapped++;
                }
            }
        }
        if (rows != NULL && *rows_written < row_cap) {
            rows[(*rows_written)++] = row;
        }
    }
    s->reg_topics_active = topics_active;
    s->reg_pubs_active = pubs;
    s->reg_subs_active = subs;
    t->rmw_topics_active += topics_active;
    t->rmw_pubs_active += pubs;
    t->rmw_subs_active += subs;
}

int weft_inspect_topology(const weft_inspect_ctx_t *ctx,
                          weft_inspect_topic_row_t *rows, unsigned row_cap,
                          weft_inspect_topology_t *out) {
    if (ctx == NULL || out == NULL) return WEFT_INSPECT_ERR_INVALID;
    memset(out, 0, sizeof *out);
    out->observed_ns = weft_inspect_now_ns();
    unsigned rows_written = 0u;

    for (unsigned i = 0; i < ctx->count; i++) {
        weft_inspect_segment_t *s = &ctx->segs[i];
        if (!s->attached || s->base == NULL) {
            if (s->family == WEFT_INSPECT_FAMILY_UNKNOWN) {
                out->unknown_flagged++;
            }
            continue;
        }
        switch (s->family) {
        case WEFT_INSPECT_FAMILY_RMW_RING:
        case WEFT_INSPECT_FAMILY_CLUSTER_RING:
            out->rings_watched++;
            out->ring_payload_bytes +=
                (uint64_t)s->slot_count * (uint64_t)s->payload_bytes;
            break;
        case WEFT_INSPECT_FAMILY_RMW_REGISTRY: {
            census_rmw_registry(s, ctx, rows, row_cap, &rows_written, out);
            out->rmw_registries++;
            break;
        }
        case WEFT_INSPECT_FAMILY_CLUSTER_REGISTRY: {
            weft_ipc_registry_t reg;
            if (weft_ipc_registry_open(&reg, 0 /*no create*/, 1 /*RO*/) == 0) {
                weft_ipc_discovered_t disc[64];
                int n = weft_ipc_discover(&reg, 0 /*no stale marking*/,
                                          NULL, disc, 64);
                if (n >= 0) {
                    out->cluster_sessions += (uint32_t)n;
                }
                weft_ipc_registry_close(&reg);
            }
            out->cluster_registries++;
            break;
        }
        default:
            out->unknown_flagged++;
            break;
        }
    }
    if (rows != NULL && rows_written > 0u) {
        /* rows are written in table order; nothing further to sort */
    }
    return (int)rows_written;
}
