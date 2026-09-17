// sha256_mb.c — Multi-buffer SHA-256: 8-way AVX2 / 4-way NEON / scalar lanes.
//
// WHY EXISTS (see sha256_mb.h): the batch decode-verify path hashes many
// INDEPENDENT short messages with one shared key; multi-buffer turns that
// serialization into vector-lane parallelism. The classic shape (Intel's
// multi-buffer SHA-256 and OpenSSL's sha256-mb descend from it) packs N
// streams' state words into the N dwords of a vector register.
//
// STRUCTURE — deliberately the scalar file's structure, lifted to lanes:
//   per group:   W[0..15]  scalar dword gather (transpose into cols[16][8])
//                W[16..63] vector message schedule
//                rounds    vector A..H with K[i] broadcast
//                feed-forward from the saved state; finished lanes snapshot
// This is NOT the register-resident rolling-window schedule of the tuned
// Intel kernels: W lives in a 2 KiB stack array and rounds load from it. The
// cost is measurable (see the bench evidence) and the win is review symmetry —
// every line below maps 1:1 onto sha256.c's scalar structure, which is what
// makes "bit-identical by construction" a claim a reviewer can check by eye.
//
// Provenance: the round/schedule equations are FIPS 180-4 §6.2.2 re-derived
// against sha256.c (the in-tree normative reference); the lane-packing layout
// (state word i of message j in dword j of register i) follows the published
// multi-buffer idiom. No third-party code was copied.
//
// Layer discipline: driver layer; weft.c/weft.h untouched; no allocation.
//
// Honesty boundary: AVX2 executable-verified here (VMB sweeps under both
// pins; ASAN clean). NEON 4-way is compile-guarded aarch64, NOT
// executable-tested on this x86_64 sandbox — declared (sha256_hw.c precedent).

#include "sha256_mb.h"

#include <stdatomic.h>
#include <string.h>

// The 64 round constants — same table as sha256.c (single source of truth
// would be nicer, but sha256.c's K is file-static by design; duplicating a
// FIPS constant table is the lesser evil vs widening Series-6's surface).
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};


// Lanes past their declared block count contribute a zero dword in the
// gather below: their register result is garbage after the snapshot, which
// the caller ignores.

static inline uint32_t load_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// ---------------------------------------------------------------------------
// x86-64: 8-way AVX2
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)

#if defined(__GNUC__)
#include <immintrin.h>
#endif

