/*
 * weft_adapter_checksum.c - Weft Pillar 6: SIMD checksum & frame
 *                            integrity engine.
 *
 * CRC-32C (Castagnoli / iSCSI profile):
 *   x86_64 : SSE4.2 CRC32 instruction via __builtin_ia32_crc32*
 *            - compile-time direct when __SSE4_2__/__CRC32__ is set
 *            - otherwise runtime dispatch (constructor CPU probe,
 *              target-attributed function keeps the binary portable)
 *   aarch64: ARMv8 CRC32C via __builtin_arm_crc32c* when
 *            __ARM_FEATURE_CRC32 is enabled at compile time
 *   always : portable slicing-by-8 fallback (weft_crc32c_tables.h),
 *            also exported as the reference oracle for cross-checks.
 *
 * Adler-32 (RFC 1950):
 *   x86_64 : SSE2 vectorized kernel - per 16-byte chunk:
 *              acc_s1 += sad_epu8(chunk, 0)           (byte sums)
 *              acc_w  += acc_s1 (BEFORE the sad add)  (age weighting)
 *              acc_dot += madd_epi16(unpacked chunk, [16..1] weights)
 *            provable identity (see D-61 section 5): after K chunks,
 *              s1 = s1_0 + hsum(acc_s1)
 *              s2 = s2_0 + 16*hsum(acc_w) + hsum(acc_dot)
 *            reduced mod 65521 every 256 chunks (4096 bytes) with
 *            lane bounds <= 2^27 (no overflow, signed-safe).
 *   else   : portable deferred-modulo kernel (5552-byte NMAX blocks,
 *            16x unrolled inner loop).
 *
 * Law 1: no allocation anywhere; Law 3: compiles clean with
 * -std=c11 -Wall -Wextra -Werror -pedantic on gcc and clang, x86_64
 * and aarch64; Law 4: public ABI frozen in weft_adapters.h.
 */
#include "weft_adapters.h"

#include <string.h>

#include "weft_crc32c_tables.h"

#if defined(__x86_64__) || defined(__i386__)
#  define WEFT_ARCH_X86 1
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(__SSE2__)
#  if defined(__SSE2__)
#    include <emmintrin.h>
#    define WEFT_ADLER_SSE2 1
#  endif
#endif

/* ================================================================== */
/* Big-endian helpers (WAF frame fields)                              */
/* ================================================================== */

static inline uint32_t weft_ck_load_be32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return v;
#else
    return __builtin_bswap32(v);
#endif
}

static inline void weft_ck_store_be32(uint8_t *p, uint32_t v)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    memcpy(p, &v, sizeof v);
#else
    uint32_t s = __builtin_bswap32(v);
    memcpy(p, &s, sizeof s);
#endif
}

/* ================================================================== */
/* CRC-32C: portable slicing-by-8 reference                           */
/* ================================================================== */

uint32_t weft_adapter_crc32c_sw_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t state = crc ^ 0xFFFFFFFFu;

    if (p == NULL && len != 0u) { return crc; }

    while (len >= 8u) {
        uint32_t one, two;
        memcpy(&one, p, 4);
        memcpy(&two, p + 4, 4);
        one ^= state;
        state = weft_crc32c_slice8[7][one & 0xFFu] ^
                weft_crc32c_slice8[6][(one >> 8) & 0xFFu] ^
                weft_crc32c_slice8[5][(one >> 16) & 0xFFu] ^
                weft_crc32c_slice8[4][(one >> 24) & 0xFFu] ^
                weft_crc32c_slice8[3][two & 0xFFu] ^
                weft_crc32c_slice8[2][(two >> 8) & 0xFFu] ^
                weft_crc32c_slice8[1][(two >> 16) & 0xFFu] ^
                weft_crc32c_slice8[0][(two >> 24) & 0xFFu];
        p += 8;
        len -= 8;
    }
    while (len > 0u) {
        state = (state >> 8) ^ weft_crc32c_slice8[0][(state ^ *p) & 0xFFu];
        ++p;
        --len;
    }
    return state ^ 0xFFFFFFFFu;
}

uint32_t weft_adapter_crc32c_sw(const void *data, size_t len)
{
    return weft_adapter_crc32c_sw_update(0u, data, len);
}

/* ================================================================== */
/* CRC-32C: hardware kernels + dispatch                               */
/* ================================================================== */

#if defined(WEFT_ARCH_X86)

