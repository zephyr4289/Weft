// weft_simd.h — cross-architecture SIMD vector compute core (Pillar 5, D-52).
//
// WHY EXISTS: the spectrum driver pipeline's terminal engine — and the
// < 1 us graceful-degradation target of every vendor driver — is a CPU
// vector engine that must run the SAME transformation kernels bit-exactly
// on every silicon on Earth: AVX-512/AVX2 (x86_64), NEON/SVE2 (ARM64),
// RVV 1.0 (riscv64), and a guaranteed scalar path for legacy/budget
// architectures. "Bit-exact" here is a PROVEN property, not an aspiration:
//
// NORMATIVE KERNEL CONTRACTS (any implementation must compute exactly):
//
//   K1 NORMALIZE_F32:    dst[i] = (src[i] - f_min) * f_rs
//                        Two IEEE-754 operations per element, element-wise:
//                        no reduction, no reassociation — width-independent
//                        by construction. f_rs is the caller's ONE-TIME
//                        precomputed reciprocal span times scale (the ABI
//                        deliberately trades a per-element division for a
//                        single-multiply contract; the premultiplication
//                        is documented here so every language binding and
//                        every ISA agrees on the same rounding sequence).
//
//   K2 DELTA_ENCODE_U32: dst[0] = src[0] - seed;
//                        dst[i] = src[i] - src[i-1]          (i >= 1)
//                        DELTA_DECODE_U32: dst[i] = seed + sum_{j<=i} src[j]
//                        Unsigned 32-bit modular arithmetic (wraparound is
//                        DEFINED, not UB) — width-independent by
//                        construction; vector prefix-scans use block sums
//                        + block offsets, which are exact integer algebra.
//
//   K3 DOT_F32:          C[i,j] = strict ascending-k chain
//                                acc = fma(A[i,k], B[k,j], acc), k = 0..K-1
//                        ONE accumulator per output element, ONE fused
//                        multiply-add per k, in k order. Every target's
//                        fused op (vfmadd231ps/vfmaq_f32/svmla_f32/vfmacc_vv/
//                        C fmaf) is IEEE-754 single-rounding — the correctly
//                        rounded result of a*b+c is UNIQUE, so every ISA and
//                        every vector width produces identical bits.
//                        Lane-per-output register blocking changes WHICH
//                        outputs move together, never the per-output op
//                        sequence. K-split and pairwise summation are
//                        FORBIDDEN (they reassociate; they would break
//                        bit-exactness for a few percent of throughput —
//                        a trade this module refuses by law).
//
//   K4 SEQLOCK_CHECKSUM: Fletcher-32 over little-endian u16 word pairs of
//                        the buffer, then a splitmix-style avalanche mixing
//                        the OBSERVED seqlock stamp:
//                          N = bytes/2 (bytes must be even; else EINVAL)
//                          w_i   = LE u16 word i
//                          sum1  = sum_i w_i            (mod 65535)
//                          sum2  = sum_i (N - i) * w_i  (mod 65535)
//                          r     = (sum2 << 16) | sum1
//                          r    ^= stamp * 0x9E3779B9u;  r *= 0x85EBCA6Bu;
//                          r    ^= r >> 13;              r *= 0xC2B2AE35u;
//                          r    ^= r >> 16;              return r
//                        All u32 arithmetic — width-independent by
//                        construction. The stamp makes the digest a KEYED
//                        function of the seqlock version observed at read
//                        time: a torn read (different stamp) yields a
//                        different digest with avalanche properties.
//
//   FLETCHER VECTOR STRATEGY (the part that needs proof, since naive
//   deferred accumulation changes grouping): implementations process
//   C-word chunks (C = vector lanes per iteration, any C in [1, 1024]):
//                          sum1c = exact sum of chunk words     (<= C*65534)
//                          cross = C * sum1_run                 (mod-reduced)
//                          wsum2 = dot(chunk, [C, C-1, ..., 1]) (<= 65534*C(C+1)/2)
//                          sum2_run = fold(sum2_run + cross + wsum2)
//                          sum1_run = fold(sum1_run + sum1c)
//   where fold(s) = (s & 0xFFFF) + (s >> 16) applied until < 2^16.
//   EQUIVALENCE PROOF (why any chunking gives identical bits): 2^16 is
//   congruent to 1 (mod 65535), so fold preserves residues; the iterative
//   reference's running values are the canonical representative (in
//   [0, 65535]) of the exact running total — canonical(x) is a function of
//   x's residue alone (0 for exact zero, 65535 for positive multiples of
//   65535, the residue otherwise), and canonical(A) + B has the same
//   residue as A + B with zero-ness agreeing too, hence
//   fold(canonical(A) + B) == canonical(A + B). Induction over chunks
//   gives chunked == iterative, for ANY C. Bounds: C <= 1024 keeps every
//   intermediate < 2^32 (sum1c < 2^26, cross < 2^26, wsum2 < 2^31), so
//   nothing wraps before the fold. QED.
//
// DISPATCH (the sha256_hw.c / fanout_simd.c house pattern — no ifunc):
//   per-function target attributes inside each implementation file, ONE
//   runtime CPU probe resolved into relaxed-atomic function pointers,
//   force pins for A/B benches and the all-paths-equal oracle gate, and
//   honest impl-name strings for evidence lines. Resolution order on
//   x86_64: avx512 > avx2 > scalar; aarch64: sve2 > neon > scalar;
//   riscv64: rvv > scalar; everywhere else: scalar.
//
//   TSAN NOTE (deliberate difference from fanout_simd): these kernels read
//   PLAIN memory only (no _Atomic lvalues inside the vector bodies), so
//   TSAN builds dispatch NORMALLY — no scalar pin is required or applied.
//
// LAWS: no heap allocation anywhere in this core (it is the zero-allocation
// terminal engine); kernels touch ONLY [dst, dst+n) and [src, src+n);
// unaligned host pointers are legal on every path (loadu discipline).