__attribute__((target("avx2")))
static void sha256_mb8_avx2(uint32_t state_out[8][8], const uint32_t state_in[8][8],
                            const uint8_t* const msg[8], const size_t nblocks[8]) {
    const uint8_t* cur[8];
    size_t rem[8];
    size_t maxblocks = 0;
    for (int j = 0; j < 8; j++) {
        cur[j] = msg[j];
        rem[j] = nblocks[j];
        if (nblocks[j] > maxblocks) maxblocks = nblocks[j];
        if (nblocks[j] == 0) {
            memcpy(state_out[j], state_in[j], 8 * sizeof(uint32_t));
        }
    }

    // State word i of lane j -> dword j of register i.
    __m256i A = _mm256_setr_epi32((int)state_in[0][0], (int)state_in[1][0], (int)state_in[2][0],
                                  (int)state_in[3][0], (int)state_in[4][0], (int)state_in[5][0],
                                  (int)state_in[6][0], (int)state_in[7][0]);
    __m256i B = _mm256_setr_epi32((int)state_in[0][1], (int)state_in[1][1], (int)state_in[2][1],
                                  (int)state_in[3][1], (int)state_in[4][1], (int)state_in[5][1],
                                  (int)state_in[6][1], (int)state_in[7][1]);
    __m256i C = _mm256_setr_epi32((int)state_in[0][2], (int)state_in[1][2], (int)state_in[2][2],
                                  (int)state_in[3][2], (int)state_in[4][2], (int)state_in[5][2],
                                  (int)state_in[6][2], (int)state_in[7][2]);
    __m256i D = _mm256_setr_epi32((int)state_in[0][3], (int)state_in[1][3], (int)state_in[2][3],
                                  (int)state_in[3][3], (int)state_in[4][3], (int)state_in[5][3],
                                  (int)state_in[6][3], (int)state_in[7][3]);
    __m256i E = _mm256_setr_epi32((int)state_in[0][4], (int)state_in[1][4], (int)state_in[2][4],
                                  (int)state_in[3][4], (int)state_in[4][4], (int)state_in[5][4],
                                  (int)state_in[6][4], (int)state_in[7][4]);
    __m256i F = _mm256_setr_epi32((int)state_in[0][5], (int)state_in[1][5], (int)state_in[2][5],
                                  (int)state_in[3][5], (int)state_in[4][5], (int)state_in[5][5],
                                  (int)state_in[6][5], (int)state_in[7][5]);
    __m256i G = _mm256_setr_epi32((int)state_in[0][6], (int)state_in[1][6], (int)state_in[2][6],
                                  (int)state_in[3][6], (int)state_in[4][6], (int)state_in[5][6],
                                  (int)state_in[6][6], (int)state_in[7][6]);
    __m256i H = _mm256_setr_epi32((int)state_in[0][7], (int)state_in[1][7], (int)state_in[2][7],
                                  (int)state_in[3][7], (int)state_in[4][7], (int)state_in[5][7],
                                  (int)state_in[6][7], (int)state_in[7][7]);

    // cols[k][j] = lane j's schedule dword k — the transpose that turns eight
    // strided block streams into sixteen contiguous vector loads.
    uint32_t cols[16][8] __attribute__((aligned(32)));
    // W[64] on the stack: 2 KiB, L1-resident (see the header's structure note).
    __m256i w[64] __attribute__((aligned(32)));

    for (size_t g = 0; g < maxblocks; g++) {
        for (int k = 0; k < 16; k++) {
            for (int j = 0; j < 8; j++) {
                cols[k][j] = (rem[j] > 0) ? load_be32(cur[j] + 4 * k) : 0;
            }
        }

        for (int k = 0; k < 16; k++) {
            w[k] = _mm256_load_si256((const __m256i*)&cols[k][0]);
        }
        for (int k = 16; k < 64; k++) {
            const __m256i m15 = w[k - 15];
            const __m256i m2 = w[k - 2];
            const __m256i s0 = _mm256_xor_si256(
                _mm256_xor_si256(
                    _mm256_or_si256(_mm256_srli_epi32(m15, 7), _mm256_slli_epi32(m15, 25)),
                    _mm256_or_si256(_mm256_srli_epi32(m15, 18), _mm256_slli_epi32(m15, 14))),
                _mm256_srli_epi32(m15, 3));
            const __m256i s1 = _mm256_xor_si256(
                _mm256_xor_si256(
                    _mm256_or_si256(_mm256_srli_epi32(m2, 17), _mm256_slli_epi32(m2, 15)),
                    _mm256_or_si256(_mm256_srli_epi32(m2, 19), _mm256_slli_epi32(m2, 13))),
                _mm256_srli_epi32(m2, 10));
            w[k] = _mm256_add_epi32(
                _mm256_add_epi32(w[k - 16], s0),
                _mm256_add_epi32(w[k - 7], s1));
        }

        const __m256i save[8] = {A, B, C, D, E, F, G, H};

        for (int i = 0; i < 64; i++) {
            const __m256i S1 = _mm256_xor_si256(
                _mm256_xor_si256(
                    _mm256_or_si256(_mm256_srli_epi32(E, 6), _mm256_slli_epi32(E, 26)),
                    _mm256_or_si256(_mm256_srli_epi32(E, 11), _mm256_slli_epi32(E, 21))),
                _mm256_or_si256(_mm256_srli_epi32(E, 25), _mm256_slli_epi32(E, 7)));
            const __m256i ch = _mm256_xor_si256(
                _mm256_and_si256(E, F), _mm256_andnot_si256(E, G));
            const __m256i t1 = _mm256_add_epi32(
                _mm256_add_epi32(_mm256_add_epi32(H, S1), ch),
                _mm256_add_epi32(_mm256_set1_epi32((int)K[i]), w[i]));
            const __m256i S0 = _mm256_xor_si256(
                _mm256_xor_si256(
                    _mm256_or_si256(_mm256_srli_epi32(A, 2), _mm256_slli_epi32(A, 30)),
                    _mm256_or_si256(_mm256_srli_epi32(A, 13), _mm256_slli_epi32(A, 19))),
                _mm256_or_si256(_mm256_srli_epi32(A, 22), _mm256_slli_epi32(A, 10)));
            const __m256i maj = _mm256_xor_si256(
                _mm256_xor_si256(_mm256_and_si256(A, B), _mm256_and_si256(A, C)),
                _mm256_and_si256(B, C));
            const __m256i t2 = _mm256_add_epi32(S0, maj);

            H = G; G = F; F = E;
            E = _mm256_add_epi32(D, t1);
            D = C; C = B; B = A;
            A = _mm256_add_epi32(t1, t2);
        }

        A = _mm256_add_epi32(A, save[0]);
        B = _mm256_add_epi32(B, save[1]);
        C = _mm256_add_epi32(C, save[2]);
        D = _mm256_add_epi32(D, save[3]);
        E = _mm256_add_epi32(E, save[4]);
        F = _mm256_add_epi32(F, save[5]);
        G = _mm256_add_epi32(G, save[6]);
        H = _mm256_add_epi32(H, save[7]);

        // Advance active lanes; snapshot lanes whose last block was group g.
        // Extraction is store-based (_mm256_extract_epi32 needs an immediate
        // selector): spill the eight state vectors once, gather lane j's
        // column — at most one spill per group, on the group where some lane
        // finishes.
        int any_snap = 0;
        for (int j = 0; j < 8; j++) {
            if (rem[j] > 0) {
                cur[j] += SHA256_BLOCK_LEN;
                rem[j]--;
                if (nblocks[j] == g + 1) any_snap = 1;
            }
        }
        if (any_snap) {
            // Store-based snapshot: vectors go INTO uint32_t rows via the
            // store intrinsics (the sha256_hw.c-endorsed direction — the
            // __m256i may_alias attribute covers vector accesses over scalar
            // objects, NOT scalar reads of vector objects; the reverse cast
            // is a TBAA violation that -O2 dead-store-eliminates).
            uint32_t st32[8][8] __attribute__((aligned(32)));
            _mm256_storeu_si256((__m256i*)&st32[0][0], A);
            _mm256_storeu_si256((__m256i*)&st32[1][0], B);
            _mm256_storeu_si256((__m256i*)&st32[2][0], C);
            _mm256_storeu_si256((__m256i*)&st32[3][0], D);
            _mm256_storeu_si256((__m256i*)&st32[4][0], E);
            _mm256_storeu_si256((__m256i*)&st32[5][0], F);
            _mm256_storeu_si256((__m256i*)&st32[6][0], G);
            _mm256_storeu_si256((__m256i*)&st32[7][0], H);
            for (int j = 0; j < 8; j++) {
                if (nblocks[j] == g + 1) {
                    for (int i = 0; i < 8; i++) {
                        state_out[j][i] = st32[i][j];
                    }
                }
            }
        }
    }
}

