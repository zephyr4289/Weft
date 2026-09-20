// weft_tensor_arena.c — page-aligned tensor arenas (RFC-0021 §4).
//
// The ONLY allocation surface in the fabric (Law 1): one mmap at create,
// then O(1) bump sub-allocation — a cursor, an alignment round, a bounds
// check. Zero malloc/free, zero syscalls on the alloc path.
//
// Placement posture (mirrors the turbo.c house pattern): mmap gives the
// page alignment for free; MADV_HUGEPAGE is requested as a HINT and its
// acceptance is REPORTED (hugepage_hint), never claimed; mlock refusal is
// REPORTED (locked=0), never fatal — an unprivileged runner still gets a
// correct, aligned, private arena and the tests assert the FIELD tells
// the truth. Prefault touches every page once so the first frame does not
// pay minor faults.
//
// Thread discipline: the bump cursor is SINGLE-THREAD by design (the
// sensor/inference thread owns its arena — the kernel's writer-private
// discipline). No locks exist to contend; misuse across threads is a
// caller bug the ring layer's ownership protocol already separates.

#include "weft_tensor.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t wt_page_size(void) {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (uint64_t)ps : 4096u;
}

static uint64_t wt_round_up(uint64_t v, uint64_t mult) {
    // mult is a power of two everywhere it is used here.
    return (v + mult - 1) & ~(mult - 1);
}

static int wt_valid_flags(uint32_t flags) {
    const uint32_t known = WEFT_TENSOR_ARENA_F_MLOCK | WEFT_TENSOR_ARENA_F_HUGEPAGE |
                           WEFT_TENSOR_ARENA_F_PREFAULT | WEFT_TENSOR_ARENA_F_SHARED;
    return (flags & ~known) == 0;
}

// ---------------------------------------------------------------------------
// Create / attach / destroy
// ---------------------------------------------------------------------------

int weft_tensor_arena_create(weft_tensor_arena_t* a, uint64_t capacity,
                             uint32_t flags) {
    if (a == NULL) return WEFT_TENSOR_EINVAL;
    if (!wt_valid_flags(flags)) return WEFT_TENSOR_EINVAL;
    if (capacity == 0 || capacity >= (1ull << 63)) return WEFT_TENSOR_EINVAL;

    const uint64_t page = wt_page_size();
    const uint64_t cap = wt_round_up(capacity, page);

    int prot = PROT_READ | PROT_WRITE;
    int mapf = MAP_ANONYMOUS | (flags & WEFT_TENSOR_ARENA_F_SHARED ? MAP_SHARED
                                                                   : MAP_PRIVATE);
#if defined(MAP_NORESERVE) && !defined(__APPLE__)
    mapf |= MAP_NORESERVE;  // a purely bump-fed arena pages in lazily anyway
#endif
    void* base = mmap(NULL, (size_t)cap, prot, mapf, -1, 0);
    if (base == MAP_FAILED) return WEFT_TENSOR_ENOMEM;

    memset(a, 0, sizeof(*a));
    a->base = (uint8_t*)base;
    a->capacity = cap;
    a->flags = flags;
    a->creator = 1;

    // Hugepage hint (Linux; advisory — acceptance reported, not claimed).
    if (flags & WEFT_TENSOR_ARENA_F_HUGEPAGE) {
#ifdef MADV_HUGEPAGE
        a->hugepage_hint = madvise(base, (size_t)cap, MADV_HUGEPAGE) == 0;
#else
        a->hugepage_hint = 0;
#endif
    }

    // Page-lock (refusal reported honestly, never fatal).
    if (flags & WEFT_TENSOR_ARENA_F_MLOCK) {
        a->locked = mlock(base, (size_t)cap) == 0;
    }

    // Prefault: one write per page kills the minor-fault storm on frame 1.
    if (flags & WEFT_TENSOR_ARENA_F_PREFAULT) {
        for (uint64_t off = 0; off < cap; off += page) {
            a->base[off] = 0;
        }
        // Touch the final byte too (cap may exceed the last page boundary
        // of the requested capacity by rounding).
        a->base[cap - 1] = 0;
    }
    return WEFT_TENSOR_OK;
}

