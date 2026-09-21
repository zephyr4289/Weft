// weft_simd.c — normative scalar oracle + the runtime dispatcher (Pillar 5, D-52).
//
// WHY EXISTS: the scalar kernels below are the NORMATIVE REFERENCE every
// other ISA implementation is proven against (the all-paths-equal oracle
// battery in tests/spectrum/native/). The dispatcher follows the
// sha256_hw.c / fanout_simd.c house pattern: ONE runtime CPU probe
// resolved into relaxed-atomic function pointers (resolve-once, no
// ifunc), force pins for A/B benches and the oracle gate, and honest
// impl-name strings for evidence lines.
//
// Bit-exactness argument per kernel (full contracts in weft_simd.h):
//   K1 normalize — two IEEE-754 ops per element, element-wise. The
//       sub-then-mul shape cannot be contracted into a single fused op
//       (FMA fuses mul-into-add, never sub-into-mul), so every width and
//       every auto-vectorized baseline build produces identical bits.
//   K2 delta codecs — unsigned 32-bit modular arithmetic: wraparound is
//       defined and exact integer algebra (block sums + block offsets)
//       makes any lane grouping produce identical bits.
//   K3 dot — strict ascending-k fma() chain per output element. IEEE-754
//       single-rounding fused multiply-add is UNIQUE: fmaf() ==
//       vfmadd231ps == vfmaq_f32 == svmla == vfmacc for each k step, and
//       the per-output op sequence is identical everywhere. K-split and
//       pairwise summation are forbidden by contract (they reassociate).
//   K4 seqlock checksum — the chunked Fletcher recurrence with the
//       canonical fold is proven chunk-size-invariant in weft_simd.h
//       (2^16 == 1 mod 65535; canonical representatives commute with the
//       fold), so C=8/16/32 lane groupings all reproduce the iterative
//       reference bit-for-bit.
//
// LAW: this file allocates NOTHING, ever. It is the zero-allocation
// terminal engine and the < 1 us degradation target of every vendor
// driver. The only mutable state is the six relaxed-atomic dispatch
// pointers (+ the force pin), which are cold-write / hot-read.
//
// TSAN: kernels read plain memory only — no _Atomic lvalues inside vector
// bodies — so TSAN builds dispatch NORMALLY (documented difference from
// fanout_simd's seqlock-bracketed bodies which must pin to scalar).

#include "weft_simd.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Per-arch implementation tables (weak-free: the arch TUs compile to empty
// translation units on foreign hosts, so the externs are guarded the same
// way the capability bits are).
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
#define WEFT_WS_X86 1
extern const weft_simd_kernels_t weft_simd_avx2_kernels;    // weft_simd_x86.c
extern const weft_simd_kernels_t weft_simd_avx512_kernels;  // weft_simd_x86.c
#elif defined(__aarch64__)
#define WEFT_WS_ARM64 1
extern const weft_simd_kernels_t weft_simd_neon_kernels;    // weft_simd_arm.c
#if defined(__ARM_FEATURE_SVE2)
extern const weft_simd_kernels_t weft_simd_sve2_kernels;    // weft_simd_arm.c
extern int weft_simd_arm_sve2_runtime(void);                // HWCAP probe
#endif
#elif defined(__riscv) && defined(__riscv_v_intrinsic)
#define WEFT_WS_RISCV 1
extern const weft_simd_kernels_t weft_simd_rvv_kernels;     // weft_simd_rvv.c
#endif

// ---------------------------------------------------------------------------
// K4 canonical fold (shared by every fletcher implementation; identical
// arithmetic everywhere: fold(s) = (s & 0xFFFF) + (s >> 16) until < 2^16).
// ---------------------------------------------------------------------------