#endif  // __x86_64__

// ---------------------------------------------------------------------------
// aarch64: 4-way NEON (compile-guarded; NOT executable-tested on x86_64)
// ---------------------------------------------------------------------------

#if defined(__aarch64__)

#include <arm_neon.h>

static void sha256_mb4_neon(uint32_t state_out[4][8], const uint32_t state_in[4][8],
                            const uint8_t* const msg[4], const size_t nblocks[4]) {
    const uint8_t* cur[4];
    size_t rem[4];
    size_t maxblocks = 0;
    for (int j = 0; j < 4; j++) {
        cur[j] = msg[j];
        rem[j] = nblocks[j];
        if (nblocks[j] > maxblocks) maxblocks = nblocks[j];
        if (nblocks[j] == 0) {
            memcpy(state_out[j], state_in[j], 8 * sizeof(uint32_t));
        }
    }

    // State word i of lane j -> lane j of register i (strided gather; the
    // state_in rows are per-message, the register lanes are per-message too,
    // so word-i-across-messages is a stride-8 gather).
    uint32_t s[8][4] __attribute__((aligned(16)));
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 8; i++) s[i][j] = state_in[j][i];
    }
    uint32x4_t A = vld1q_u32(&s[0][0]);  // lane j = message j, word 0
    uint32x4_t B = vld1q_u32(&s[1][0]);
    uint32x4_t C = vld1q_u32(&s[2][0]);
    uint32x4_t D = vld1q_u32(&s[3][0]);
    uint32x4_t E = vld1q_u32(&s[4][0]);
    uint32x4_t F = vld1q_u32(&s[5][0]);
    uint32x4_t G = vld1q_u32(&s[6][0]);
    uint32x4_t H = vld1q_u32(&s[7][0]);

    uint32_t cols[16][4] __attribute__((aligned(16)));
    uint32x4_t w[64] __attribute__((aligned(16)));

    for (size_t g = 0; g < maxblocks; g++) {
        for (int k = 0; k < 16; k++) {
            for (int j = 0; j < 4; j++) {
                cols[k][j] = (rem[j] > 0) ? load_be32(cur[j] + 4 * k) : 0;
            }
        }
        for (int k = 0; k < 16; k++) {
            w[k] = vld1q_u32(&cols[k][0]);
        }
        for (int k = 16; k < 64; k++) {
            const uint32x4_t m15 = w[k - 15];
            const uint32x4_t m2 = w[k - 2];
            const uint32x4_t s0 = veorq_u32(
                veorq_u32(vorrq_u32(vshrq_n_u32(m15, 7), vshlq_n_u32(m15, 25)),
                          vorrq_u32(vshrq_n_u32(m15, 18), vshlq_n_u32(m15, 14))),
                vshrq_n_u32(m15, 3));
            const uint32x4_t s1 = veorq_u32(
                veorq_u32(vorrq_u32(vshrq_n_u32(m2, 17), vshlq_n_u32(m2, 15)),
                          vorrq_u32(vshrq_n_u32(m2, 19), vshlq_n_u32(m2, 13))),
                vshrq_n_u32(m2, 10));
            w[k] = vaddq_u32(vaddq_u32(w[k - 16], s0), vaddq_u32(w[k - 7], s1));
        }

        const uint32x4_t save[8] = {A, B, C, D, E, F, G, H};

        for (int i = 0; i < 64; i++) {
            const uint32x4_t S1 = veorq_u32(
                veorq_u32(vorrq_u32(vshrq_n_u32(E, 6), vshlq_n_u32(E, 26)),
                          vorrq_u32(vshrq_n_u32(E, 11), vshlq_n_u32(E, 21))),
                vorrq_u32(vshrq_n_u32(E, 25), vshlq_n_u32(E, 7)));
            const uint32x4_t ch = veorq_u32(vandq_u32(E, F), vbicq_u32(G, E));
            const uint32x4_t t1 = vaddq_u32(
                vaddq_u32(vaddq_u32(H, S1), ch),
                vaddq_u32(vdupq_n_u32(K[i]), w[i]));
            const uint32x4_t S0 = veorq_u32(
                veorq_u32(vorrq_u32(vshrq_n_u32(A, 2), vshlq_n_u32(A, 30)),
                          vorrq_u32(vshrq_n_u32(A, 13), vshlq_n_u32(A, 19))),
                vorrq_u32(vshrq_n_u32(A, 22), vshlq_n_u32(A, 10)));
            const uint32x4_t maj = veorq_u32(
                veorq_u32(vandq_u32(A, B), vandq_u32(A, C)), vandq_u32(B, C));
            const uint32x4_t t2 = vaddq_u32(S0, maj);

            H = G; G = F; F = E;
            E = vaddq_u32(D, t1);
            D = C; C = B; B = A;
            A = vaddq_u32(t1, t2);
        }

        A = vaddq_u32(A, save[0]);
        B = vaddq_u32(B, save[1]);
        C = vaddq_u32(C, save[2]);
        D = vaddq_u32(D, save[3]);
        E = vaddq_u32(E, save[4]);
        F = vaddq_u32(F, save[5]);
        G = vaddq_u32(G, save[6]);
        H = vaddq_u32(H, save[7]);

        // Same store-based snapshot as the AVX2 path (vgetq_lane_u32 needs an
        // immediate selector too).
        int any_snap = 0;
        for (int j = 0; j < 4; j++) {
            if (rem[j] > 0) {
                cur[j] += SHA256_BLOCK_LEN;
                rem[j]--;
                if (nblocks[j] == g + 1) any_snap = 1;
            }
        }
        if (any_snap) {
            // Same store-based snapshot as the AVX2 path (scalar reads of
            // vector objects are a TBAA hazard there; vst1q into uint32_t
            // rows is the documented-intrinsic direction here).
            uint32_t st32[8][4] __attribute__((aligned(16)));
            vst1q_u32(&st32[0][0], A);
            vst1q_u32(&st32[1][0], B);
            vst1q_u32(&st32[2][0], C);
            vst1q_u32(&st32[3][0], D);
            vst1q_u32(&st32[4][0], E);
            vst1q_u32(&st32[5][0], F);
            vst1q_u32(&st32[6][0], G);
            vst1q_u32(&st32[7][0], H);
            for (int j = 0; j < 4; j++) {
                if (nblocks[j] == g + 1) {
                    for (int i = 0; i < 8; i++) {
                        state_out[j][i] = st32[i][j];
                    }
                }
            }
        }
    }
}