#ifndef WEFT_SIMD_H
#define WEFT_SIMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// §1 Implementation selector (values are also the force-pin name strings)
// ===========================================================================

typedef enum {
    WEFT_SIMD_SCALAR = 0,   ///< guaranteed reference (normative oracle)
    WEFT_SIMD_AVX2   = 1,   ///< x86_64 256-bit (AVX2 + FMA)
    WEFT_SIMD_AVX512 = 2,   ///< x86_64 512-bit (AVX-512F/VL/BW + FMA)
    WEFT_SIMD_NEON   = 3,   ///< ARM64 128-bit NEON
    WEFT_SIMD_SVE2   = 4,   ///< ARM64 SVE2 (vector-length agnostic)
    WEFT_SIMD_RVV    = 5,   ///< RISC-V Vector 1.0 (vector-length agnostic)
    WEFT_SIMD_IMPL_COUNT
} weft_simd_impl_t;

/// Compile-time capability bitmask (what THIS binary carries).
#define WEFT_SIMD_HAS_SCALAR (1u << 0)
#define WEFT_SIMD_HAS_AVX2   (1u << 1)
#define WEFT_SIMD_HAS_AVX512 (1u << 2)
#define WEFT_SIMD_HAS_NEON   (1u << 3)
#define WEFT_SIMD_HAS_SVE2   (1u << 4)
#define WEFT_SIMD_HAS_RVV    (1u << 5)
uint32_t weft_simd_compiled_caps(void);

// ===========================================================================
// §2 Kernel signatures
// ===========================================================================

/// K1: dst[i] = (src[i] - f_min) * f_rs for i < elems. IN-PLACE legal
/// (dst == src). Unaligned pointers legal.
typedef void (*weft_simd_normalize_fn)(float* dst, const float* src,
                                       size_t elems, float f_min, float f_rs);

/// K2 encode: modular adjacent difference, seeded. IN-PLACE legal.
typedef void (*weft_simd_delta_enc_fn)(uint32_t* dst, const uint32_t* src,
                                       size_t elems, uint32_t seed);

/// K2 decode: modular prefix reconstruction, seeded. IN-PLACE legal.
typedef void (*weft_simd_delta_dec_fn)(uint32_t* dst, const uint32_t* src,
                                       size_t elems, uint32_t seed);

