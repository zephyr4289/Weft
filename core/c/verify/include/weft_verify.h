/* ---------------------------------------------------------------------------
 * weft_verify.h — Weft Pillar 8: formal memory-safety bounds & frozen ABI
 *
 * TERRITORY: core/c/verify/ (Pillar 8 directive, Engineer 1).
 * ROLE:      Compile-time proof macros and zero-overhead runtime boundary
 *            anchors shared by the whole Weft ecosystem. This header is a
 *            FROZEN C-ABI CONTRACT (Pillar 8 mandate §2.C): every symbol,
 *            enum value, struct size and macro below is normative. Changes
 *            require a new ABI major.
 *
 * LAWS OBEYED (see docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md):
 *   Law 1  — zero heap allocation: every function/macro in this header is
 *            allocation-free by construction (no printf, no malloc, no
 *            libc machinery). The only side effect a violation can cause is
 *            one atomic increment of the violation counter in .data.
 *   Law 2  — the counter word is padded to a full cache line group to keep
 *            the cold path from sharing a line with hot reader state.
 *   Law 3  — compiles clean under `gcc -std=c11 -Wall -Wextra -Werror
 *            -pedantic` and `clang` with the same flags, plus g++/clang++
 *            `-std=c++17` (extern "C" guarded).
 *   Law 4  — ABI freeze: sizeof/alignment of every exposed type is pinned
 *            with static asserts in this header AND mirrored by
 *            tests/verify/core/test_abi_cpp17.cpp from the C++17 side.
 *
 * USAGE:
 *   WEFT_STATIC_ASSERT(cond, "msg")            file-scope compile-time proof
 *   WEFT_BOUNDS_CHECK_CONST(i, n)              constant-index compile proof
 *   WEFT_PROVE_ALIGNED_CONST(p, 64)            constant-address align proof
 *   WEFT_BOUNDS_CHECK(i, n)                    runtime guard (folds away at
 *                                               -O2 when arguments constant)
 *   WEFT_PROVE_ALIGNED(p, 64)                  runtime alignment proof
 *   weft_verify_ring_index/span/...            frozen out-of-line ABI twins
 *
 *   Define WEFT_VERIFY_DISABLE before including to compile every fast path
 *   down to the constant WEFT_VERIFY_OK (release "unchecked" mode). The
 *   out-of-line ABI symbols remain exported either way.
 * ------------------------------------------------------------------------- */
#ifndef WEFT_VERIFY_H
#define WEFT_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ABI identity                                                        */
/* ------------------------------------------------------------------ */

#define WEFT_VERIFY_ABI_MAJOR 1u
#define WEFT_VERIFY_ABI_MINOR 0u
#define WEFT_VERIFY_ABI_VERSION ((WEFT_VERIFY_ABI_MAJOR << 8u) | WEFT_VERIFY_ABI_MINOR)

/* Frozen status ladder. Numeric values are normative; append-only. */
typedef enum weft_verify_status {
    WEFT_VERIFY_OK        = 0,  /* proof discharged                       */
    WEFT_VERIFY_EBOUNDS   = 1,  /* index/start outside its region         */
    WEFT_VERIFY_EMISALIGN = 2,  /* address not aligned to required width  */
    WEFT_VERIFY_ESTRIDE   = 3,  /* DMA stride smaller than element size   */
    WEFT_VERIFY_EOVERFLOW = 4,  /* span/offset sum overflows its region   */
    WEFT_VERIFY_EINVALID  = 5   /* checker argument itself is malformed   */
} weft_verify_status_t;

/* ------------------------------------------------------------------ */
/* Compile-time proof macros                                           */
/* ------------------------------------------------------------------ */

#if defined(__cplusplus)
#  if __cplusplus >= 201103L
#    define WEFT_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#  else
#    error "weft_verify.h requires C++11 or newer"
#  endif
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define WEFT_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
   /* C99 fallback: negative array typedef fails to compile */
#  define WEFT_SA_GLUE_(a, b)  a##b
#  define WEFT_SA_GLUE(a, b)   WEFT_SA_GLUE_(a, b)
#  define WEFT_STATIC_ASSERT(cond, msg) \
      typedef char WEFT_SA_GLUE(weft_static_assert_line_, __LINE__)[(cond) ? 1 : -1]
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define WEFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define WEFT_UNLIKELY(x) (x)
#endif