#endif  // __aarch64__

// ---------------------------------------------------------------------------
// Dispatch — the sha256.c pattern: resolve once, relaxed-atomically, benign
// duplicate stores; force_* re-pins for tests/bench A/B.
// ---------------------------------------------------------------------------

static weft_sha256_mb_impl_t mb_probe(void) {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        return WEFT_SHA256_MB_X86_AVX2;
    }
#endif
    return WEFT_SHA256_MB_NONE;
#elif defined(__aarch64__)
    return WEFT_SHA256_MB_ARM_NEON;  // NEON is architectural on aarch64
#else
    return WEFT_SHA256_MB_NONE;
#endif
}

static _Atomic int g_mb_forced_scalar;
static _Atomic int g_mb_resolved;  // 0 = not yet, 1 = yes (impl in g_mb_impl)
static _Atomic weft_sha256_mb_impl_t g_mb_impl;

static weft_sha256_mb_impl_t mb_resolve(void) {
    if (!atomic_load_explicit(&g_mb_resolved, memory_order_relaxed)) {
        const weft_sha256_mb_impl_t impl =
            atomic_load_explicit(&g_mb_forced_scalar, memory_order_relaxed)
                ? WEFT_SHA256_MB_NONE
                : mb_probe();
        atomic_store_explicit(&g_mb_impl, impl, memory_order_relaxed);
        atomic_store_explicit(&g_mb_resolved, 1, memory_order_relaxed);
    }
    return atomic_load_explicit(&g_mb_impl, memory_order_relaxed);
}