/// K3: row-major C[m,n] = A[m,k] . B[k,n] with strides lda >= n (A rows),
/// ldb >= n (B rows), ldc >= n (C rows). Strict ascending-k FMA chain per
/// output (see the normative contract above). C may NOT alias A or B.
typedef void (*weft_simd_dot_fn)(float* c, const float* a, const float* b,
                                 uint32_t m, uint32_t k, uint32_t n,
                                 uint32_t lda, uint32_t ldb, uint32_t ldc);

/// K4: seqlock-stamped Fletcher-32 digest (see the normative contract
/// above). bytes MUST be even. Returns the mixed u32 digest.
typedef uint32_t (*weft_simd_fletcher32_fn)(const void* data, size_t bytes,
                                            uint32_t seqlock_stamp);

// ===========================================================================
// §3 Dispatching entries (hot; resolve-once relaxed-atomic house pattern)
// ===========================================================================

void     weft_simd_normalize(float* dst, const float* src, size_t elems,
                             float f_min, float f_rs);
void     weft_simd_delta_encode(uint32_t* dst, const uint32_t* src,
                                size_t elems, uint32_t seed);
void     weft_simd_delta_decode(uint32_t* dst, const uint32_t* src,
                                size_t elems, uint32_t seed);
void     weft_simd_dot_f32(float* c, const float* a, const float* b,
                           uint32_t m, uint32_t k, uint32_t n,
                           uint32_t lda, uint32_t ldb, uint32_t ldc);
uint32_t weft_simd_seqlock_checksum(const void* data, size_t bytes,
                                    uint32_t seqlock_stamp);

// ===========================================================================
// §4 Introspection + force pins (tests/benches only; the "A/B" seams)
// ===========================================================================

/// Honest name of the implementation each kernel resolves to RIGHT NOW
/// ("scalar", "avx2", "avx512", "neon", "sve2", "rvv"). Stable pointer.
const char* weft_simd_active_impl_name(void);

/// True when `name` names an implementation compiled into this binary.
int weft_simd_impl_available(const char* name);

/// Pin the dispatcher to `name` (test/bench only). 0 on success, -1 when
/// unknown or not compiled in (pin unchanged on refusal — fail-closed).
int weft_simd_force_impl(const char* name);

/// Pin to the normative scalar reference (the oracle).
void weft_simd_force_scalar(void);

/// Return to the auto-resolved implementation (re-probes on next call).
void weft_simd_force_auto(void);

/// Direct per-kernel oracle access: the scalar reference functions, always
/// compiled, for the all-paths-equal batteries and golden fixtures.
void     weft_simd_scalar_normalize(float* dst, const float* src,
                                    size_t elems, float f_min, float f_rs);
void     weft_simd_scalar_delta_encode(uint32_t* dst, const uint32_t* src,
                                       size_t elems, uint32_t seed);
void     weft_simd_scalar_delta_decode(uint32_t* dst, const uint32_t* src,
                                       size_t elems, uint32_t seed);
void     weft_simd_scalar_dot_f32(float* c, const float* a, const float* b,
                                  uint32_t m, uint32_t k, uint32_t n,
                                  uint32_t lda, uint32_t ldb, uint32_t ldc);
uint32_t weft_simd_scalar_seqlock_checksum(const void* data, size_t bytes,
                                           uint32_t seqlock_stamp);

/// Enumerated impl access for the bench's A/B legs (returns the impl's
/// kernel table, or NULL when that impl is not compiled in).
typedef struct weft_simd_kernels_s {
    weft_simd_normalize_fn  normalize;
    weft_simd_delta_enc_fn  delta_encode;
    weft_simd_delta_dec_fn  delta_decode;
    weft_simd_dot_fn        dot_f32;
    weft_simd_fletcher32_fn seqlock_checksum;
    const char*             impl_name;
} weft_simd_kernels_t;

const weft_simd_kernels_t* weft_simd_impl_kernels(weft_simd_impl_t impl);

#ifdef __cplusplus
}
#endif

#endif // WEFT_SIMD_H
