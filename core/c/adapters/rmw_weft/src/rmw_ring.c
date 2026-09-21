// rmw_ring.c — seqlock SPSC loan-ring engine (see rmw_ring.h for the ABI).
//
// Implementation notes that reviewers care about:
//   * Every shared word is a C11 _Atomic accessed with an EXPLICIT order;
//     the seqlock write path uses the two-fence form (odd store -> release
//     fence -> payload -> release fence -> even store), the ARM/SVE2-safe
//     equivalent of Linux's smp_wmb() pair.
//   * The steady state makes ZERO syscalls: no futex unless a waiter has
//     published itself (waiters != 0), no clock reads on the fast fail.
//   * Deadlines are CLOCK_MONOTONIC absolute nanoseconds; every ladder
//     rung re-checks the deadline (Law 3 — no unbounded blocking).

#include "rmw_weft/rmw_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* futex op codes (stable Linux UAPI values; declared locally to keep the
 * pedantic C11 TU free of kernel-header drag). */
#define RMW_FUTEX_WAIT 0
#define RMW_FUTEX_WAKE 1

uint8_t *rmw_ring_slot_payload(rmw_ring_slot_t *slot) {
    return (uint8_t *)slot + RMW_WEFT_SLOT_HDR_BYTES;
}

static inline void rmw_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#else
    /* portable no-op; ladder still yields via sched_yield rungs */
#endif
}

static inline int64_t rmw_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Config knobs (cached; getenv is NOT on the hot path)
// ---------------------------------------------------------------------------

static int64_t rmw_cfg_env_i64(const char *name, int64_t def,
                               int64_t lo, int64_t hi) {
    const char *s = getenv(name);
    if (s == NULL || *s == '\0') return def;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == NULL || *end != '\0') return def;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int64_t)v;
}

int64_t rmw_weft_spin_budget_ns(void) {
    static _Atomic int64_t cached;
    int64_t v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v == 0) {
        v = rmw_cfg_env_i64("WEFT_RMW_SPIN_US", 40, 0, 1000000) * 1000;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v;
}

int64_t rmw_weft_cfg_pub_wait_ns(void) {
    static _Atomic int64_t cached;
    int64_t v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v == 0) {
        v = rmw_cfg_env_i64("WEFT_RMW_PUB_WAIT_US", 50000, 0, 60000000) * 1000;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v;
}

int64_t rmw_weft_cfg_sub_wait_ns(void) {
    static _Atomic int64_t cached;
    int64_t v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v == 0) {
        v = rmw_cfg_env_i64("WEFT_RMW_SUB_WAIT_US", 100000, 0, 60000000) *
            1000;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v;
}

static uint32_t rmw_cfg_env_u32(const char *name, uint32_t def,
                                uint32_t lo, uint32_t hi) {
    const char *s = getenv(name);
    if (s == NULL || *s == '\0') return def;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == NULL || *end != '\0') return def;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (uint32_t)v;
}

uint32_t rmw_weft_cfg_slots(void) {
    return rmw_cfg_env_u32("WEFT_RMW_SLOTS", RMW_WEFT_RING_SLOTS_DEFAULT,
                           4u, 4096u);
}

uint32_t rmw_weft_cfg_payload(void) {
    return rmw_cfg_env_u32("WEFT_RMW_PAYLOAD",
                           RMW_WEFT_RING_PAYLOAD_DEFAULT, 16u, 4u << 20);
}

uint32_t rmw_weft_cfg_domain(uint32_t domain_id) {
    return domain_id > 232u ? 232u : domain_id;
}

// ---------------------------------------------------------------------------
// CRC-32 (IEEE reflected 0xEDB88320) — table built once, lazily
// ---------------------------------------------------------------------------

static uint32_t rmw_crc_table[256];
static _Atomic int rmw_crc_ready;

uint32_t rmw_weft_crc32(const void *data, size_t len) {
    if (atomic_load_explicit(&rmw_crc_ready, memory_order_acquire) == 0) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            rmw_crc_table[i] = c;
        }
        atomic_store_explicit(&rmw_crc_ready, 1, memory_order_release);
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = rmw_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Geometry / names
// ---------------------------------------------------------------------------