#  if defined(__SSE4_2__) || defined(__CRC32__)
#    define WEFT_CRC32C_X86_STATIC 1
#  endif

/* SSE4.2 kernel. The target attribute keeps the containing object
 * portable (only this function body requires the extension). */
#  if !defined(WEFT_CRC32C_X86_STATIC)
__attribute__((target("sse4.2")))
#  endif
static uint32_t weft_crc32c_sse42_update(uint32_t crc, const uint8_t *p, size_t len)
{
    uint32_t state = crc ^ 0xFFFFFFFFu;

    while (len >= 8u) {
        uint64_t v;
        memcpy(&v, p, 8);
        state = (uint32_t)__builtin_ia32_crc32di((unsigned int)state,
                                                 (unsigned long long)v);
        p += 8;
        len -= 8;
    }
    if (len >= 4u) {
        uint32_t v;
        memcpy(&v, p, 4);
        state = (uint32_t)__builtin_ia32_crc32si((unsigned int)state, v);
        p += 4;
        len -= 4;
    }
    if (len >= 2u) {
        uint16_t v;
        memcpy(&v, p, 2);
        state = (uint32_t)__builtin_ia32_crc32hi((unsigned int)state, v);
        p += 2;
        len -= 2;
    }
    if (len >= 1u) {
        state = (uint32_t)__builtin_ia32_crc32qi((unsigned int)state,
                                                 (unsigned int)*p);
    }
    return state ^ 0xFFFFFFFFu;
}

#  if !defined(WEFT_CRC32C_X86_STATIC)
static volatile int g_weft_crc32c_impl = (int)WEFT_CKSUM_IMPL_SLICE8;

__attribute__((constructor))
static void weft_crc32c_cpu_probe(void)
{
    if (__builtin_cpu_supports("sse4.2")) {
        g_weft_crc32c_impl = (int)WEFT_CKSUM_IMPL_X86_SSE42;
    }
}
#  endif

#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)

#  define WEFT_CRC32C_ARM_STATIC 1
static uint32_t weft_crc32c_sse42_update(uint32_t crc, const uint8_t *p, size_t len)
{
    uint32_t state = crc ^ 0xFFFFFFFFu;

    while (len >= 8u) {
        uint64_t v;
        memcpy(&v, p, 8);
        state = __builtin_arm_crc32cd(state, v);
        p += 8;
        len -= 8;
    }
    if (len >= 4u) {
        uint32_t v;
        memcpy(&v, p, 4);
        state = __builtin_arm_crc32cw(state, v);
        p += 4;
        len -= 4;
    }
    if (len >= 2u) {
        uint16_t v;
        memcpy(&v, p, 2);
        state = __builtin_arm_crc32ch(state, v);
        p += 2;
        len -= 2;
    }
    if (len >= 1u) {
        state = __builtin_arm_crc32cb(state, (uint32_t)*p);
    }
    return state ^ 0xFFFFFFFFu;
}

#endif /* arch dispatch */

uint32_t weft_adapter_crc32c_update(uint32_t crc, const void *data, size_t len)
{
    if (data == NULL && len != 0u) { return crc; }

#if defined(WEFT_CRC32C_X86_STATIC) || defined(WEFT_CRC32C_ARM_STATIC)
    return weft_crc32c_sse42_update(crc, (const uint8_t *)data, len);
#elif defined(WEFT_ARCH_X86)
    if (g_weft_crc32c_impl == (int)WEFT_CKSUM_IMPL_X86_SSE42) {
        return weft_crc32c_sse42_update(crc, (const uint8_t *)data, len);
    }
    return weft_adapter_crc32c_sw_update(crc, data, len);
#else
    return weft_adapter_crc32c_sw_update(crc, data, len);
#endif
}

uint32_t weft_adapter_crc32c(const void *data, size_t len)
{
    return weft_adapter_crc32c_update(0u, data, len);
}

int weft_adapter_crc32c_impl_id(void)
{
#if defined(WEFT_CRC32C_X86_STATIC)
    return (int)WEFT_CKSUM_IMPL_X86_SSE42;
#elif defined(WEFT_CRC32C_ARM_STATIC)
    return (int)WEFT_CKSUM_IMPL_ARM_CRC32;
#elif defined(WEFT_ARCH_X86)
    return g_weft_crc32c_impl;
#else
    return (int)WEFT_CKSUM_IMPL_SLICE8;
#endif
}

