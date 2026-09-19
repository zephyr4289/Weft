// fanout_simd.c — SIMD claim-copy kernels + dispatch (see fanout_simd.h)
//
// Implementation ladder, each behind its own compile/runtime gate:
//   scalar  — the byte-identical reference: the exact relaxed-atomic word
//             loop that lived inline in weft_fanout_claim (fanout.c).
//   sse2    — x86-64 baseline: 4 words (16 B) per move (target("sse2")).
//   avx2    — 8 words (32 B) per move (target("avx2")).
//   avx512  — 16 words (64 B) per move (target("avx512f")); issue #17's
//             128-byte-line tier pays off here: one iteration per line.
//   neon    — aarch64: 4 words (16 B) per move (compile-gated __ARM_NEON).
// Epilogues (and the whole scalar path) stay on atomic loads so odd sizes
// keep the reference's exact access shape; the vector bodies use unaligned
// loadu/storeu because slot cursors are geometry-shifted (16 + 8M + k*pb),
// not vector-aligned by contract.

#include "fanout_simd.h"

#include <stdbool.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
  #define WEFT_FS_X86 1
  #include <immintrin.h>
#elif defined(__aarch64__) || defined(__ARM_NEON)
  #include <arm_neon.h>
  #define WEFT_FS_NEON_ARCH 1
#endif

// TSAN cannot model seqlock brackets: pin the whole dispatcher to the
// formally-race-free scalar path under -fsanitize=thread (documented in
// fanout_simd.h; the protocol argument lives there).
#if defined(__SANITIZE_THREAD__)
  #define WEFT_FS_TSAN 1
#endif

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------

static void fs_copy_scalar(uint32_t* dst, const _Atomic uint32_t* src,
                           size_t words) {
    for (size_t w = 0; w < words; w++) {
        dst[w] = atomic_load_explicit(src + w, memory_order_relaxed);
    }
}

#if WEFT_FS_X86
__attribute__((target("sse2")))
static void fs_copy_sse2(uint32_t* dst, const _Atomic uint32_t* src,
                         size_t words) {
    size_t w = 0;
    for (; w + 4 <= words; w += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src + w));
        _mm_storeu_si128((__m128i*)(dst + w), v);
    }
    for (; w < words; w++) {
        dst[w] = atomic_load_explicit(src + w, memory_order_relaxed);
    }
}

__attribute__((target("avx2")))
static void fs_copy_avx2(uint32_t* dst, const _Atomic uint32_t* src,
                         size_t words) {
    size_t w = 0;
    // 2x-unrolled 32-byte body: issue-limited loops retire best with two
    // independent load/store chains in flight.
    for (; w + 16 <= words; w += 16) {
        __m256i a = _mm256_loadu_si256((const __m256i*)(src + w));
        __m256i b = _mm256_loadu_si256((const __m256i*)(src + w + 8));
        _mm256_storeu_si256((__m256i*)(dst + w), a);
        _mm256_storeu_si256((__m256i*)(dst + w + 8), b);
    }
    for (; w + 8 <= words; w += 8) {
        __m256i a = _mm256_loadu_si256((const __m256i*)(src + w));
        _mm256_storeu_si256((__m256i*)(dst + w), a);
    }
    for (; w < words; w++) {
        dst[w] = atomic_load_explicit(src + w, memory_order_relaxed);
    }
}

__attribute__((target("avx512f")))
static void fs_copy_avx512(uint32_t* dst, const _Atomic uint32_t* src,
                           size_t words) {
    size_t w = 0;
    for (; w + 16 <= words; w += 16) {
        __m512i a = _mm512_loadu_si512((const void*)(src + w));
        _mm512_storeu_si512((void*)(dst + w), a);
    }
    for (; w < words; w++) {
        dst[w] = atomic_load_explicit(src + w, memory_order_relaxed);
    }
}
#endif // WEFT_FS_X86

#if WEFT_FS_NEON_ARCH
static void fs_copy_neon(uint32_t* dst, const _Atomic uint32_t* src,
                         size_t words) {
    size_t w = 0;
    for (; w + 8 <= words; w += 8) {
        uint32x4_t a = vld1q_u32((const uint32_t*)(src + w));
        uint32x4_t b = vld1q_u32((const uint32_t*)(src + w + 4));
        vst1q_u32(dst + w, a);
        vst1q_u32(dst + w + 4, b);
    }
    for (; w + 4 <= words; w += 4) {
        vst1q_u32(dst + w, vld1q_u32((const uint32_t*)(src + w)));
    }
    for (; w < words; w++) {
        dst[w] = atomic_load_explicit(src + w, memory_order_relaxed);
    }
}
#endif // WEFT_FS_NEON_ARCH

// ---------------------------------------------------------------------------
// Dispatch (sha256_hw.c house pattern: resolved once, force pins, no ifunc)
// ---------------------------------------------------------------------------

// File-scope declaration (the header's extern lives inside the inline
// wrapper's block scope; this TU needs it at file scope for both users).
extern weft_fanout_copy_fn _weft_fanout_copy_resolve(void);

