// fanout_simd.h — SIMD-accelerated claim copy for the RFC-0004 fan-out ring
//
// WHY EXISTS (issue #17 task 1, "Zero-Copy Fan-Out (SIMD)"): the reader's
// claim path copies the freshest slot into the reader's pre-allocated buffer
// one u32 at a time (fanout.c: the relaxed-atomic word loop inside
// weft_fanout_claim). On every target we ship, a relaxed atomic load is a
// plain load — the loop is limited by instruction issue, not memory: 1 load +
// 1 store + pointer bump per 4 bytes. AVX-512 does 16 words per instruction
// pair; AVX-512/AVX2/NEON widen the copy to 64B/32B/16B per instruction and
// the claim's copy phase becomes bandwidth-bound instead of issue-bound.
//
// PROTOCOL SAFETY (why a vector load is legal inside a seqlock bracket):
//   The fan-out ring is a stamped seqlock (fanout.h P1/P2). The claim copy
//   is bracketed by the stamp validation BEFORE and a SeqCst fence +
//   stamp revalidation AFTER — ANY torn observation, however read, is
//   detected and the claim retries on a newer frame. A 256/512-bit load is
//   the same x86 'mov'/AArch64 'ldp' the scalar loop retires as, only wider;
//   it introduces no new ordering (the revalidation fence already covers
//   the whole copy) and no new tearing mode (per-word tearing was already
//   possible and is what the bracket exists to catch). This is the same
//   discipline RFC-0012's weft_turbo_fill established for the writer side
//   (SSE2 non-temporal stores into the slot under the P1 bracket).
//
//   FORMAL HONESTY NOTE: C11 formally reserves accesses to _Atomic objects
//   to atomic lvalues; the vector paths read the slot's word array through
//   non-atomic lvalues exactly as turbo_fill writes it. The scalar path
//   (and every build under -fsanitize=thread) stays on atomic loads; the
//   TSAN build additionally pins the dispatcher to scalar so the sanitizer
//   never sees a formally-racy access the protocol proves safe but TSAN
//   cannot (TSAN does not model seqlock brackets).
//
// DISPATCH (the sha256_hw.c/blend_q12.c house pattern — no ifunc):
//   compile-time target attributes per implementation, runtime CPU probe
//   resolved ONCE into a relaxed-atomic function pointer (benign duplicate
//   store on races), force pins for A/B benches and the bit-identity gate,
//   and an honest impl-name string for evidence lines. x86-64: avx512f >
//   avx2 > sse2 > scalar. aarch64: neon (compile-gated __ARM_NEON) >
//   scalar. Everywhere else: scalar — a copy that cannot prove its wider
//   path stays on the byte-identical reference (Law 4).
//
// BYTE-IDENTITY CONTRACT (the acceptance gate, FS2): every compiled-in
//   implementation must produce bit-identical destination bytes to the
//   scalar reference across the full size sweep (4 B .. 64 KiB+), all
//   word offsets, and noise-filled sources — the same all-paths-equal
//   discipline blend_q12's golden selftest enforces. weft_fanout_claim
//   gains the dispatching copy behind a seam; -DWEFT_FANOUT_SIMD_DISABLE=1
//   restores the literal inline scalar loop (byte-exact legacy builds and
//   bisection).
//
// LAW 2: the copy allocates nothing and touches only [dst, dst+words) and
//        [src, src+words). LAW 1: no loops beyond the copy itself.

#ifndef WEFT_FANOUT_SIMD_H
#define WEFT_FANOUT_SIMD_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Implementation selector values (also the force-pins' name strings).
typedef enum {
    WEFT_FANOUT_COPY_SCALAR = 0,
    WEFT_FANOUT_COPY_SSE2   = 1,
    WEFT_FANOUT_COPY_AVX2   = 2,
    WEFT_FANOUT_COPY_AVX512 = 3,
    WEFT_FANOUT_COPY_NEON   = 4
} weft_fanout_copy_impl_t;

/// The copy kernel signature: `words` u32 words from the slot's atomic word
/// array into the reader's plain buffer. Exactly the loop it replaces.
typedef void (*weft_fanout_copy_fn)(uint32_t* dst, const _Atomic uint32_t* src,
                                    size_t words);

// ---------------------------------------------------------------------------
// Dispatch (resolved once; force pins for tests/benches)
// ---------------------------------------------------------------------------

/// Name of the implementation the dispatcher will use ("scalar", "sse2",
/// "avx2", "avx512", "neon"). Stable pointer for the lifetime of the process.
const char* weft_fanout_copy_active_impl(void);

/// True when `name` names an implementation compiled into this binary.
int weft_fanout_copy_impl_available(const char* name);

/// Pin the dispatcher to `name` (test/bench only). Returns 0, or -1 when the
/// name is unknown or not compiled in (the pin is unchanged on refusal).
int weft_fanout_copy_force_impl(const char* name);

/// Pin to the byte-identical scalar reference (also what TSAN builds resolve).
void weft_fanout_copy_force_scalar(void);

/// Return to the auto-resolved implementation (re-probes on next call).
void weft_fanout_copy_force_auto(void);

/// Below this many words the inline scalar loop beats ANY dispatch (the
/// indirect call costs more than 32 relaxed loads; microbench: scalar wins
/// through ~128 B, vectors win from 256 B — see the FS evidence). Tiny
/// payloads therefore never leave the claim's inlined path: zero regression
/// at the small end by construction.
#ifndef WEFT_FANOUT_COPY_INLINE_WORDS
#define WEFT_FANOUT_COPY_INLINE_WORDS 32
#endif

extern _Atomic weft_fanout_copy_fn _weft_fanout_copy_dispatch;
weft_fanout_copy_fn _weft_fanout_copy_resolve(void);

/// The dispatching copy. Hot path: tiny payloads (< INLINE_WORDS) run the
/// exact inline scalar loop the claim always ran — zero dispatch cost, zero
/// behavior delta; larger payloads take the resolved-once function pointer
/// (the branch predictor owns the call).
/// This is the ONLY function weft_fanout_claim calls (the fanout.c seam).
static inline void weft_fanout_copy_words(uint32_t* dst,
                                          const _Atomic uint32_t* src,
                                          size_t words) {
    if (__builtin_expect(words < (size_t)WEFT_FANOUT_COPY_INLINE_WORDS, 1)) {
        for (size_t w = 0; w < words; w++) {
            dst[w] = atomic_load_explicit(src + w, memory_order_relaxed);
        }
        return;
    }
    weft_fanout_copy_fn fn =
        atomic_load_explicit(&_weft_fanout_copy_dispatch, memory_order_relaxed);
    if (__builtin_expect(fn != NULL, 1)) {
        fn(dst, src, words);
        return;
    }
    // Cold: first call (or after force_auto) resolves and stores.
    fn = _weft_fanout_copy_resolve();
    fn(dst, src, words);
}

/// Always-dispatched variant (no tiny-payload inline shortcut): the
/// conformance/bench entry. FS2's byte-identity gate pins each vector impl
/// and calls THIS, so sub-threshold sizes still exercise the vector bodies
/// (the inline shortcut would make those checks vacuous).
void weft_fanout_copy_words_dispatched(uint32_t* dst,
                                       const _Atomic uint32_t* src,
                                       size_t words);

#ifdef __cplusplus
}
#endif

#endif // WEFT_FANOUT_SIMD_H
