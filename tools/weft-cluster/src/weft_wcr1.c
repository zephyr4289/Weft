// weft_wcr1.c — the region contract implementation (see include/weft_wcr1.h).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "weft_wcr1.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>

#include "shm_ring.h"   // the WFSH bridge (core/c — read-only dependency)

// ---- frozen ABI (Law 2) ------------------------------------------------------

_Static_assert(sizeof(weft_wcr1_region_t) == 40, "wcr1: region view is 40 bytes");
_Static_assert(offsetof(weft_wcr1_region_t, span)          == 8,  "wcr1: span@8");
_Static_assert(offsetof(weft_wcr1_region_t, chunk0_offset) == 16, "wcr1: chunk0@16");
_Static_assert(offsetof(weft_wcr1_region_t, chunk_size)    == 24, "wcr1: chunk_size@24");
_Static_assert(offsetof(weft_wcr1_region_t, chunk_count)   == 28, "wcr1: chunk_count@28");
_Static_assert(offsetof(weft_wcr1_region_t, node_id)       == 32, "wcr1: node_id@32");

static void refuse(char* why, size_t whylen, const char* msg) {
    if (why && whylen) snprintf(why, whylen, "wcr1: %s", msg);
}

weft_wcr1_refusal_t weft_wcr1_validate(const weft_wcr1_region_t* r,
                                       char* why, size_t whylen) {
    if (!r || !r->base) {
        refuse(why, whylen, "NULL region/base (rung: null)");
        return WEFT_WCR1_REFUSE_NULL;
    }
    if (((uintptr_t)r->base % WEFT_WCR1_PAGE_ALIGN) != 0) {
        refuse(why, whylen, "base not 4096-aligned (rung: base_align)");
        return WEFT_WCR1_REFUSE_BASE_ALIGN;
    }
    if ((r->chunk_size % WEFT_WCR1_CHUNK_ALIGN) != 0 || r->chunk_size == 0) {
        refuse(why, whylen, "chunk_size not a multiple of 64 (rung: chunk_align)");
        return WEFT_WCR1_REFUSE_CHUNK_ALIGN;
    }
    if (r->chunk_count < 2) {
        refuse(why, whylen, "chunk_count < 2 (rung: chunk_count)");
        return WEFT_WCR1_REFUSE_CHUNK_COUNT;
    }
    if ((r->chunk0_offset % WEFT_WCR1_CHUNK_ALIGN) != 0) {
        refuse(why, whylen, "chunk0_offset not 64-aligned (rung: chunk0_align)");
        return WEFT_WCR1_REFUSE_CHUNK0_ALIGN;
    }
    if (r->span != r->chunk0_offset + (uint64_t)r->chunk_count * r->chunk_size) {
        refuse(why, whylen, "span != chunk0 + count*chunk_size (rung: span_geometry)");
        return WEFT_WCR1_REFUSE_SPAN_GEOMETRY;
    }
    return WEFT_WCR1_OK;
}

int weft_wcr1_create_anon(uint32_t chunk_count, uint32_t chunk_size,
                          uint16_t node_id, weft_wcr1_region_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    // Geometry first — refuse by name before any allocation.
    weft_wcr1_region_t probe = {
        .base = (uint8_t*)0x1000,  // page-aligned sentinel for validation
        .span = (uint64_t)chunk_count * chunk_size,
        .chunk0_offset = 0,
        .chunk_size = chunk_size,
        .chunk_count = chunk_count,
        .node_id = node_id,
    };
    if (weft_wcr1_validate(&probe, NULL, 0) != WEFT_WCR1_OK) return -1;

    const size_t span = (size_t)probe.span;
    void* p = mmap(NULL, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;

    // Prefault: every engine pins these pages; fault-at-registration is
    // setup-path, fault-in-transport would be a Law-2 latency hit.
    memset(p, 0, span);

    // Best-effort pin (RLIMIT_MEMLOCK honesty: recorded, never laundered).
    if (mlock(p, span) == 0) {
        probe.flags |= WEFT_WCR1_F_PINNED;
    }

    probe.base = (uint8_t*)p;
    *out = probe;
    return 0;
}

void weft_wcr1_destroy(weft_wcr1_region_t* r) {
    if (!r || !r->base) return;
    if (r->flags & WEFT_WCR1_F_PINNED) munlock(r->base, (size_t)r->span);
    munmap(r->base, (size_t)r->span);
    memset(r, 0, sizeof(*r));
}

weft_wcr1_refusal_t weft_wcr1_from_shm(const struct weft_shm_map* m,
                                       uint16_t node_id,
                                       weft_wcr1_region_t* out,
                                       char* why, size_t whylen) {
    if (!out) return WEFT_WCR1_REFUSE_NULL;
    memset(out, 0, sizeof(*out));
    if (!m || !m->base) {
        refuse(why, whylen, "NULL shm map (rung: null)");
        return WEFT_WCR1_REFUSE_NULL;
    }

    const size_t payload_bytes = weft_shm_payload_bytes(m);
    const unsigned slots = weft_shm_slot_count(m);

    // Chunk k IS slot k's payload area. The RFC-0004 layout (fanout.h):
    // mapping = 64-byte WFSH header + 16 ctrl + 8M stamps + M payloads.
    weft_wcr1_region_t r = {
        .base = m->base,
        .span = (uint64_t)m->mapping_bytes,
        .chunk0_offset =
            (uint64_t)WEFT_SHM_HEADER_BYTES + 16 + 8ull * slots,
        .chunk_size = (uint32_t)payload_bytes,
        .chunk_count = slots,
        .node_id = node_id,
        .flags = WEFT_WCR1_F_SHM_BACKED,
    };

    const weft_wcr1_refusal_t rung = weft_wcr1_validate(&r, why, whylen);
    if (rung != WEFT_WCR1_OK) return rung;
    // The bridge demands the mapping be page-aligned in span (UMEM/MR
    // register [base, base+span) — a partial trailing page is legal for
    // RDMA but NOT for UMEM; refuse unless the mapping fills its pages).
    if ((r.span % WEFT_WCR1_PAGE_ALIGN) != 0) {
        refuse(why, whylen,
               "WFSH mapping span not page-multiple (rung: span_geometry)");
        return WEFT_WCR1_REFUSE_SPAN_GEOMETRY;
    }
    *out = r;
    return WEFT_WCR1_OK;
}

size_t weft_wcr1_report(const weft_wcr1_region_t* r, char* buf,
                        size_t buflen) {
    if (!r) return 0;
    int n = snprintf(buf, buflen,
                     "wcr1: base=%p span=%llu chunk0=%llu chunk=%u x%u "
                     "node=%u pinned=%d shm=%d",
                     (void*)r->base, (unsigned long long)r->span,
                     (unsigned long long)r->chunk0_offset, r->chunk_size,
                     r->chunk_count, r->node_id,
                     !!(r->flags & WEFT_WCR1_F_PINNED),
                     !!(r->flags & WEFT_WCR1_F_SHM_BACKED));
    return n > 0 ? (size_t)n : 0;
}