_Atomic weft_fanout_copy_fn _weft_fanout_copy_dispatch = NULL;

static const struct {
    const char* name;
    weft_fanout_copy_impl_t id;
    weft_fanout_copy_fn fn;
    int compiled;   // 0 = not compiled in this build (probe refuses it)
    int probeable;  // 1 = needs a runtime CPU feature check
} fs_table[] = {
    { "scalar", WEFT_FANOUT_COPY_SCALAR, fs_copy_scalar, 1, 0 },
#if WEFT_FS_X86
    { "sse2",   WEFT_FANOUT_COPY_SSE2,   fs_copy_sse2,   1, 1 },
    { "avx2",   WEFT_FANOUT_COPY_AVX2,   fs_copy_avx2,   1, 1 },
    { "avx512", WEFT_FANOUT_COPY_AVX512, fs_copy_avx512, 1, 1 },
#endif
#if WEFT_FS_NEON_ARCH
    { "neon",   WEFT_FANOUT_COPY_NEON,   fs_copy_neon,   1, 0 },
#endif
};

enum { FS_TABLE_N = sizeof(fs_table) / sizeof(fs_table[0]) };

/// Runtime CPU feature gate per implementation. __builtin_cpu_supports
/// takes a literal only, so the ladder is spelled out. Under TSAN every
/// non-scalar path is refused (the header's protocol-vs-sanitizer note).
static bool fs_feature_ok(weft_fanout_copy_impl_t id) {
#if defined(WEFT_FS_TSAN)
    // TSAN does not model seqlock brackets: the vector bodies' wide loads
    // are formally-racy reads the protocol proves safe but the sanitizer
    // cannot — every non-scalar pin is refused so TSAN builds only ever
    // exercise the atomic-load reference (fanout_simd.h argument).
    return id == WEFT_FANOUT_COPY_SCALAR;
#elif WEFT_FS_X86
    switch (id) {
        case WEFT_FANOUT_COPY_AVX512: return __builtin_cpu_supports("avx512f") != 0;
        case WEFT_FANOUT_COPY_AVX2:   return __builtin_cpu_supports("avx2") != 0;
        case WEFT_FANOUT_COPY_SSE2:   return __builtin_cpu_supports("sse2") != 0;
        default:                      return true;
    }
#else
    (void)id;
    return true;
#endif
}

/// The auto policy: widest compiled-in path the CPU actually has. Under TSAN
/// everything but scalar is refused (see header). On non-x86/non-NEON builds
/// scalar is the only entry — a copy that cannot prove its wider path stays
/// on the reference (Law 4).
static weft_fanout_copy_fn fs_probe(void) {
#if WEFT_FS_TSAN
    return fs_copy_scalar;
#else
    for (int i = (int)FS_TABLE_N - 1; i > 0; i--) {  // widest first
        if (fs_table[i].compiled && fs_feature_ok(fs_table[i].id)) {
            return fs_table[i].fn;
        }
    }
    return fs_copy_scalar;
#endif
}

void weft_fanout_copy_words_dispatched(uint32_t* dst,
                                       const _Atomic uint32_t* src,
                                       size_t words) {
    const weft_fanout_copy_fn fn = _weft_fanout_copy_resolve();
    fn(dst, src, words);
}

weft_fanout_copy_fn _weft_fanout_copy_resolve(void) {
    weft_fanout_copy_fn fn =
        atomic_load_explicit(&_weft_fanout_copy_dispatch, memory_order_relaxed);
    if (fn != NULL) return fn;  // lost a benign race with another first-caller
    fn = fs_probe();
    atomic_store_explicit(&_weft_fanout_copy_dispatch, fn, memory_order_relaxed);
    return fn;
}

const char* weft_fanout_copy_active_impl(void) {
    const weft_fanout_copy_fn fn =
        _weft_fanout_copy_resolve();
    for (int i = 0; i < (int)FS_TABLE_N; i++) {
        if (fs_table[i].fn == fn) return fs_table[i].name;
    }
    return "?";
}

int weft_fanout_copy_impl_available(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < (int)FS_TABLE_N; i++) {
        if (strcmp(fs_table[i].name, name) == 0) return fs_table[i].compiled;
    }
    return 0;
}

int weft_fanout_copy_force_impl(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < (int)FS_TABLE_N; i++) {
        if (strcmp(fs_table[i].name, name) == 0) {
            if (!fs_table[i].compiled) return -1;
            if (!fs_feature_ok(fs_table[i].id)) {
                // Pinned but the silicon refuses: keep scalar, caller can
                // read active_impl to notice (honest refusal, Law 4).
                return -1;
            }
            atomic_store_explicit(&_weft_fanout_copy_dispatch, fs_table[i].fn,
                                  memory_order_relaxed);
            return 0;
        }
    }
    return -1;
}

void weft_fanout_copy_force_scalar(void) {
    atomic_store_explicit(&_weft_fanout_copy_dispatch, fs_copy_scalar,
                          memory_order_relaxed);
}

void weft_fanout_copy_force_auto(void) {
    atomic_store_explicit(&_weft_fanout_copy_dispatch, NULL,
                          memory_order_relaxed);
}