weft_sha256_mb_impl_t weft_sha256_mb_active_impl(void) {
    return mb_resolve();
}

int weft_sha256_mb_lanes(void) {
    switch (mb_resolve()) {
        case WEFT_SHA256_MB_X86_AVX2: return 8;
        case WEFT_SHA256_MB_ARM_NEON: return 4;
        default: return 0;
    }
}

void weft_sha256_mb_force_scalar(void) {
    atomic_store_explicit(&g_mb_forced_scalar, 1, memory_order_relaxed);
    atomic_store_explicit(&g_mb_impl, WEFT_SHA256_MB_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_mb_resolved, 1, memory_order_relaxed);
}

void weft_sha256_mb_force_auto(void) {
    atomic_store_explicit(&g_mb_forced_scalar, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mb_resolved, 0, memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The scalar lane loop — the normative reference. It goes through the
// Series-6 single-stream dispatch (sha256_update on a seeded ctx), so it
// honors weft_sha256_force_scalar and inherits SHA-NI/ARM-CE per lane when
// auto — a fair "best serial" baseline for the bench A/B, and a fully scalar
// reference under the Series-6 pin.
// ---------------------------------------------------------------------------

static void mb_lane_scalar(uint32_t out[8], const uint32_t in[8],
                           const uint8_t* msg, size_t nblocks) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    memcpy(ctx.h, in, 8 * sizeof(uint32_t));
    sha256_update(&ctx, msg, nblocks * SHA256_BLOCK_LEN);  // total_len unused here
    memcpy(out, ctx.h, 8 * sizeof(uint32_t));
}

int weft_sha256_mb(uint32_t state_out[/*lanes*/][8],
                   const uint32_t state_in[/*lanes*/][8],
                   const uint8_t* const msg[/*lanes*/],
                   const size_t nblocks[/*lanes*/],
                   int lanes) {
    const weft_sha256_mb_impl_t impl = mb_resolve();

    if (impl == WEFT_SHA256_MB_X86_AVX2) {
        if (lanes != 8) return -1;
#if defined(__x86_64__) || defined(_M_X64)
        sha256_mb8_avx2(state_out, state_in, msg, nblocks);
        return 0;
#endif
    }
#if defined(__aarch64__)
    if (impl == WEFT_SHA256_MB_ARM_NEON) {
        if (lanes != 4) return -1;
        sha256_mb4_neon(state_out, state_in, msg, nblocks);
        return 0;
    }
#endif

    if (lanes < 1 || lanes > WEFT_SHA256_MB_MAX_LANES) return -1;
    for (int j = 0; j < lanes; j++) {
        if (nblocks[j] == 0) {
            memcpy(state_out[j], state_in[j], 8 * sizeof(uint32_t));
        } else {
            mb_lane_scalar(state_out[j], state_in[j], msg[j], nblocks[j]);
        }
    }
    return 0;
}