int rmw_ring_geometry_ok(uint32_t slot_count, uint32_t payload_bytes) {
    if (slot_count == 0 || (slot_count & (slot_count - 1u)) != 0) return 0;
    if (slot_count < 4u || slot_count > 4096u) return 0;
    if (payload_bytes < 16u || payload_bytes > (4u << 20)) return 0;
    uint64_t stride = (uint64_t)RMW_WEFT_SLOT_HDR_BYTES + payload_bytes;
    stride = (stride + 63u) & ~63ull;
    uint64_t mapping = RMW_WEFT_RING_SLOTS_OFFSET +
                       (uint64_t)slot_count * stride;
    return mapping < (1ull << 40) ? 1 : 0;  /* 1 TiB absurdity wall */
}

static int rmw_name_ok(const char *name) {
    if (name == NULL) return 0;
    size_t n = 0;
    while (name[n] != '\0') {
        char c = name[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return 0;
        }
        if (++n >= RMW_WEFT_RING_NAME_MAX) return 0;
    }
    return (n > 0 && name[0] != '-') ? 1 : 0;
}

static uint32_t rmw_slot_stride(uint32_t payload_bytes) {
    uint64_t s = (uint64_t)RMW_WEFT_SLOT_HDR_BYTES + payload_bytes;
    return (uint32_t)((s + 63u) & ~63ull);
}

static rmw_ring_slot_t *rmw_slot_at(rmw_ring_map_t *m, uint64_t idx) {
    uint64_t stride = m->hdr->slot_stride;
    uint8_t *p = (uint8_t *)m->slots +
                 (idx & (uint64_t)(m->hdr->slot_count - 1u)) * stride;
    return (rmw_ring_slot_t *)p;
}

static uint8_t *rmw_payload_at(rmw_ring_slot_t *slot) {
    return rmw_ring_slot_payload(slot);
}

// ---------------------------------------------------------------------------
// Create / attach / destroy
// ---------------------------------------------------------------------------

static int rmw_validate_ctrl(const rmw_ring_ctrl_t *ctrl) {
    if (!atomic_is_lock_free(&ctrl->head)) return -1;
    if (!atomic_is_lock_free(&ctrl->tail_ack)) return -1;
    if (!atomic_is_lock_free(&ctrl->doorbell)) return -1;
    if (!atomic_is_lock_free(&ctrl->state)) return -1;
    if (((uintptr_t)&ctrl->head & 7u) != 0) return -1;
    if (((uintptr_t)ctrl & 63u) != 0) return -1;
    return 0;
}

