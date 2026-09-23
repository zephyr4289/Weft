// weft_tensor_ring.c — lock-free circular DMA tensor ring (RFC-0021 §5).
//
// PROTOCOL (Vyukov-style bounded sequence cursors, adapted so every slot
// carries a full weft_tensor_view_t):
//
//   invariant   slot[i].seq cycles  i -> (commit t: t+1) -> (release t:
//               t+N) -> (commit t+N: t+N+1) -> ...  where t ≡ i (mod N).
//               seq == t     the slot is free for producer ticket t
//               seq == t+1   ticket t is COMMITTED (awaiting acquire)
//               otherwise    in transition; try operations report EAGAIN
//
//   claim       producers race a CAS on ctrl->head (check-then-CAS: the
//               slot-seq gate makes the check authoritative, the CAS
//               makes the ticket unique). Winner owns the slot's payload
//               exclusively until commit.
//   commit      producer writes payload + view (plain stores), then ONE
//               release-store of seq = t+1. Every payload byte written
//               before the release is visible to the consumer that
//               acquire-loads seq == t+1 — a committed tensor is never
//               observed torn (the WT-series stress battery proves it
//               under MPSC, SPMC and fork()).
//   acquire     consumers gate on slot[tail].seq == tail+1, then advance
//               ctrl->tail (store for the MPSC single consumer, CAS for
//               SPMC/MMPC — every ticket is acquired by EXACTLY one
//               consumer, structurally).
//   release     consumer reads payload + view, then release-stores
//               seq = t+N: its reads happen-before the next producer's
//               writes for that slot (the producer's claim acquire-loads
//               seq == its ticket). Full bidirectional hygiene, no fences
//               beyond the release/acquire pairs the protocol already
//               needs — x86 pays nothing extra, ARM gets it for free.
//
// LAW 2 (bounded latency): steady-state claim/commit/acquire/release is
// pure shared-memory atomics — zero syscalls (the strace evidence class
// the shm_ring S-series established). Under contention the wait ladder
// runs FIXED rungs — 64 pause cycles, one sched_yield, one 50us
// nanosleep — with the caller's deadline checked between every rung.
// There is no unbounded-wait entry point: timeout_ns is a required
// argument everywhere.
//
// CROSS-PROCESS: the mapping is MAP_SHARED|MAP_ANONYMOUS; fork() children
// and any process handed the mapping (shm object, memfd, dma-buf import)
// attach with weft_tensor_ring_attach and see the SAME ring (C11 atomics
// on shared pages are cross-process-safe — the documented foundation the
// RFC-0011 session layer already rides). Pointers handed out derive from
// the LOCAL base — the ring is fully relocatable across address spaces.

#include "weft_tensor.h"

#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Wire layout: control header (the first 128 bytes of the mapping)
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t           magic;            // 0   "WTR1"
    uint16_t           version;          // 4
    uint16_t           mode;             // 6   MPSC / SPMC / MPMC
    uint32_t           slot_count;       // 8   power of two, <= 2^20
    uint32_t           payload_bytes;    // 12  multiple of 64
    uint32_t           slot_stride;      // 16  256 + payload_bytes
    uint32_t           flags;            // 20  creator flags (advisory)
    uint64_t           ring_bytes;       // 24  128 + slot_count*slot_stride
    uint32_t           creator_pid;      // 32  diagnostics only
    uint32_t           reserved0;        // 36  zero; unknown bits rejected
    uint64_t           created_unix_ns;  // 40  diagnostics only
    _Atomic uint64_t   head;             // 48  next ticket to CLAIM
    _Atomic uint64_t   tail;             // 56  next ticket to ACQUIRE
    _Atomic uint64_t   stat_committed;   // 64
    _Atomic uint64_t   stat_acquired;    // 72
    _Atomic uint64_t   stat_released;    // 80
    _Atomic uint64_t   stat_full_hits;   // 88  try_claim refusals
    uint64_t           reserved[4];      // 96..127 zero; rejected if not
} weft_tensor_ring_ctrl_t;

_Static_assert(sizeof(weft_tensor_ring_ctrl_t) == WEFT_TENSOR_RING_CTRL_BYTES,
               "ctrl header ABI drift: expected 128 bytes (RFC-0021 §5)");