int weft_tensor_arena_attach(weft_tensor_arena_t* a, void* memory,
                             uint64_t capacity, uint32_t flags) {
    if (a == NULL || memory == NULL) return WEFT_TENSOR_EINVAL;
    if (!wt_valid_flags(flags)) return WEFT_TENSOR_EINVAL;
    if (capacity == 0 || capacity >= (1ull << 63)) return WEFT_TENSOR_EINVAL;
    if (((uintptr_t)memory & 15u) != 0) return WEFT_TENSOR_EMISALIGN;  // Law 4

    memset(a, 0, sizeof(*a));
    a->base = (uint8_t*)memory;
    a->capacity = capacity;
    a->flags = flags;
    a->creator = 0;
    // Attacher-declared knowledge: we never mlock/madvise memory we do
    // not own — the fields stay 0 (honest: "not imposed").
    return WEFT_TENSOR_OK;
}

void weft_tensor_arena_destroy(weft_tensor_arena_t* a) {
    if (a == NULL) return;
    if (a->creator && a->base != NULL) {
        munmap(a->base, (size_t)a->capacity);
    }
    memset(a, 0, sizeof(*a));
}

// ---------------------------------------------------------------------------
// Bump sub-allocation (O(1); the sub-microsecond claim is measured in
// WT-series latency legs, not asserted in prose)
// ---------------------------------------------------------------------------

void* weft_tensor_arena_alloc(weft_tensor_arena_t* a, uint64_t size,
                              uint32_t align, uint64_t* offset_out,
                              int* status_out) {
    int st = WEFT_TENSOR_OK;
    void* result = NULL;
    do {
        if (a == NULL || a->base == NULL) { st = WEFT_TENSOR_EINVAL; break; }
        if (align == 0 || align > 4096u || (align & (align - 1u)) != 0) {
            st = WEFT_TENSOR_EINVAL;  // power-of-two alignment classes only
            break;
        }
        if (size == 0 || size >= (1ull << 63)) { st = WEFT_TENSOR_EINVAL; break; }

        const uint64_t aligned = wt_round_up(a->cursor, align);
        if (aligned > a->capacity || size > a->capacity - aligned) {
            st = WEFT_TENSOR_ENOMEM;  // exhausted: reported, never rounded
            break;
        }

        a->cursor = aligned + size;
        a->alloc_count++;
        if (a->cursor > a->highwater) a->highwater = a->cursor;
        if (offset_out != NULL) *offset_out = aligned;
        result = a->base + aligned;
    } while (0);

    if (status_out != NULL) *status_out = st;
    return result;
}

int weft_tensor_arena_mark(const weft_tensor_arena_t* a,
                           weft_tensor_arena_mark_t* m) {
    if (a == NULL || m == NULL) return WEFT_TENSOR_EINVAL;
    m->cursor = a->cursor;
    m->alloc_count = a->alloc_count;
    return WEFT_TENSOR_OK;
}

int weft_tensor_arena_rewind(weft_tensor_arena_t* a,
                             const weft_tensor_arena_mark_t* m) {
    if (a == NULL || m == NULL) return WEFT_TENSOR_EINVAL;
    if (m->cursor > a->cursor) return WEFT_TENSOR_EINVAL;  // foreign/regressive
    a->cursor = m->cursor;
    return WEFT_TENSOR_OK;
}

void weft_tensor_arena_reset(weft_tensor_arena_t* a) {
    if (a == NULL) return;
    a->cursor = 0;
}

void weft_tensor_arena_stats(const weft_tensor_arena_t* a, uint64_t* used,
                             uint64_t* highwater, uint64_t* allocs) {
    if (used != NULL) *used = a ? a->cursor : 0;
    if (highwater != NULL) *highwater = a ? a->highwater : 0;
    if (allocs != NULL) *allocs = a ? a->alloc_count : 0;
}