int rmw_ring_create(const char *name, uint32_t slot_count,
                    uint32_t payload_bytes, uint32_t sub_instance,
                    uint32_t reliability, uint64_t registry_epoch,
                    rmw_ring_map_t *out) {
    if (name == NULL || out == NULL) return -1;
    if (!rmw_name_ok(name)) return -1;
    if (!rmw_ring_geometry_ok(slot_count, payload_bytes)) return -1;

    memset(out, 0, sizeof(*out));
    out->fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (out->fd < 0) return -1;

    uint32_t stride = rmw_slot_stride(payload_bytes);
    uint64_t mapping =
        RMW_WEFT_RING_SLOTS_OFFSET + (uint64_t)slot_count * stride;
    if (ftruncate(out->fd, (off_t)mapping) != 0) {
        (void)close(out->fd);
        shm_unlink(name);
        return -1;
    }

    out->base = (uint8_t *)mmap(NULL, (size_t)mapping, PROT_READ | PROT_WRITE,
                                MAP_SHARED, out->fd, 0);
    if (out->base == MAP_FAILED) {
        (void)close(out->fd);
        shm_unlink(name);
        return -1;
    }

    rmw_ring_header_t *h = (rmw_ring_header_t *)out->base;
    rmw_ring_ctrl_t *ctrl = (rmw_ring_ctrl_t *)(out->base +
                                                RMW_WEFT_RING_HEADER_BYTES);
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);

    memset(h, 0, sizeof(*h));
    h->magic = RMW_WEFT_RING_MAGIC;
    h->version = RMW_WEFT_RING_VERSION;
    h->header_size = RMW_WEFT_RING_HEADER_BYTES;
    h->slot_count = slot_count;
    h->payload_bytes = payload_bytes;
    h->slot_stride = stride;
    h->mapping_bytes = mapping;
    h->creator_pid = (uint32_t)getpid();
    h->created_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull +
                         (uint64_t)ts.tv_nsec;
    h->registry_epoch = registry_epoch;
    h->sub_instance = sub_instance;
    h->reliability = reliability;

    atomic_init(&ctrl->head, 0);
    atomic_init(&ctrl->tail_ack, 0);
    atomic_init(&ctrl->published_total, 0);
    atomic_init(&ctrl->dropped_total, 0);
    atomic_init(&ctrl->doorbell, 0);
    atomic_init(&ctrl->waiters, 0);
    atomic_init(&ctrl->state, 0);
    atomic_init(&ctrl->pub_gid_a, 0);
    atomic_init(&ctrl->pub_gid_b, 0);

    if (rmw_validate_ctrl(ctrl) != 0) {
        (void)munmap(out->base, (size_t)mapping);
        (void)close(out->fd);
        shm_unlink(name);
        return -1;
    }

    out->ctrl = ctrl;
    out->slots = (rmw_ring_slot_t *)(out->base + RMW_WEFT_RING_SLOTS_OFFSET);
    out->hdr = (const rmw_ring_header_t *)out->base;
    out->mapping_bytes = (size_t)mapping;
    out->creator = 1;
    snprintf(out->name, sizeof(out->name), "%s", name);
    return 0;
}

int rmw_ring_attach(const char *name, uint32_t expected_slot_count,
                    uint32_t expected_payload_bytes, rmw_ring_map_t *out) {
    if (name == NULL || out == NULL) return -1;
    if (!rmw_name_ok(name)) return -1;

    memset(out, 0, sizeof(*out));
    out->fd = shm_open(name, O_RDWR, 0600);
    if (out->fd < 0) return -1;

    struct stat st;
    if (fstat(out->fd, &st) != 0 || st.st_size <= 0) {
        (void)close(out->fd);
        return -1;
    }

    /* Probe the header with a private throwaway mapping first: a torn or
     * foreign object must never be validated against a partially written
     * public view. Creator wrote the header before any attacher could open
     * (O_EXCL + registry ordering), so this is belt-and-suspenders. */
    size_t probe_len = RMW_WEFT_RING_SLOTS_OFFSET;
    uint8_t *probe = (uint8_t *)mmap(NULL, probe_len, PROT_READ,
                                     MAP_SHARED, out->fd, 0);
    if (probe == MAP_FAILED) {
        (void)close(out->fd);
        return -1;
    }
    const rmw_ring_header_t *h = (const rmw_ring_header_t *)probe;
    int bad = 0;
    if (h->magic != RMW_WEFT_RING_MAGIC) bad = 1;
    if (h->version != RMW_WEFT_RING_VERSION) bad = 1;
    if (h->header_size != RMW_WEFT_RING_HEADER_BYTES) bad = 1;
    if (h->slot_count != expected_slot_count) bad = 1;
    if (h->payload_bytes != expected_payload_bytes) bad = 1;
    if (h->slot_stride != rmw_slot_stride(expected_payload_bytes)) bad = 1;
    if (h->mapping_bytes != (uint64_t)st.st_size) bad = 1;
    for (size_t i = 0; i < sizeof(h->reserved) && !bad; i++) {
        if (h->reserved[i] != 0) bad = 1;
    }
    (void)munmap(probe, probe_len);
    if (bad) {
        (void)close(out->fd);
        return -1;
    }

    size_t mapping = (size_t)st.st_size;
    out->base = (uint8_t *)mmap(NULL, mapping, PROT_READ | PROT_WRITE,
                                MAP_SHARED, out->fd, 0);
    if (out->base == MAP_FAILED) {
        (void)close(out->fd);
        return -1;
    }
    out->ctrl = (rmw_ring_ctrl_t *)(out->base + RMW_WEFT_RING_HEADER_BYTES);
    if (rmw_validate_ctrl(out->ctrl) != 0) {
        (void)munmap(out->base, mapping);
        (void)close(out->fd);
        return -1;
    }
    out->slots = (rmw_ring_slot_t *)(out->base + RMW_WEFT_RING_SLOTS_OFFSET);
    out->hdr = (const rmw_ring_header_t *)out->base;
    out->mapping_bytes = mapping;
    out->creator = 0;
    snprintf(out->name, sizeof(out->name), "%s", name);
    return 0;
}