const char *weft_adapter_crc32c_impl_name(void)
{
    switch (weft_adapter_crc32c_impl_id()) {
    case WEFT_CKSUM_IMPL_X86_SSE42:
#if defined(WEFT_CRC32C_X86_STATIC)
        return "x86-sse4.2-static";
#else
        return "x86-sse4.2-rtdispatch";
#endif
    case WEFT_CKSUM_IMPL_ARM_CRC32:
        return "armv8-crc32-static";
    default:
        return "slice8-portable";
    }
}

/* ================================================================== */
/* Adler-32: portable deferred-modulo kernel                          */
/* ================================================================== */

#define WEFT_ADLER_MOD 65521u
#define WEFT_ADLER_NMAX 5552u

uint32_t weft_adapter_adler32_portable(uint32_t adler, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t s1 = adler & 0xFFFFu;
    uint32_t s2 = (adler >> 16) & 0xFFFFu;

    if (p == NULL && len != 0u) { return adler; }

    while (len > 0u) {
        size_t block = (len < WEFT_ADLER_NMAX) ? len : WEFT_ADLER_NMAX;
        len -= block;
        while (block >= 16u) {
            s1 += (uint32_t)p[0];  s2 += s1;
            s1 += (uint32_t)p[1];  s2 += s1;
            s1 += (uint32_t)p[2];  s2 += s1;
            s1 += (uint32_t)p[3];  s2 += s1;
            s1 += (uint32_t)p[4];  s2 += s1;
            s1 += (uint32_t)p[5];  s2 += s1;
            s1 += (uint32_t)p[6];  s2 += s1;
            s1 += (uint32_t)p[7];  s2 += s1;
            s1 += (uint32_t)p[8];  s2 += s1;
            s1 += (uint32_t)p[9];  s2 += s1;
            s1 += (uint32_t)p[10]; s2 += s1;
            s1 += (uint32_t)p[11]; s2 += s1;
            s1 += (uint32_t)p[12]; s2 += s1;
            s1 += (uint32_t)p[13]; s2 += s1;
            s1 += (uint32_t)p[14]; s2 += s1;
            s1 += (uint32_t)p[15]; s2 += s1;
            p += 16;
            block -= 16;
        }
        while (block > 0u) {
            s1 += (uint32_t)*p;
            s2 += s1;
            ++p;
            --block;
        }
        s1 %= WEFT_ADLER_MOD;
        s2 %= WEFT_ADLER_MOD;
    }
    return (s2 << 16) | s1;
}

/* ================================================================== */
/* Adler-32: SSE2 vectorized kernel                                   */
/* ================================================================== */

#if defined(WEFT_ADLER_SSE2)

/* Lane-bound proof (K <= 256 chunks per reduction block):
 *   acc_s1 lanes <= 2040 * 256                =      522,240
 *   acc_w  lanes <= 65520 + 2040 * 256^2 / 2  =   67,376,640  (< 2^26)
 *   16*hsum(acc_w) + hsum(acc_dot) + s2       < 2^31 (signed-safe)
 * Identity: s2_final = s2_0 + 16*hsum(acc_w) + hsum(acc_dot) mod 65521
 * where acc_w accumulates the s1-prefix BEFORE each chunk (age term,
 * including K*s1_0) and acc_dot holds the intra-chunk weighted sums
 * with byte weights [16,15,...,1]. */
#define WEFT_ADLER_SSE2_CHUNKS 256u

static inline uint32_t weft_hsum_epi32(__m128i v)
{
    __m128i r = _mm_add_epi32(v, _mm_srli_si128(v, 8));
    r = _mm_add_epi32(r, _mm_srli_si128(r, 4));
    return (uint32_t)_mm_cvtsi128_si32(r);
}