_Static_assert(offsetof(weft_tensor_ring_ctrl_t, head) == 48, "ABI: head");
_Static_assert(offsetof(weft_tensor_ring_ctrl_t, tail) == 56, "ABI: tail");
_Static_assert(offsetof(weft_tensor_ring_ctrl_t, stat_committed) == 64,
               "ABI: stat_committed");

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static inline weft_tensor_ring_ctrl_t* wt_ctrl(const weft_tensor_ring_t* r) {
    return (weft_tensor_ring_ctrl_t*)r->base;
}

static inline weft_tensor_ring_slot_t* wt_slot(const weft_tensor_ring_t* r,
                                               uint64_t ticket) {
    const uint64_t idx = ticket & (uint64_t)(r->slot_count - 1u);
    return (weft_tensor_ring_slot_t*)(r->base + WEFT_TENSOR_RING_CTRL_BYTES +
                                       idx * (uint64_t)r->slot_stride);
}

static inline uint8_t* wt_slot_payload(const weft_tensor_ring_t* r,
                                       uint64_t ticket) {
    return (uint8_t*)wt_slot(r, ticket) + WEFT_TENSOR_SLOT_HEADER_BYTES;
}

static inline void wt_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

static uint64_t wt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t wt_page_size(void) {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (uint64_t)ps : 4096u;
}

/// The Law-2 wait ladder: fixed rungs, deadline checked between every
/// rung. Returns ETIMEOUT when the deadline passed, OK to re-probe.
static int wt_wait_ladder(uint64_t deadline_ns) {
    for (uint32_t i = 0; i < WEFT_TENSOR_WAIT_PAUSE_SPINS; i++) wt_cpu_relax();
    if (wt_now_ns() >= deadline_ns) return WEFT_TENSOR_ETIMEOUT;
    sched_yield();
    if (wt_now_ns() >= deadline_ns) return WEFT_TENSOR_ETIMEOUT;
    struct timespec ts = {0, (long)WEFT_TENSOR_WAIT_SLEEP_NS};
    nanosleep(&ts, NULL);
    if (wt_now_ns() >= deadline_ns) return WEFT_TENSOR_ETIMEOUT;
    return WEFT_TENSOR_OK;
}

static int wt_mode_valid(uint16_t mode) {
    return mode == WEFT_TENSOR_RING_MODE_MPSC ||
           mode == WEFT_TENSOR_RING_MODE_SPMC ||
           mode == WEFT_TENSOR_RING_MODE_MPMC;
}