void rmw_ring_destroy(rmw_ring_map_t *m) {
    if (m == NULL || m->base == NULL) return;
    if (m->creator) {
        atomic_store_explicit(&m->ctrl->state, 2u, memory_order_release);
        shm_unlink(m->name);
    }
    (void)munmap(m->base, m->mapping_bytes);
    if (m->fd >= 0) (void)close(m->fd);
    memset(m, 0, sizeof(*m));
    m->fd = -1;
}

void rmw_ring_stamp_publisher(rmw_ring_map_t *m, uint64_t gid_a,
                              uint64_t gid_b) {
    if (m == NULL || m->ctrl == NULL) return;
    atomic_store_explicit(&m->ctrl->pub_gid_a, gid_a, memory_order_release);
    atomic_store_explicit(&m->ctrl->pub_gid_b, gid_b, memory_order_release);
}

// ---------------------------------------------------------------------------
// Hot path: publish / borrow / commit
// ---------------------------------------------------------------------------

static int rmw_ring_has_room(const rmw_ring_map_t *m, uint64_t head,
                             uint64_t tail) {
    /* keep one slot of margin: the loan at the tail is never the target */
    return (head - tail) <= (uint64_t)(m->hdr->slot_count - 2u);
}

static int rmw_ring_wait_room(rmw_ring_map_t *m, int reliable,
                              int64_t deadline_ns) {
    /* Bounded ladder for RELIABLE publishers: pause phase -> yield rungs
     * -> 50 us sleeps. PROGRESS-AWARE (D-62 §C.5): the zero-progress
     * deadline RE-ARMS every time the consumer's tail_ack advances, so a
     * live-but-slow consumer keeps the publisher backpressured (correct
     * RELIABLE semantics) while a true dead peer is detected within one
     * budget window. This matters in quota-throttled containers where the
     * CFS scheduler can stall BOTH processes for tens of milliseconds:
     * wall-clock-only deadlines convert a transient container stall into
     * a spurious stream failure. The budget default (20 ms) bridges a
     * typical CFS period; honest RMW_RET_TIMEOUT still fires within one
     * window of zero consumer progress. */
    const uint64_t spins_per_us = 16u;
    uint64_t spin = (uint64_t)rmw_weft_spin_budget_ns() / 1000u *
                    spins_per_us;
    while (spin--) rmw_pause();

    int64_t budget = deadline_ns - rmw_now_ns();
    if (budget < 100000) budget = 100000;  /* 100 us floor: one honest try */
    int64_t zero_progress_deadline = rmw_now_ns() + budget;
    uint64_t last_tail = atomic_load_explicit(&m->ctrl->tail_ack,
                                              memory_order_acquire);

    int rung = 0;
    for (;;) {
        uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                             memory_order_acquire);
        uint64_t tail = atomic_load_explicit(&m->ctrl->tail_ack,
                                             memory_order_acquire);
        if (rmw_ring_has_room(m, head, tail)) return 1;
        if (!reliable) return 0;
        int64_t now = rmw_now_ns();
        if (tail != last_tail) {
            /* consumer made progress: it is alive — re-arm the window */
            last_tail = tail;
            zero_progress_deadline = now + budget;
        }
        if (now >= zero_progress_deadline) return 0;
        if ((rung++ & 7u) == 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000};
            (void)nanosleep(&ts, NULL);
        } else {
            (void)sched_yield();
        }
    }
}