static inline uint32_t ws_fold32(uint32_t s) {
    while (s > 0xFFFFu) {
        s = (s & 0xFFFFu) + (s >> 16);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Normative scalar kernels (the oracle)
// ---------------------------------------------------------------------------

void weft_simd_scalar_normalize(float* dst, const float* src, size_t elems,
                                float f_min, float f_rs) {
    for (size_t i = 0; i < elems; i++) {
        // Two roundings, never contracted: sub feeding mul has no fused form.
        dst[i] = (src[i] - f_min) * f_rs;
    }
}

void weft_simd_scalar_delta_encode(uint32_t* dst, const uint32_t* src,
                                    size_t elems, uint32_t seed) {
    if (elems == 0) {
        return;
    }
    uint32_t prev = src[0];
    dst[0] = prev - seed;   // modular u32: defined wraparound
    for (size_t i = 1; i < elems; i++) {
        const uint32_t cur = src[i];
        dst[i] = cur - prev;
        prev = cur;
    }
}

void weft_simd_scalar_delta_decode(uint32_t* dst, const uint32_t* src,
                                    size_t elems, uint32_t seed) {
    uint32_t acc = seed;    // modular: seed + sum_{j<=i} src[j]
    for (size_t i = 0; i < elems; i++) {
        acc = acc + src[i];
        dst[i] = acc;
    }
}

void weft_simd_scalar_dot_f32(float* c, const float* a, const float* b,
                              uint32_t m, uint32_t k, uint32_t n,
                              uint32_t lda, uint32_t ldb, uint32_t ldc) {
    for (uint32_t i = 0; i < m; i++) {
        const float* arow = a + (size_t)i * lda;
        float* crow = c + (size_t)i * ldc;
        for (uint32_t j = 0; j < n; j++) {
            // Strict ascending-k fused chain: ONE accumulator, ONE fma per k.
            // The compiler cannot reassociate this without -ffast-math.
            float acc = 0.0f;
            for (uint32_t kk = 0; kk < k; kk++) {
                acc = fmaf(arow[kk], b[(size_t)kk * ldb + j], acc);
            }
            crow[j] = acc;
        }
    }
}

uint32_t weft_simd_scalar_seqlock_checksum(const void* data, size_t bytes,
                                           uint32_t seqlock_stamp) {
    // Iterative reference: the canonical running representatives.
    const unsigned char* p = (const unsigned char*)data;
    const size_t words = bytes / 2;
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    for (size_t i = 0; i < words; i++) {
        uint16_t w;
        memcpy(&w, p + i * 2, sizeof(w));   // LE host; loadu discipline
        sum1 = ws_fold32(sum1 + (uint32_t)w);
        sum2 = ws_fold32(sum2 + sum1);
    }
    uint32_t r = (sum2 << 16) | sum1;
    // Keyed avalanche over the OBSERVED seqlock stamp (torn-read detector).
    r ^= seqlock_stamp * 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    r *= 0xC2B2AE35u;
    r ^= r >> 16;
    return r;
}

static const weft_simd_kernels_t ws_scalar_kernels = {
    .normalize        = weft_simd_scalar_normalize,
    .delta_encode     = weft_simd_scalar_delta_encode,
    .delta_decode     = weft_simd_scalar_delta_decode,
    .dot_f32          = weft_simd_scalar_dot_f32,
    .seqlock_checksum = weft_simd_scalar_seqlock_checksum,
    .impl_name        = "scalar",
};

// ---------------------------------------------------------------------------
// Compile-time capability mask + runtime probe (resolve-once)
// ---------------------------------------------------------------------------

uint32_t weft_simd_compiled_caps(void) {
    uint32_t caps = WEFT_SIMD_HAS_SCALAR;
#if WEFT_WS_X86
    caps |= WEFT_SIMD_HAS_AVX2 | WEFT_SIMD_HAS_AVX512;
#elif WEFT_WS_ARM64
    caps |= WEFT_SIMD_HAS_NEON;
#if defined(__ARM_FEATURE_SVE2)
    caps |= WEFT_SIMD_HAS_SVE2;
#endif
#elif WEFT_WS_RISCV
    caps |= WEFT_SIMD_HAS_RVV;
#endif
    return caps;
}

static weft_simd_impl_t ws_probe_best(void) {
    const uint32_t caps = weft_simd_compiled_caps();
#if WEFT_WS_X86
    // AVX-512 needs F + BW for this module's kernel set (vpmovzxwd/vpmulld
    // zmm forms in the fletcher; valignd/vpermd in the codecs).
    if ((caps & WEFT_SIMD_HAS_AVX512) != 0u &&
        __builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw")) {
        return WEFT_SIMD_AVX512;
    }
    if ((caps & WEFT_SIMD_HAS_AVX2) != 0u &&
        __builtin_cpu_supports("avx2") &&
        __builtin_cpu_supports("fma")) {
        return WEFT_SIMD_AVX2;
    }
    return WEFT_SIMD_SCALAR;
#elif WEFT_WS_ARM64
#if defined(__ARM_FEATURE_SVE2)
    if ((caps & WEFT_SIMD_HAS_SVE2) != 0u && weft_simd_arm_sve2_runtime()) {
        return WEFT_SIMD_SVE2;
    }
#endif
    if ((caps & WEFT_SIMD_HAS_NEON) != 0u) {
        return WEFT_SIMD_NEON;
    }
    return WEFT_SIMD_SCALAR;
#elif WEFT_WS_RISCV
    if ((caps & WEFT_SIMD_HAS_RVV) != 0u) {
        return WEFT_SIMD_RVV;   // the riscv shard compiles with -march=rv64gcv
    }
    return WEFT_SIMD_SCALAR;
#else
    (void)caps;
    return WEFT_SIMD_SCALAR;
#endif
}

// ---------------------------------------------------------------------------
// Dispatcher state (cold-write / hot-read relaxed atomics)
// ---------------------------------------------------------------------------

static _Atomic weft_simd_normalize_fn  g_normalize;
static _Atomic weft_simd_delta_enc_fn  g_delta_encode;
static _Atomic weft_simd_delta_dec_fn  g_delta_decode;
static _Atomic weft_simd_dot_fn        g_dot_f32;
static _Atomic weft_simd_fletcher32_fn g_fletcher;
static _Atomic int32_t                 g_impl = WEFT_SIMD_SCALAR;
static _Atomic int32_t                 g_forced = -1;   // -1 = auto

static const char* const ws_names[WEFT_SIMD_IMPL_COUNT] = {
    "scalar", "avx2", "avx512", "neon", "sve2", "rvv",
};

const weft_simd_kernels_t* weft_simd_impl_kernels(weft_simd_impl_t impl) {
    switch (impl) {
    case WEFT_SIMD_SCALAR:
        return &ws_scalar_kernels;
#if WEFT_WS_X86
    case WEFT_SIMD_AVX2:
        return &weft_simd_avx2_kernels;
    case WEFT_SIMD_AVX512:
        return &weft_simd_avx512_kernels;
#endif
#if WEFT_WS_ARM64
    case WEFT_SIMD_NEON:
        return &weft_simd_neon_kernels;
#if defined(__ARM_FEATURE_SVE2)
    case WEFT_SIMD_SVE2:
        return &weft_simd_sve2_kernels;
#endif
#endif
#if WEFT_WS_RISCV
    case WEFT_SIMD_RVV:
        return &weft_simd_rvv_kernels;
#endif
    default:
        return NULL;   // not compiled into this binary: honest NULL
    }
}

static void ws_resolve(void) {
    const int32_t forced = atomic_load_explicit(&g_forced, memory_order_relaxed);
    weft_simd_impl_t impl =
        (forced >= 0) ? (weft_simd_impl_t)forced : ws_probe_best();
    const weft_simd_kernels_t* kt = weft_simd_impl_kernels(impl);
    if (kt == NULL) {
        kt = &ws_scalar_kernels;   // forced-but-absent: fail-closed to oracle
        impl = WEFT_SIMD_SCALAR;
    }
    atomic_store_explicit(&g_impl, (int32_t)impl, memory_order_release);
    atomic_store_explicit(&g_normalize, kt->normalize, memory_order_release);
    atomic_store_explicit(&g_delta_encode, kt->delta_encode, memory_order_release);
    atomic_store_explicit(&g_delta_decode, kt->delta_decode, memory_order_release);
    atomic_store_explicit(&g_dot_f32, kt->dot_f32, memory_order_release);
    atomic_store_explicit(&g_fletcher, kt->seqlock_checksum, memory_order_release);
}

// ---------------------------------------------------------------------------
// Hot dispatch entries (one relaxed atomic read each; resolve-once)
// ---------------------------------------------------------------------------

void weft_simd_normalize(float* dst, const float* src, size_t elems,
                         float f_min, float f_rs) {
    weft_simd_normalize_fn f =
        atomic_load_explicit(&g_normalize, memory_order_relaxed);
    if (f == NULL) {
        ws_resolve();
        f = atomic_load_explicit(&g_normalize, memory_order_relaxed);
    }
    f(dst, src, elems, f_min, f_rs);
}

void weft_simd_delta_encode(uint32_t* dst, const uint32_t* src,
                            size_t elems, uint32_t seed) {
    weft_simd_delta_enc_fn f =
        atomic_load_explicit(&g_delta_encode, memory_order_relaxed);
    if (f == NULL) {
        ws_resolve();
        f = atomic_load_explicit(&g_delta_encode, memory_order_relaxed);
    }
    f(dst, src, elems, seed);
}

void weft_simd_delta_decode(uint32_t* dst, const uint32_t* src,
                            size_t elems, uint32_t seed) {
    weft_simd_delta_dec_fn f =
        atomic_load_explicit(&g_delta_decode, memory_order_relaxed);
    if (f == NULL) {
        ws_resolve();
        f = atomic_load_explicit(&g_delta_decode, memory_order_relaxed);
    }
    f(dst, src, elems, seed);
}

void weft_simd_dot_f32(float* c, const float* a, const float* b,
                       uint32_t m, uint32_t k, uint32_t n,
                       uint32_t lda, uint32_t ldb, uint32_t ldc) {
    weft_simd_dot_fn f = atomic_load_explicit(&g_dot_f32, memory_order_relaxed);
    if (f == NULL) {
        ws_resolve();
        f = atomic_load_explicit(&g_dot_f32, memory_order_relaxed);
    }
    f(c, a, b, m, k, n, lda, ldb, ldc);
}

uint32_t weft_simd_seqlock_checksum(const void* data, size_t bytes,
                                    uint32_t seqlock_stamp) {
    weft_simd_fletcher32_fn f =
        atomic_load_explicit(&g_fletcher, memory_order_relaxed);
    if (f == NULL) {
        ws_resolve();
        f = atomic_load_explicit(&g_fletcher, memory_order_relaxed);
    }
    return f(data, bytes, seqlock_stamp);
}

// ---------------------------------------------------------------------------
// Introspection + force pins (tests/benches only; single-threaded contract)
// ---------------------------------------------------------------------------

const char* weft_simd_active_impl_name(void) {
    if (atomic_load_explicit(&g_normalize, memory_order_relaxed) == NULL) {
        ws_resolve();
    }
    const int32_t impl = atomic_load_explicit(&g_impl, memory_order_relaxed);
    if (impl < 0 || impl >= WEFT_SIMD_IMPL_COUNT) {
        return "scalar";   // unreachable: fail-closed name
    }
    return ws_names[impl];
}

static int ws_name_to_impl(const char* name, weft_simd_impl_t* out) {
    if (name == NULL || out == NULL) {
        return -1;
    }
    for (int i = 0; i < WEFT_SIMD_IMPL_COUNT; i++) {
        if (strcmp(name, ws_names[i]) == 0) {
            *out = (weft_simd_impl_t)i;
            return 0;
        }
    }
    return -1;
}

int weft_simd_impl_available(const char* name) {
    weft_simd_impl_t impl;
    if (ws_name_to_impl(name, &impl) != 0) {
        return 0;
    }
    const uint32_t caps = weft_simd_compiled_caps();
    return (caps & (1u << impl)) != 0u;
}

int weft_simd_force_impl(const char* name) {
    weft_simd_impl_t impl;
    if (ws_name_to_impl(name, &impl) != 0) {
        return -1;
    }
    const uint32_t caps = weft_simd_compiled_caps();
    if ((caps & (1u << impl)) == 0u) {
        return -1;   // not compiled in: pin unchanged (fail-closed)
    }
    atomic_store_explicit(&g_forced, (int32_t)impl, memory_order_relaxed);
    ws_resolve();
    return 0;
}

void weft_simd_force_scalar(void) {
    atomic_store_explicit(&g_forced, (int32_t)WEFT_SIMD_SCALAR,
                          memory_order_relaxed);
    ws_resolve();
}

void weft_simd_force_auto(void) {
    atomic_store_explicit(&g_forced, -1, memory_order_relaxed);
    ws_resolve();
}