static int wt_ring_flags_valid(uint32_t flags) {
    const uint32_t known = WEFT_TENSOR_RING_F_MLOCK | WEFT_TENSOR_RING_F_HUGEPAGE |
                           WEFT_TENSOR_RING_F_PREFAULT;
    return (flags & ~known) == 0;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

uint64_t weft_tensor_ring_required_bytes(uint32_t slot_count,
                                         uint32_t payload_bytes,
                                         int* status_out) {
    int st = WEFT_TENSOR_OK;
    uint64_t bytes = 0;
    do {
        if (slot_count == 0 || slot_count > WEFT_TENSOR_RING_MAX_SLOTS ||
            (slot_count & (slot_count - 1u)) != 0) {
            st = WEFT_TENSOR_EINVAL;  // power-of-two depth only
            break;
        }
        if (payload_bytes == 0 || (payload_bytes & 63u) != 0 ||
            payload_bytes > (1u << 30)) {
            st = WEFT_TENSOR_EINVAL;  // 64B multiple (128B users pass a
            break;                    // 128 multiple — also a 64 multiple)
        }
        const uint64_t stride =
            (uint64_t)WEFT_TENSOR_SLOT_HEADER_BYTES + payload_bytes;
        const uint64_t total =
            (uint64_t)WEFT_TENSOR_RING_CTRL_BYTES + (uint64_t)slot_count * stride;
        if (total < (uint64_t)WEFT_TENSOR_RING_CTRL_BYTES) {
            st = WEFT_TENSOR_EOVERFLOW;
            break;
        }
        bytes = total;
    } while (0);

    if (status_out != NULL) *status_out = st;
    return bytes;
}

// ---------------------------------------------------------------------------
// Create / attach / destroy
// ---------------------------------------------------------------------------

int weft_tensor_ring_create(weft_tensor_ring_t* r, uint32_t slot_count,
                            uint32_t payload_bytes, uint16_t mode,
                            uint32_t flags) {
    if (r == NULL) return WEFT_TENSOR_EINVAL;
    if (!wt_mode_valid(mode)) return WEFT_TENSOR_EINVAL;
    if (!wt_ring_flags_valid(flags)) return WEFT_TENSOR_EINVAL;

    int st = WEFT_TENSOR_OK;
    const uint64_t ring_bytes =
        weft_tensor_ring_required_bytes(slot_count, payload_bytes, &st);
    if (st != WEFT_TENSOR_OK) return st;

    const uint64_t page = wt_page_size();
    const uint64_t map_len = (ring_bytes + page - 1) & ~(page - 1);
    void* base = mmap(NULL, (size_t)map_len, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return WEFT_TENSOR_ENOMEM;

    memset(r, 0, sizeof(*r));
    r->base = (uint8_t*)base;
    r->slot_count = slot_count;
    r->payload_bytes = payload_bytes;
    r->slot_stride = WEFT_TENSOR_SLOT_HEADER_BYTES + payload_bytes;
    r->mode = mode;
    r->flags = flags;
    r->ring_bytes = ring_bytes;
    r->creator = 1;

    // Hugepage hint — advisory, acceptance reported (never claimed).
    if (flags & WEFT_TENSOR_RING_F_HUGEPAGE) {
#ifdef MADV_HUGEPAGE
        r->hugepage_hint = madvise(base, (size_t)map_len, MADV_HUGEPAGE) == 0;
#else
        r->hugepage_hint = 0;
#endif
    }
    // Page-lock — refusal reported honestly, never fatal.
    if (flags & WEFT_TENSOR_RING_F_MLOCK) {
        r->locked = mlock(base, (size_t)map_len) == 0;
    }

    // Identity fields (plain stores: written once, before any peer maps —
    // the attach contract validates them; zero pages carry the rest).
    weft_tensor_ring_ctrl_t* ctrl = wt_ctrl(r);
    ctrl->magic = WEFT_TENSOR_RING_MAGIC;
    ctrl->version = (uint16_t)WEFT_TENSOR_RING_VERSION;
    ctrl->mode = mode;
    ctrl->slot_count = slot_count;
    ctrl->payload_bytes = payload_bytes;
    ctrl->slot_stride = WEFT_TENSOR_SLOT_HEADER_BYTES + payload_bytes;
    ctrl->flags = flags;
    ctrl->ring_bytes = ring_bytes;
    ctrl->creator_pid = (uint32_t)getpid();
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ctrl->created_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    }
    atomic_store_explicit(&ctrl->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->stat_committed, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->stat_acquired, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->stat_released, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->stat_full_hits, 0, memory_order_relaxed);

    // Fresh-ring invariant: slot i is free for ticket i.
    for (uint32_t i = 0; i < slot_count; i++) {
        weft_tensor_ring_slot_t* slot =
            (weft_tensor_ring_slot_t*)(r->base + WEFT_TENSOR_RING_CTRL_BYTES +
                                       (uint64_t)i * r->slot_stride);
        atomic_store_explicit(&slot->seq, i, memory_order_relaxed);
    }

    // Prefault: touch every page once (kills the frame-1 minor-fault
    // storm); claims no THP backing, just presence.
    if (flags & WEFT_TENSOR_RING_F_PREFAULT) {
        for (uint64_t off = 0; off < map_len; off += page) {
            r->base[off] = 0;
        }
        r->base[map_len - 1] = 0;
    }
    return WEFT_TENSOR_OK;
}

int weft_tensor_ring_attach(weft_tensor_ring_t* r, void* mapping,
                            uint64_t mapping_bytes, uint16_t mode) {
    if (r == NULL || mapping == NULL) return WEFT_TENSOR_EINVAL;
    if (mapping_bytes < WEFT_TENSOR_RING_CTRL_BYTES) return WEFT_TENSOR_EINVAL;
    if (!wt_mode_valid(mode)) return WEFT_TENSOR_EINVAL;

    // Read the identity fields with plain loads: they are write-once
    // (creator, before publish) — every attach after that sees them
    // stable on cache-coherent shared pages.
    const weft_tensor_ring_ctrl_t* ctrl =
        (const weft_tensor_ring_ctrl_t*)mapping;
    if (ctrl->magic != WEFT_TENSOR_RING_MAGIC) return WEFT_TENSOR_EMAGIC;
    if (ctrl->version != WEFT_TENSOR_RING_VERSION) return WEFT_TENSOR_EMAGIC;
    if (ctrl->mode != mode) return WEFT_TENSOR_EINVAL;  // cursor ownership
                                                         // discipline differs
    if (ctrl->slot_count == 0 || ctrl->slot_count > WEFT_TENSOR_RING_MAX_SLOTS ||
        (ctrl->slot_count & (ctrl->slot_count - 1u)) != 0)
        return WEFT_TENSOR_EGEOMETRY;
    if (ctrl->payload_bytes == 0 || (ctrl->payload_bytes & 63u) != 0 ||
        ctrl->payload_bytes > (1u << 30))
        return WEFT_TENSOR_EGEOMETRY;
    const uint64_t stride =
        (uint64_t)WEFT_TENSOR_SLOT_HEADER_BYTES + ctrl->payload_bytes;
    if (ctrl->slot_stride != stride) return WEFT_TENSOR_EGEOMETRY;
    if (ctrl->ring_bytes !=
        (uint64_t)WEFT_TENSOR_RING_CTRL_BYTES +
            (uint64_t)ctrl->slot_count * stride)
        return WEFT_TENSOR_EGEOMETRY;
    if (mapping_bytes < ctrl->ring_bytes) return WEFT_TENSOR_EINVAL;
    if (ctrl->reserved0 != 0 || ctrl->reserved[0] != 0 || ctrl->reserved[1] != 0 ||
        ctrl->reserved[2] != 0 || ctrl->reserved[3] != 0)
        return WEFT_TENSOR_EGEOMETRY;  // unknown bits = version we don't know

    memset(r, 0, sizeof(*r));
    r->base = (uint8_t*)mapping;
    r->slot_count = ctrl->slot_count;
    r->payload_bytes = ctrl->payload_bytes;
    r->slot_stride = ctrl->slot_stride;
    r->mode = ctrl->mode;
    r->flags = ctrl->flags;
    r->ring_bytes = ctrl->ring_bytes;
    r->creator = 0;
    return WEFT_TENSOR_OK;
}

void weft_tensor_ring_destroy(weft_tensor_ring_t* r) {
    if (r == NULL || r->base == NULL) return;
    if (r->creator) {
        const uint64_t page = wt_page_size();
        const uint64_t map_len = (r->ring_bytes + page - 1) & ~(page - 1);
        munmap(r->base, (size_t)map_len);
    }
    memset(r, 0, sizeof(*r));
}

// ---------------------------------------------------------------------------
// Producer side
// ---------------------------------------------------------------------------

int weft_tensor_ring_try_claim(weft_tensor_ring_t* r, uint64_t* ticket_out,
                               uint8_t** payload_out, uint32_t* cap_out) {
    if (r == NULL || r->base == NULL || ticket_out == NULL)
        return WEFT_TENSOR_EINVAL;

    weft_tensor_ring_ctrl_t* ctrl = wt_ctrl(r);
    uint64_t h = atomic_load_explicit(&ctrl->head, memory_order_acquire);

    // Slot-seq gate: authoritative fullness + generation check.
    weft_tensor_ring_slot_t* slot = wt_slot(r, h);
    const uint64_t s = atomic_load_explicit(&slot->seq, memory_order_acquire);
    if (s != h) {
        atomic_fetch_add_explicit(&ctrl->stat_full_hits, 1, memory_order_relaxed);
        return WEFT_TENSOR_EAGAIN;
    }

    // Unique ticket: CAS on head. Losing the race is EAGAIN (retry), not
    // an error — the next probe sees head+1.
    if (!atomic_compare_exchange_strong_explicit(&ctrl->head, &h, h + 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return WEFT_TENSOR_EAGAIN;
    }

    *ticket_out = h;
    if (payload_out != NULL) *payload_out = wt_slot_payload(r, h);
    if (cap_out != NULL) *cap_out = r->payload_bytes;
    return WEFT_TENSOR_OK;
}

int weft_tensor_ring_claim(weft_tensor_ring_t* r, uint64_t timeout_ns,
                           uint64_t* ticket_out, uint8_t** payload_out,
                           uint32_t* cap_out) {
    if (r == NULL || r->base == NULL || ticket_out == NULL)
        return WEFT_TENSOR_EINVAL;
    if (timeout_ns == 0) {
        return weft_tensor_ring_try_claim(r, ticket_out, payload_out, cap_out);
    }
    const uint64_t deadline = wt_now_ns() + timeout_ns;
    for (;;) {
        const int st = weft_tensor_ring_try_claim(r, ticket_out, payload_out,
                                                  cap_out);
        if (st == WEFT_TENSOR_OK || st != WEFT_TENSOR_EAGAIN) return st;
        const int w = wt_wait_ladder(deadline);
        if (w == WEFT_TENSOR_ETIMEOUT) return WEFT_TENSOR_ETIMEOUT;
    }
}

int weft_tensor_ring_commit(weft_tensor_ring_t* r, uint64_t ticket,
                            const weft_tensor_view_t* view,
                            uint32_t payload_used) {
    if (r == NULL || r->base == NULL || view == NULL) return WEFT_TENSOR_EINVAL;
    if (payload_used == 0 || payload_used > r->payload_bytes)
        return WEFT_TENSOR_EINVAL;

    weft_tensor_ring_slot_t* slot = wt_slot(r, ticket);

    // Ownership check: after a successful claim the slot's sequence still
    // equals the ticket (we only release-store at commit). Anything else
    // is protocol misuse — detected, reported, not defended.
    const uint64_t s = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    if (s != ticket) return WEFT_TENSOR_EAGAIN;

    // Normalize + wall the view against THIS slot:
    //   physical_or_shm_addr := the slot's payload address (same-process
    //   consumers read it directly; cross-process consumers use the
    //   payload pointer their own acquire returned — documented);
    //   byte_length := payload_used (the committed span);
    //   the walking-extent wall then proves the view cannot touch a byte
    //   outside [payload, payload+payload_used), and Law 4 alignment
    //   (dtype-natural against payload+byte_offset) is enforced with an
    //   explicit EMISALIGN.
    weft_tensor_view_t v = *view;
    v.physical_or_shm_addr = (uintptr_t)wt_slot_payload(r, ticket);
    v.byte_length = payload_used;
    const int st = weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_NONE);
    if (st != WEFT_TENSOR_OK) return st;

    // Publish: plain stores for the descriptor, then the single
    // release-store that makes them (and the producer's payload writes)
    // visible to the acquiring consumer. Zero tearing by construction.
    slot->view = v;
    slot->payload_used = payload_used;
    slot->flags = 0;
    atomic_store_explicit(&slot->seq, ticket + 1, memory_order_release);
    atomic_fetch_add_explicit(&wt_ctrl(r)->stat_committed, 1,
                              memory_order_relaxed);
    return WEFT_TENSOR_OK;
}

// ---------------------------------------------------------------------------
// Consumer side
// ---------------------------------------------------------------------------

int weft_tensor_ring_try_acquire(weft_tensor_ring_t* r, uint64_t* ticket_out,
                                 const weft_tensor_view_t** view_out,
                                 const uint8_t** payload_out,
                                 uint32_t* used_out) {
    if (r == NULL || r->base == NULL || ticket_out == NULL || view_out == NULL)
        return WEFT_TENSOR_EINVAL;

    weft_tensor_ring_ctrl_t* ctrl = wt_ctrl(r);
    uint64_t t = atomic_load_explicit(&ctrl->tail, memory_order_acquire);

    // Committed gate: seq == t+1 means ticket t is fully published.
    weft_tensor_ring_slot_t* slot = wt_slot(r, t);
    const uint64_t s = atomic_load_explicit(&slot->seq, memory_order_acquire);
    if (s != t + 1) return WEFT_TENSOR_EAGAIN;

    // Advance tail FIRST — unique ownership of the ticket:
    //   MPSC  the single consumer owns tail (plain release-store);
    //   SPMC/MMPC  consumers race a CAS; the loser saw the same t, lost
    //   the advance, and returns EAGAIN without reading anything.
    if (r->mode == WEFT_TENSOR_RING_MODE_MPSC) {
        atomic_store_explicit(&ctrl->tail, t + 1, memory_order_release);
    } else {
        if (!atomic_compare_exchange_strong_explicit(&ctrl->tail, &t, t + 1,
                                                     memory_order_acq_rel,
                                                     memory_order_acquire)) {
            return WEFT_TENSOR_EAGAIN;
        }
    }

    *ticket_out = t;
    *view_out = &slot->view;  // stable until release
    if (payload_out != NULL) *payload_out = wt_slot_payload(r, t);
    if (used_out != NULL) *used_out = slot->payload_used;
    atomic_fetch_add_explicit(&ctrl->stat_acquired, 1, memory_order_relaxed);
    return WEFT_TENSOR_OK;
}

int weft_tensor_ring_acquire(weft_tensor_ring_t* r, uint64_t timeout_ns,
                             uint64_t* ticket_out,
                             const weft_tensor_view_t** view_out,
                             const uint8_t** payload_out, uint32_t* used_out) {
    if (r == NULL || r->base == NULL || ticket_out == NULL || view_out == NULL)
        return WEFT_TENSOR_EINVAL;
    if (timeout_ns == 0) {
        return weft_tensor_ring_try_acquire(r, ticket_out, view_out,
                                            payload_out, used_out);
    }
    const uint64_t deadline = wt_now_ns() + timeout_ns;
    for (;;) {
        const int st = weft_tensor_ring_try_acquire(r, ticket_out, view_out,
                                                    payload_out, used_out);
        if (st == WEFT_TENSOR_OK || st != WEFT_TENSOR_EAGAIN) return st;
        const int w = wt_wait_ladder(deadline);
        if (w == WEFT_TENSOR_ETIMEOUT) return WEFT_TENSOR_ETIMEOUT;
    }
}

int weft_tensor_ring_release(weft_tensor_ring_t* r, uint64_t ticket) {
    if (r == NULL || r->base == NULL) return WEFT_TENSOR_EINVAL;
    weft_tensor_ring_slot_t* slot = wt_slot(r, ticket);

    // Misuse detection: the ticket must be in the acquired state.
    const uint64_t s = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    if (s != ticket + 1) return WEFT_TENSOR_EAGAIN;

    // The consumer's reads happen-before this release-store; the next
    // producer for this slot (ticket + N) acquire-loads seq == its ticket
    // and is ordered after those reads. Bidirectional hygiene closed.
    atomic_store_explicit(&slot->seq, ticket + r->slot_count,
                          memory_order_release);
    atomic_fetch_add_explicit(&wt_ctrl(r)->stat_released, 1,
                              memory_order_relaxed);
    return WEFT_TENSOR_OK;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

void weft_tensor_ring_stats(const weft_tensor_ring_t* r, uint64_t* committed,
                            uint64_t* acquired, uint64_t* released,
                            uint64_t* full_hits) {
    if (r == NULL || r->base == NULL) {
        if (committed != NULL) *committed = 0;
        if (acquired != NULL) *acquired = 0;
        if (released != NULL) *released = 0;
        if (full_hits != NULL) *full_hits = 0;
        return;
    }
    const weft_tensor_ring_ctrl_t* ctrl = wt_ctrl(r);
    if (committed != NULL)
        *committed = atomic_load_explicit(&ctrl->stat_committed,
                                          memory_order_relaxed);
    if (acquired != NULL)
        *acquired = atomic_load_explicit(&ctrl->stat_acquired,
                                         memory_order_relaxed);
    if (released != NULL)
        *released = atomic_load_explicit(&ctrl->stat_released,
                                         memory_order_relaxed);
    if (full_hits != NULL)
        *full_hits = atomic_load_explicit(&ctrl->stat_full_hits,
                                          memory_order_relaxed);
}

uint64_t weft_tensor_ring_in_flight(const weft_tensor_ring_t* r) {
    if (r == NULL || r->base == NULL) return 0;
    const weft_tensor_ring_ctrl_t* ctrl = wt_ctrl(r);
    const uint64_t c =
        atomic_load_explicit(&ctrl->stat_committed, memory_order_relaxed);
    const uint64_t a =
        atomic_load_explicit(&ctrl->stat_acquired, memory_order_relaxed);
    return c >= a ? c - a : 0;
}

uint32_t weft_tensor_ring_slot_count(const weft_tensor_ring_t* r) {
    return r != NULL ? r->slot_count : 0;
}

uint32_t weft_tensor_ring_payload_bytes(const weft_tensor_ring_t* r) {
    return r != NULL ? r->payload_bytes : 0;
}

uint32_t weft_tensor_ring_slot_stride(const weft_tensor_ring_t* r) {
    return r != NULL ? r->slot_stride : 0;
}

uint16_t weft_tensor_ring_mode(const weft_tensor_ring_t* r) {
    return r != NULL ? (uint16_t)r->mode : 0;
}