static void rmw_ring_knock(rmw_ring_map_t *m) {
    atomic_fetch_add_explicit(&m->ctrl->doorbell, 1u, memory_order_release);
    if (atomic_load_explicit(&m->ctrl->waiters, memory_order_relaxed) != 0) {
        (void)syscall(SYS_futex, (uint32_t *)&m->ctrl->doorbell,
                      RMW_FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

static void rmw_ring_commit_locked(rmw_ring_map_t *m, rmw_ring_slot_t *slot,
                                    uint32_t size) {
    uint64_t seq = atomic_load_explicit(&m->ctrl->published_total,
                                        memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                         memory_order_acquire);
    uint64_t v = atomic_load_explicit(&slot->version, memory_order_relaxed);
    if (v & 1u) v += 1u;  /* torn prior writer: heal to even before reuse */

    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);

    atomic_store_explicit(&slot->version, v + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    slot->seq_id = seq;
    slot->payload_size = size;
    /* payload_crc stays 0 on the hot path — the O(1) SLA contract. The
     * integrity evidence story is CONTENT-LEVEL, in the batteries (LCG
     * full-payload verification in R1/torture, zero-corruption saturation
     * in R3, CRC in the vision battery); the engine-level tripwire for
     * tearing is the seqlock version pair re-checked on take. A commit-
     * time CRC-32 would cost ~150 us per 64 KiB message — 100x the whole
     * RTT SLA — to check bytes the zero-copy loan aliases BY CONSTRUCTION
     * (D-62 §B.3 has the math). rmw_weft_crc32 stays exported for
     * diagnostics and the vision battery's frame stamps. */
    slot->source_ts_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull +
                              (uint64_t)ts.tv_nsec;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&slot->version, v + 2u, memory_order_release);

    atomic_store_explicit(&m->ctrl->head, head + 1u, memory_order_release);
    atomic_fetch_add_explicit(&m->ctrl->published_total, 1u,
                              memory_order_relaxed);
    rmw_ring_knock(m);
}

int rmw_ring_publish(rmw_ring_map_t *m, const void *msg, uint32_t size,
                     int reliable, int64_t deadline_ns) {
    if (m == NULL || msg == NULL) return -1;
    if (size > m->hdr->payload_bytes) return -1;
    if (atomic_load_explicit(&m->ctrl->state, memory_order_acquire) == 2u) {
        return -2;  /* dead peer: honest refusal */
    }
    uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                         memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&m->ctrl->tail_ack,
                                         memory_order_acquire);
    if (!rmw_ring_has_room(m, head, tail)) {
        if (reliable) {
            if (deadline_ns < 0) {
                deadline_ns = rmw_now_ns() + rmw_weft_cfg_pub_wait_ns();
            }
            if (!rmw_ring_wait_room(m, 1, deadline_ns)) return -3; /* TO */
        } else {
            atomic_fetch_add_explicit(&m->ctrl->dropped_total, 1u,
                                      memory_order_relaxed);
            return -4;  /* BEST_EFFORT drop: newest, counted, honest */
        }
        head = atomic_load_explicit(&m->ctrl->head, memory_order_acquire);
    }
    rmw_ring_slot_t *slot = rmw_slot_at(m, head);
    uint8_t *dst = rmw_payload_at(slot);
    if (size > 0) memcpy(dst, msg, size);
    rmw_ring_commit_locked(m, slot, size);
    return 0;
}

int rmw_ring_borrow(rmw_ring_map_t *m, int reliable, int64_t deadline_ns,
                    rmw_ring_slot_t **slot_out, uint8_t **payload_out) {
    if (m == NULL || slot_out == NULL || payload_out == NULL) return -1;
    if (atomic_load_explicit(&m->ctrl->state, memory_order_acquire) == 2u) {
        return -2;
    }
    uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                         memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&m->ctrl->tail_ack,
                                         memory_order_acquire);
    if (!rmw_ring_has_room(m, head, tail)) {
        if (reliable) {
            if (deadline_ns < 0) {
                deadline_ns = rmw_now_ns() + rmw_weft_cfg_pub_wait_ns();
            }
            if (!rmw_ring_wait_room(m, 1, deadline_ns)) return -3;
        } else {
            atomic_fetch_add_explicit(&m->ctrl->dropped_total, 1u,
                                      memory_order_relaxed);
            return -4;
        }
        head = atomic_load_explicit(&m->ctrl->head, memory_order_acquire);
    }
    *slot_out = rmw_slot_at(m, head);
    *payload_out = rmw_payload_at(*slot_out);
    return 0;
}