#define WEFT_VERIFY_IS_POW2(x) \
    ((x) != 0 && (((x) & ((x) - 1)) == 0))

/* Constant-flavoured proofs: fail the BUILD, not the run. */
#define WEFT_BOUNDS_CHECK_CONST(idx, limit) \
    WEFT_STATIC_ASSERT((uint64_t)(idx) < (uint64_t)(limit), \
                       "WEFT_BOUNDS_CHECK_CONST: constant index out of bounds")

#define WEFT_PROVE_ALIGNED_CONST(addr, align) \
    WEFT_STATIC_ASSERT((((uintptr_t)(addr)) & (((uintptr_t)(align)) - 1u)) == 0u, \
                       "WEFT_PROVE_ALIGNED_CONST: constant address misaligned")

/* ------------------------------------------------------------------ */
/* Frozen ABI surface (out-of-line twins live in weft_verify_bounds.c) */
/* ------------------------------------------------------------------ */

/* Version of the ABI this library was built with (== WEFT_VERIFY_ABI_VERSION). */
uint32_t weft_verify_abi_version(void);

/* Number of boundary violations recorded since process start (cold path). */
uint64_t weft_verify_violation_count(void);

/* Self-check: verifies the frozen layout assertions at runtime.
 * Returns WEFT_VERIFY_OK, always (asserts are compile-time; this merely
 * re-derives them so linkers/reflection can observe the ABI is live). */
weft_verify_status_t weft_verify_abi_selfcheck(void);

/* Ring indices: pos must index a slot of a ring of `capacity` entries. */
weft_verify_status_t weft_verify_ring_index(uint64_t pos, uint64_t capacity);

/* Ring spans: [start, start+len) must lie inside [0, capacity). */
weft_verify_status_t weft_verify_ring_span(uint64_t start, uint64_t len,
                                           uint64_t capacity);

/* Variable-length-field offsets: [field_off, field_off+field_size) must lie
 * inside a record of `record_size` bytes (overflow-safe by construction). */
weft_verify_status_t weft_verify_vlf_offset(uint64_t field_off,
                                            uint64_t field_size,
                                            uint64_t record_size);

/* DMA strides: stride >= elem_size and stride*count must fit in buf_len
 * (checked with division so no multiplication overflow can occur). */
weft_verify_status_t weft_verify_dma_stride(uint64_t stride, uint64_t elem_size,
                                            uint64_t count, uint64_t buf_len);

/* Alignment: (addr & (align-1)) == 0 and align must be a power of two. */
weft_verify_status_t weft_verify_alignment(uintptr_t addr, size_t align);

/* Raw index check (used by WEFT_BOUNDS_CHECK). */
weft_verify_status_t weft_verify_index(uint64_t idx, uint64_t limit);

/* Cold-path recorder invoked by the fast paths below. NEVER allocates. */
void weft_verify_report_violation(uint32_t code, const char *file,
                                  uint32_t line);

/* ------------------------------------------------------------------ */
/* Zero-overhead inline fast paths                                     */
/* ------------------------------------------------------------------ */

#if !defined(WEFT_VERIFY_DISABLE)

