/* ---------------------------------------------------------------------------
 * weft_verify_bounds.c — Weft Pillar 8: frozen ABI twins & violation ledger
 *
 * TERRITORY: core/c/verify/src/ (Pillar 8 directive, Engineer 1).
 *
 * Every out-of-line function here is a FROZEN ABI symbol. They all delegate
 * to the zero-overhead inline fast paths in weft_verify.h so semantics are
 * single-sourced, and they never allocate (Law 1).
 *
 * The violation counter is the ONLY mutable state in this component. It is
 * aligned to 64 bytes (Law 2) so the cold-path word never shares a cache
 * line with any hot-plane reader/writer state, and updates use a lock-free
 * fetch-add so anchors stay safe under multi-threaded hosts.
 * ------------------------------------------------------------------------- */

#include "weft_verify.h"

#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Frozen layout proofs (this TU pins the C-side view of the ABI)      */
/* ------------------------------------------------------------------ */

WEFT_STATIC_ASSERT(sizeof(uint64_t) == 8, "frozen ABI: uint64_t must be 8 bytes");
WEFT_STATIC_ASSERT(sizeof(uint32_t) == 4, "frozen ABI: uint32_t must be 4 bytes");
WEFT_STATIC_ASSERT(sizeof(weft_verify_status_t) == 4,
                   "frozen ABI: status enum must be 4 bytes on this platform");
WEFT_STATIC_ASSERT(sizeof(void *) >= 4,
                   "frozen ABI: pointers must be at least 4 bytes");

/* ------------------------------------------------------------------ */
/* Violation ledger — one cache-line-aligned word (Law 2)              */
/* ------------------------------------------------------------------ */

static _Alignas(64) _Atomic uint64_t weft_verify_violations_ = 0;

uint64_t weft_verify_violation_count(void)
{
    return atomic_load_explicit(&weft_verify_violations_, memory_order_relaxed);
}

void weft_verify_report_violation(uint32_t code, const char *file, uint32_t line)
{
    /* Cold path: one lock-free increment. No allocation, no I/O (Law 1).
     * The code/file/line are observable by debuggers via these locals; we
     * deliberately avoid any table or string work on the hot plane. */
    (void)code;
    (void)file;
    (void)line;
    atomic_fetch_add_explicit(&weft_verify_violations_, 1u, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Frozen ABI twins — single-sourced semantics via the inline paths    */
/* ------------------------------------------------------------------ */

uint32_t weft_verify_abi_version(void)
{
    return WEFT_VERIFY_ABI_VERSION;
}

weft_verify_status_t weft_verify_abi_selfcheck(void)
{
    /* Re-derive every frozen fact the header asserts; a mismatch could only
     * come from a broken build, in which case this returns non-OK and the
     * G6 gate fails closed. */
    if (sizeof(weft_verify_status_t) != 4)      { return WEFT_VERIFY_EINVALID; }
    if (WEFT_VERIFY_ABI_VERSION != 0x0100u)     { return WEFT_VERIFY_EINVALID; }
    if (WEFT_VERIFY_OK != 0)                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_index_fast(0u, 1u) != WEFT_VERIFY_OK)          { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_index_fast(1u, 1u) != WEFT_VERIFY_EBOUNDS)     { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_ring_span_fast(0u, 8u, 8u) != WEFT_VERIFY_OK)  { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_ring_span_fast(8u, 8u, 8u) != WEFT_VERIFY_EOVERFLOW)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_ring_span_fast(4u, 8u, 8u) != WEFT_VERIFY_EOVERFLOW)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_vlf_offset_fast(0u, 64u, 64u) != WEFT_VERIFY_OK)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_vlf_offset_fast(60u, 8u, 64u) != WEFT_VERIFY_EOVERFLOW)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_dma_stride_fast(64u, 64u, 10u, 640u) != WEFT_VERIFY_OK)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_dma_stride_fast(32u, 64u, 10u, 640u) != WEFT_VERIFY_ESTRIDE)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_alignment_fast(64u, 64u) != WEFT_VERIFY_OK)   { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_alignment_fast(65u, 64u) != WEFT_VERIFY_EMISALIGN)
                                                                    { return WEFT_VERIFY_EINVALID; }
    if (weft_verify_alignment_fast(64u, 0u) != WEFT_VERIFY_EINVALID)
                                                                    { return WEFT_VERIFY_EINVALID; }
    return WEFT_VERIFY_OK;
}

weft_verify_status_t weft_verify_index(uint64_t idx, uint64_t limit)
{
    return weft_verify_index_fast(idx, limit);
}

weft_verify_status_t weft_verify_ring_index(uint64_t pos, uint64_t capacity)
{
    return weft_verify_index_fast(pos, capacity);
}

weft_verify_status_t weft_verify_ring_span(uint64_t start, uint64_t len,
                                           uint64_t capacity)
{
    return weft_verify_ring_span_fast(start, len, capacity);
}

weft_verify_status_t weft_verify_vlf_offset(uint64_t field_off,
                                            uint64_t field_size,
                                            uint64_t record_size)
{
    return weft_verify_vlf_offset_fast(field_off, field_size, record_size);
}

weft_verify_status_t weft_verify_dma_stride(uint64_t stride, uint64_t elem_size,
                                            uint64_t count, uint64_t buf_len)
{
    return weft_verify_dma_stride_fast(stride, elem_size, count, buf_len);
}

weft_verify_status_t weft_verify_alignment(uintptr_t addr, size_t align)
{
    return weft_verify_alignment_fast(addr, align);
}