static uint32_t weft_adler32_sse2(uint32_t adler, const uint8_t *p, size_t len)
{
    uint32_t s1 = adler & 0xFFFFu;
    uint32_t s2 = (adler >> 16) & 0xFFFFu;

    if (p == NULL && len != 0u) { return adler; }

    while (len >= 16u) {
        const __m128i zero = _mm_setzero_si128();
        const __m128i wlo  = _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10, 9);
        const __m128i whi  = _mm_setr_epi16(8, 7, 6, 5, 4, 3, 2, 1);
        /* lane0 seeded with s1: the age term 16*K*s1_0 must enter
         * acc_w through acc_s1 (proof in D-61 section 5). */
        __m128i acc_s1 = _mm_cvtsi32_si128((int)s1);
        __m128i acc_w  = _mm_setzero_si128();
        __m128i acc_dot = _mm_setzero_si128();
        size_t chunks = len / 16u;
        if (chunks > WEFT_ADLER_SSE2_CHUNKS) { chunks = WEFT_ADLER_SSE2_CHUNKS; }

        for (size_t i = 0; i < chunks; ++i) {
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)p);
            __m128i sad = _mm_sad_epu8(v, zero);
            acc_w = _mm_add_epi32(acc_w, acc_s1);       /* age term  */
            acc_s1 = _mm_add_epi32(acc_s1, sad);        /* byte sums */
            acc_dot = _mm_add_epi32(acc_dot,
                        _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi8(v, zero), wlo),
                                      _mm_madd_epi16(_mm_unpackhi_epi8(v, zero), whi)));
            p += 16;
        }
        len -= chunks * 16u;

        s1 = weft_hsum_epi32(acc_s1) % WEFT_ADLER_MOD;  /* incl. s1_0 */
        s2 = (s2 + 16u * weft_hsum_epi32(acc_w) + weft_hsum_epi32(acc_dot))
             % WEFT_ADLER_MOD;
    }

    /* tail (< 16 bytes): classic scalar, immediate-mod */
    while (len > 0u) {
        s1 = (s1 + (uint32_t)*p) % WEFT_ADLER_MOD;
        s2 = (s2 + s1) % WEFT_ADLER_MOD;
        ++p;
        --len;
    }
    return (s2 << 16) | s1;
}

uint32_t weft_adapter_adler32_update(uint32_t adler, const void *data, size_t len)
{
    return weft_adler32_sse2(adler, (const uint8_t *)data, len);
}

const char *weft_adapter_adler32_impl_name(void)
{
    return "x86-sse2-madd";
}

#else /* !WEFT_ADLER_SSE2 */

uint32_t weft_adapter_adler32_update(uint32_t adler, const void *data, size_t len)
{
    return weft_adapter_adler32_portable(adler, data, len);
}

const char *weft_adapter_adler32_impl_name(void)
{
    return "portable-deferred-mod";
}

#endif /* WEFT_ADLER_SSE2 */

uint32_t weft_adapter_adler32(const void *data, size_t len)
{
    return weft_adapter_adler32_update(1u, data, len);
}

/* ================================================================== */
/* Weft Adapter Frame (WAF) engine                                    */
/* ================================================================== */

int32_t weft_adapter_waf_scan(const uint8_t *buf, size_t len,
                              weft_waf_view_t *out)
{
    uint32_t magic, plen, wcrc, calc;

    if (!buf || !out) { return WEFT_ADAPTER_EINVAL; }
    if (((uintptr_t)buf) & 3u) { return WEFT_ADAPTER_EALIGN; }
    if (len < 12u) { return WEFT_ADAPTER_ETRUNC; }

    magic = weft_ck_load_be32(buf);
    if (magic != WEFT_WAF_MAGIC) { return WEFT_ADAPTER_EBADMSG; }

    plen = weft_ck_load_be32(buf + 4);
    if ((size_t)plen > len - 12u) { return WEFT_ADAPTER_ETRUNC; }

    wcrc = weft_ck_load_be32(buf + 8u + (size_t)plen);
    calc = weft_adapter_crc32c(buf + 8, (size_t)plen);
    if (wcrc != calc) { return WEFT_ADAPTER_ECRC; }

    out->payload     = buf + 8;
    out->payload_len = plen;
    out->crc_wire    = wcrc;
    out->crc_calc    = calc;
    return WEFT_ADAPTER_OK;
}

int32_t weft_adapter_waf_emit(uint8_t *WEFT_RESTRICT dst, size_t dst_len,
                              const void *WEFT_RESTRICT payload,
                              size_t payload_len)
{
    if (!dst || (!payload && payload_len != 0u)) {
        return WEFT_ADAPTER_EINVAL;
    }
    if (((uintptr_t)dst) & 3u) { return WEFT_ADAPTER_EALIGN; }
    if (dst_len < 12u + payload_len) { return WEFT_ADAPTER_ETRUNC; }

    weft_ck_store_be32(dst, WEFT_WAF_MAGIC);
    weft_ck_store_be32(dst + 4, (uint32_t)payload_len);
    if (payload_len > 0u) {
        memcpy(dst + 8, payload, payload_len);
    }
    weft_ck_store_be32(dst + 8u + payload_len,
                       weft_adapter_crc32c(payload, payload_len));
    return (int32_t)(12u + payload_len);
}