int rmw_ring_commit(rmw_ring_map_t *m, rmw_ring_slot_t *slot,
                    uint32_t size) {
    if (m == NULL || slot == NULL) return -1;
    if (size > m->hdr->payload_bytes) return -1;
    rmw_ring_commit_locked(m, slot, size);
    return 0;
}

// ---------------------------------------------------------------------------
// Hot path: take / return / wait
// ---------------------------------------------------------------------------

int rmw_ring_try_take(rmw_ring_map_t *m, uint64_t cursor,
                      rmw_ring_slot_t **slot_out, uint64_t *seq_id,
                      uint32_t *size, uint32_t *crc,
                      uint64_t *source_ts) {
    if (m == NULL || slot_out == NULL) return -1;
    uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                         memory_order_acquire);
    if (head == cursor) return 0;
    rmw_ring_slot_t *slot = rmw_slot_at(m, cursor);
    uint64_t v = atomic_load_explicit(&slot->version, memory_order_acquire);
    if (v & 1u) return -1;  /* torn: writer mid-commit */
    if (slot->seq_id < cursor) {
        /* stale slot the writer never committed (abandoned borrow) */
        return -1;
    }
    *slot_out = slot;
    if (seq_id != NULL) *seq_id = slot->seq_id;
    if (size != NULL) *size = slot->payload_size;
    if (crc != NULL) *crc = slot->payload_crc;
    if (source_ts != NULL) *source_ts = slot->source_ts_unix_ns;
    return 1;
}

void rmw_ring_advance_tail(rmw_ring_map_t *m, uint64_t new_tail) {
    if (m == NULL) return;
    atomic_store_explicit(&m->ctrl->tail_ack, new_tail, memory_order_release);
}

static void rmw_futex_wait(_Atomic uint32_t *word, uint32_t expected,
                           int64_t deadline_ns) {
    struct timespec ts;
    int64_t now = rmw_now_ns();
    int64_t rem = deadline_ns - now;
    if (rem <= 0) rem = 200000;  /* minimal park so wake can land */
    if (rem > 1000000) rem = 1000000;  /* 1 ms re-check chunks */
    ts.tv_sec = rem / 1000000000;
    ts.tv_nsec = (long)(rem % 1000000000);
    (void)syscall(SYS_futex, (uint32_t *)word, RMW_FUTEX_WAIT,
                  (uint32_t)expected, &ts, NULL, 0);
}

int rmw_ring_wait(rmw_ring_map_t *m, uint64_t cursor, int64_t deadline_ns) {
    if (m == NULL) return 0;
    if (deadline_ns < 0) {
        deadline_ns = rmw_now_ns() + rmw_weft_cfg_sub_wait_ns();
    }
    const uint64_t spins_per_us = 16u;
    uint64_t spin = (uint64_t)rmw_weft_spin_budget_ns() / 1000u *
                    spins_per_us;
    while (spin--) {
        uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                             memory_order_acquire);
        if (head != cursor) return 1;
        rmw_pause();
    }
    for (;;) {
        uint64_t head = atomic_load_explicit(&m->ctrl->head,
                                             memory_order_acquire);
        if (head != cursor) return 1;
        if (rmw_now_ns() >= deadline_ns) return 0;

        atomic_fetch_add_explicit(&m->ctrl->waiters, 1u,
                                  memory_order_acq_rel);
        /* re-check AFTER publishing the waiter: the classic lost-wake fix */
        head = atomic_load_explicit(&m->ctrl->head, memory_order_acquire);
        if (head != cursor) {
            atomic_fetch_sub_explicit(&m->ctrl->waiters, 1u,
                                      memory_order_acq_rel);
            return 1;
        }
        uint32_t cur = atomic_load_explicit(&m->ctrl->doorbell,
                                            memory_order_acquire);
        rmw_futex_wait(&m->ctrl->doorbell, cur, deadline_ns);
        atomic_fetch_sub_explicit(&m->ctrl->waiters, 1u,
                                  memory_order_acq_rel);
    }
}