static inline weft_verify_status_t weft_verify_index_fast(uint64_t idx,
                                                          uint64_t limit)
{
    if (WEFT_UNLIKELY(idx >= limit)) {
        return WEFT_VERIFY_EBOUNDS;
    }
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_ring_span_fast(
    uint64_t start, uint64_t len, uint64_t capacity)
{
    if (WEFT_UNLIKELY(start > capacity)) {
        return WEFT_VERIFY_EBOUNDS;
    }
    if (WEFT_UNLIKELY(len > capacity - start)) {
        return WEFT_VERIFY_EOVERFLOW;
    }
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_vlf_offset_fast(
    uint64_t field_off, uint64_t field_size, uint64_t record_size)
{
    if (WEFT_UNLIKELY(field_off > record_size)) {
        return WEFT_VERIFY_EBOUNDS;
    }
    if (WEFT_UNLIKELY(field_size > record_size - field_off)) {
        return WEFT_VERIFY_EOVERFLOW;
    }
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_dma_stride_fast(
    uint64_t stride, uint64_t elem_size, uint64_t count, uint64_t buf_len)
{
    if (WEFT_UNLIKELY(stride < elem_size)) {
        return WEFT_VERIFY_ESTRIDE;
    }
    if (WEFT_UNLIKELY(count > 0 && buf_len / count < stride)) {
        return WEFT_VERIFY_EOVERFLOW;
    }
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_alignment_fast(uintptr_t addr,
                                                              size_t align)
{
    if (WEFT_UNLIKELY(!WEFT_VERIFY_IS_POW2(align))) {
        return WEFT_VERIFY_EINVALID;
    }
    if (WEFT_UNLIKELY((addr & ((uintptr_t)align - 1u)) != 0u)) {
        return WEFT_VERIFY_EMISALIGN;
    }
    return WEFT_VERIFY_OK;
}

/* Checked fast paths: identical to the _fast twins but record the violation
 * (cold, atomic counter increment only) before returning the status. */
static inline weft_verify_status_t weft_verify_index_checked(
    uint64_t idx, uint64_t limit, const char *file, uint32_t line)
{
    if (WEFT_UNLIKELY(idx >= limit)) {
        weft_verify_report_violation((uint32_t)WEFT_VERIFY_EBOUNDS, file, line);
        return WEFT_VERIFY_EBOUNDS;
    }
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_alignment_checked(
    uintptr_t addr, size_t align, const char *file, uint32_t line)
{
    weft_verify_status_t st = weft_verify_alignment_fast(addr, align);
    if (WEFT_UNLIKELY(st != WEFT_VERIFY_OK)) {
        weft_verify_report_violation((uint32_t)st, file, line);
    }
    return st;
}

#define WEFT_BOUNDS_CHECK(idx, limit) \
    (weft_verify_index_checked((uint64_t)(idx), (uint64_t)(limit), \
                               __FILE__, (uint32_t)__LINE__) == \
     WEFT_VERIFY_OK)

#define WEFT_PROVE_ALIGNED(addr, align) \
    (weft_verify_alignment_checked((uintptr_t)(addr), (size_t)(align), \
                                   __FILE__, (uint32_t)__LINE__) == \
     WEFT_VERIFY_OK)

#else  /* WEFT_VERIFY_DISABLE: every guard folds to the constant true. */

static inline weft_verify_status_t weft_verify_index_fast(
    uint64_t idx, uint64_t limit)
{
    (void)idx; (void)limit;
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_ring_span_fast(
    uint64_t start, uint64_t len, uint64_t capacity)
{
    (void)start; (void)len; (void)capacity;
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_vlf_offset_fast(
    uint64_t field_off, uint64_t field_size, uint64_t record_size)
{
    (void)field_off; (void)field_size; (void)record_size;
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_dma_stride_fast(
    uint64_t stride, uint64_t elem_size, uint64_t count, uint64_t buf_len)
{
    (void)stride; (void)elem_size; (void)count; (void)buf_len;
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_alignment_fast(
    uintptr_t addr, size_t align)
{
    (void)addr; (void)align;
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_index_checked(
    uint64_t idx, uint64_t limit, const char *file, uint32_t line)
{
    (void)idx; (void)limit; (void)file; (void)line;
    return WEFT_VERIFY_OK;
}

static inline weft_verify_status_t weft_verify_alignment_checked(
    uintptr_t addr, size_t align, const char *file, uint32_t line)
{
    (void)addr; (void)align; (void)file; (void)line;
    return WEFT_VERIFY_OK;
}

#define WEFT_BOUNDS_CHECK(idx, limit) \
    ((void)(idx), (void)(limit), 1)
#define WEFT_PROVE_ALIGNED(addr, align) \
    ((void)(addr), (void)(align), 1)

#endif /* WEFT_VERIFY_DISABLE */

/* ------------------------------------------------------------------ */
/* Frozen layout proofs (checked from C and mirrored from C++17)       */
/* ------------------------------------------------------------------ */

WEFT_STATIC_ASSERT(sizeof(weft_verify_status_t) == 4,
                   "frozen ABI: weft_verify_status_t must be 4 bytes");
WEFT_STATIC_ASSERT(WEFT_VERIFY_ABI_VERSION == 0x0100u,
                   "frozen ABI: version packing must be major<<8|minor");
WEFT_STATIC_ASSERT(WEFT_VERIFY_OK == 0,
                   "frozen ABI: WEFT_VERIFY_OK must be 0 (zero-overhead fast path)");

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFT_VERIFY_H */
