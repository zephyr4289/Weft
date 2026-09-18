// verified_mb.c — VerifiedWeft multi-buffer batch verification (SIMD lanes).
//
// INNER HASH, per record: H(ipad || envelope[0..16) || payload). The ipad
// block is shared by every record under the key, so the state after it is
// computed once per stream (weft_vw_mb_init) — the multi-buffer lanes start
// there. Each lane then compresses its OWN env||payload||padding sequence.
//
// OUTER HASH, per record: H(opad || inner_digest). Same amortization: lanes
// start from the after-opad state and compress exactly one block
// (32-byte digest + 0x80 + zeros + BE(768)).
//
// PER-BATCH COST (payload <= WEFT_VW_MB_MAX_PAYLOAD, the flight-recorder hot
// case): two multi-buffer transform calls (inner tail + outer block) plus a
// staging memcpy of env||payload per record. Records with LARGER payloads
// fall back to the serial pre-keyed verifier — the pad-block amortization
// they give up is negligible at that size, and the stack staging stays
// bounded (declared, Law-4 style, not hidden).
//
// SEMANTIC EQUIVALENCE to weft_vw_batch_decode_verify is pinned by VMB3/VMB4
// (generated streams swept under the vector regime AND the forced-scalar
// route): same codes, same stop points, same counters. The trick that makes
// batch collection safe: geometry of record k+1 is only examined AFTER the
// tags of records <= k have all been accepted, exactly as the serial loop
// would encounter them — a full lane-batch is tag-checked before the next
// record's geometry is read, so error precedence is preserved.
//
// Layer discipline: driver layer; weft.c/weft.h untouched; no allocation.

#include "verified_mb.h"
#include "sha256_mb.h"

#include <string.h>

/// Staging bytes per lane: envelope(16) + payload cap + two blocks of slack
/// (0x80 + zeros + 8-byte length never exceed one block past the data; the
/// slack keeps the bound obvious rather than exact).
#define WEFT_VW_MB_LANE_BYTES (16 + WEFT_VW_MB_MAX_PAYLOAD + 2 * SHA256_BLOCK_LEN)

void weft_vw_mb_init(weft_vw_mb_t* v, const uint8_t auth_key[WEFT_VW_KEY_LEN]) {
    hmac_sha256_key_t k;
    hmac_sha256_init_key(&k, auth_key, WEFT_VW_KEY_LEN);
    // k->inner already holds H-state-after-ipad (init_key updates the ipad
    // block through it); the outer pad state is one compression away.
    memcpy(v->inner_after_ipad, k.inner.h, sizeof(v->inner_after_ipad));
    sha256_ctx_t outer;
    sha256_init(&outer);
    sha256_update(&outer, k.opad_block, SHA256_BLOCK_LEN);
    memcpy(v->outer_after_opad, outer.h, sizeof(v->outer_after_opad));
}

// ---------------------------------------------------------------------------
// One batch of lanes: stage, two multi-buffer calls, tag-compare in order.
// ---------------------------------------------------------------------------

/// Verify `n_real` records (valid geometry already checked by the collector)
/// through a `lanes`-wide packing (8 AVX2 / 4 NEON). Records past n_real are
/// padded with duplicates of record 0 — the transform requires full lanes,
/// and duplicate lanes' outputs are ignored by the ordered tag compare.
/// Returns 0 when all n_real verify; otherwise the 0-based index of the
/// FIRST failing record is written to *first_bad. Record j spans
/// [rec[j], rec[j] + hs[j] + plen[j] + 32).
static int mb_verify_batch(const weft_vw_mb_t* key,
                           const uint8_t* rec[WEFT_SHA256_MB_MAX_LANES],
                           const size_t hs[WEFT_SHA256_MB_MAX_LANES],
                           const size_t plen[WEFT_SHA256_MB_MAX_LANES],
                           int n_real, int lanes, size_t* first_bad) {
    uint8_t staging[WEFT_SHA256_MB_MAX_LANES][WEFT_VW_MB_LANE_BYTES];
    const uint8_t* msg[WEFT_SHA256_MB_MAX_LANES] = {0};
    size_t nblocks[WEFT_SHA256_MB_MAX_LANES] = {0};
    uint32_t st_in[WEFT_SHA256_MB_MAX_LANES][8] = {{0}};
    uint32_t st_out[WEFT_SHA256_MB_MAX_LANES][8] = {{0}};

    // Lane-local copies so the duplicate padding below can fill the tail
    // lanes (rec/hs/plen arrive const from the collector).
    const uint8_t* rec_l[WEFT_SHA256_MB_MAX_LANES];
    size_t hs_l[WEFT_SHA256_MB_MAX_LANES];
    size_t plen_l[WEFT_SHA256_MB_MAX_LANES];
    for (int j = 0; j < n_real; j++) {
        rec_l[j] = rec[j];
        hs_l[j] = hs[j];
        plen_l[j] = plen[j];
    }
    for (int j = n_real; j < lanes; j++) {
        rec_l[j] = rec[0];
        hs_l[j] = hs[0];
        plen_l[j] = plen[0];
    }

    // --- stage inner sequences: env[0..16) || payload || padding ----------
    for (int j = 0; j < lanes; j++) {
        const size_t d = WEFT_VW_ENVELOPE_LEN + plen_l[j];
        uint8_t* s = staging[j];
        memcpy(s, rec_l[j], WEFT_VW_ENVELOPE_LEN);
        memcpy(s + WEFT_VW_ENVELOPE_LEN, rec_l[j] + hs_l[j], plen_l[j]);
        const size_t n_blocks = (d + 9 + SHA256_BLOCK_LEN - 1) / SHA256_BLOCK_LEN;
        memset(s + d, 0, n_blocks * SHA256_BLOCK_LEN - d);
        s[d] = 0x80;
        const uint64_t bit_len = (uint64_t)(SHA256_BLOCK_LEN + d) * 8;  // ipad included
        for (int b = 0; b < 8; b++) {
            s[n_blocks * SHA256_BLOCK_LEN - 8 + b] = (uint8_t)(bit_len >> (56 - b * 8));
        }
        msg[j] = s;
        nblocks[j] = n_blocks;
        memcpy(st_in[j], key->inner_after_ipad, sizeof(st_in[j]));
    }
    if (weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg, nblocks, lanes) != 0) {
        return -1;  // lane count mismatch with the active backend — caller bug
    }

    // --- stage outer blocks: digest || 0x80 || zeros || BE(768) -----------
    for (int j = 0; j < lanes; j++) {
        uint8_t* s = staging[j];
        for (int i = 0; i < 8; i++) {
            s[i * 4]     = (uint8_t)(st_out[j][i] >> 24);
            s[i * 4 + 1] = (uint8_t)(st_out[j][i] >> 16);
            s[i * 4 + 2] = (uint8_t)(st_out[j][i] >> 8);
            s[i * 4 + 3] = (uint8_t)(st_out[j][i]);
        }
        memset(s + 32, 0, 32);
        s[32] = 0x80;
        const uint64_t bit_len = (uint64_t)(SHA256_BLOCK_LEN + SHA256_DIGEST_LEN) * 8;
        for (int b = 0; b < 8; b++) {
            s[56 + b] = (uint8_t)(bit_len >> (56 - b * 8));
        }
        msg[j] = s;
        nblocks[j] = 1;
        memcpy(st_in[j], key->outer_after_opad, sizeof(st_in[j]));
    }
    if (weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg, nblocks, lanes) != 0) {
        return -1;
    }

    // --- tag compare in record order (constant-time per tag) --------------
    for (int j = 0; j < n_real; j++) {
        uint8_t tag[WEFT_VW_TAG_LEN];
        for (int i = 0; i < 8; i++) {
            tag[i * 4]     = (uint8_t)(st_out[j][i] >> 24);
            tag[i * 4 + 1] = (uint8_t)(st_out[j][i] >> 16);
            tag[i * 4 + 2] = (uint8_t)(st_out[j][i] >> 8);
            tag[i * 4 + 3] = (uint8_t)(st_out[j][i]);
        }
        if (weft_vw_ct_eq(tag, rec[j] + hs[j] + plen[j], WEFT_VW_TAG_LEN) != 1) {
            *first_bad = (size_t)j;
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Batch stream verification — the public entry
// ---------------------------------------------------------------------------

weft_vw_result_t weft_vw_batch_decode_verify_mb(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                                const uint8_t* src, size_t src_len,
                                                weft_vw_record_view_t* views,
                                                size_t views_cap,
                                                size_t* n_verified,
                                                size_t* bytes_consumed) {
    // No vector backend: identical semantics at serial speed — route to the
    // Series-6 implementation rather than run 1-lane batches through the
    // staging machinery. (Tests still exercise the scalar lane loop directly
    // through weft_sha256_mb; this route is about the shipped hot path.)
    if (weft_sha256_mb_lanes() == 0) {
        return weft_vw_batch_decode_verify(auth_key, src, src_len, views, views_cap,
                                           n_verified, bytes_consumed);
    }

    weft_vw_mb_t key;
    weft_vw_mb_init(&key, auth_key);

    const int lanes = weft_sha256_mb_lanes();
    size_t verified = 0;
    size_t off = 0;

    const uint8_t* rec[WEFT_SHA256_MB_MAX_LANES];
    size_t hs[WEFT_SHA256_MB_MAX_LANES];
    size_t plen[WEFT_SHA256_MB_MAX_LANES];

    while (off + WEFT_VW_ENVELOPE_LEN + WEFT_VW_TAG_LEN <= src_len) {
        // Collect one lane-batch of geometrically valid records. A geometry
        // error at record k is PENDING, not immediate: the serial contract
        // verifies the good prefix first and only then reports the error —
        // so the collected batch is flushed (verified, views filled) before
        // the pending code is returned.
        size_t batch_off = off;
        int nbatch = 0;
        weft_vw_result_t pending = WEFT_VW_OK;
        while (nbatch < lanes && batch_off + WEFT_VW_ENVELOPE_LEN + WEFT_VW_TAG_LEN <= src_len) {
            const uint8_t* r = src + batch_off;
            if (!(r[0] == 'W' && r[1] == 'E' && r[2] == 'F' && r[3] == 'T')) {
                pending = WEFT_VW_ERR_BAD_MAGIC;
                break;
            }
            const uint16_t header_size = (uint16_t)(r[6] | ((uint16_t)r[7] << 8));
            const uint32_t p = (uint32_t)r[12] | ((uint32_t)r[13] << 8) |
                               ((uint32_t)r[14] << 16) | ((uint32_t)r[15] << 24);
            if (header_size < WEFT_VW_ENVELOPE_LEN) {
                pending = WEFT_VW_ERR_BAD_MAGIC;
                break;
            }
            const size_t body = (size_t)header_size + p;
            if (body > src_len - batch_off - WEFT_VW_TAG_LEN) {
                pending = WEFT_VW_ERR_SHORT;
                break;
            }
            rec[nbatch] = r;
            hs[nbatch] = header_size;
            plen[nbatch] = p;
            nbatch++;
            batch_off += body + WEFT_VW_TAG_LEN;
        }

        // Split the batch at the SIMD payload cap: in-cap records verify in
        // lanes; an oversized record hands the REST of the stream to the
        // serial verifier (identical semantics; the lane win is the small-
        // record hot case, stated in the header).
        int simd_n = 0;
        while (simd_n < nbatch && plen[simd_n] <= WEFT_VW_MB_MAX_PAYLOAD) {
            simd_n++;
        }

        if (simd_n > 0) {
            size_t first_bad = 0;
            const int bad = mb_verify_batch(&key, rec, hs, plen, simd_n, lanes, &first_bad);
            if (bad < 0) {
                // Backend lane mismatch cannot happen here (lanes came from
                // the same probe); treat as a hard error, Law 4: count only
                // what was proven.
                *n_verified = verified;
                *bytes_consumed = off;
                return WEFT_VW_ERR_SHORT;
            }
            if (bad == 1) {
                // Stop at the first failing tag — same point the serial loop
                // would reach it. bytes_consumed = end of the last VERIFIED
                // record: the failing record's start when first_bad > 0,
                // the batch start otherwise (records are contiguous).
                *n_verified = verified + first_bad;
                *bytes_consumed = (first_bad == 0)
                    ? off
                    : (size_t)(rec[first_bad] - src);
                return WEFT_VW_ERR_TAG;
            }
            // All simd_n records verified: fill views (bounded), advance.
            for (int j = 0; j < simd_n; j++) {
                if (views != NULL && verified + (size_t)j < views_cap) {
                    weft_vw_record_view_t* out = &views[verified + (size_t)j];
                    out->envelope = rec[j];
                    out->payload = rec[j] + hs[j];
                    out->payload_len = plen[j];
                    out->seq = (uint32_t)rec[j][8] | ((uint32_t)rec[j][9] << 8) |
                               ((uint32_t)rec[j][10] << 16) | ((uint32_t)rec[j][11] << 24);
                }
            }
            verified += (size_t)simd_n;
            off = (simd_n == nbatch) ? batch_off : (size_t)(rec[simd_n] - src);
        }

        if (simd_n < nbatch) {
            // An oversized record: serial-verify the remainder of the stream
            // from the first oversized record, with view/consumer continuity.
            const uint8_t* rest = rec[simd_n];
            const size_t rest_len = src_len - (size_t)(rest - src);
            weft_vw_record_view_t* vout =
                (views != NULL && verified < views_cap) ? views + verified : NULL;
            const size_t vcap = (views != NULL && verified < views_cap)
                                    ? views_cap - verified
                                    : 0;
            size_t n2 = 0, consumed2 = 0;
            const weft_vw_result_t r2 =
                weft_vw_batch_decode_verify(auth_key, rest, rest_len, vout, vcap,
                                            &n2, &consumed2);
            verified += n2;
            off = (size_t)(rest - src) + consumed2;
            *n_verified = verified;
            *bytes_consumed = off;
            return r2;
        }

        if (pending != WEFT_VW_OK) {
            // The collected batch flushed clean above; now report the
            // geometry error the collector hit (serial order preserved:
            // records before the error were verified and counted first).
            *n_verified = verified;
            *bytes_consumed = off;
            return pending;
        }

        if (nbatch == 0) {
            break;  // no full record left: caller decides truncation policy
        }
    }

    *n_verified = verified;
    *bytes_consumed = off;
    return WEFT_VW_OK;
}

// ---------------------------------------------------------------------------
// SIMD magic scan — crash-tolerant resync primitive
// ---------------------------------------------------------------------------

static int scan_magic_scalar(const uint8_t* src, size_t len, size_t from, size_t* out_off) {
    for (size_t i = from; i + 4 <= len; i++) {
        if (src[i] == 'W' && src[i + 1] == 'E' && src[i + 2] == 'F' && src[i + 3] == 'T') {
            *out_off = i;
            return 1;
        }
    }
    return 0;
}

#if defined(__x86_64__) || defined(_M_X64)

#if defined(__GNUC__)
#include <immintrin.h>
#endif

// Byte-mask AND chain: candidate byte b in a 32-byte window iff
// src[b]=='W' & src[b+1]=='E' & src[b+2]=='F' & src[b+3]=='T'. Bit b of the
// AND-ed mask covers a needle fully inside the window (b <= 28); windows
// advance 29 so every start offset is covered exactly once across windows.
__attribute__((target("avx2")))
static int scan_magic_avx2(const uint8_t* src, size_t len, size_t from, size_t* out_off) {
    const __m256i wv = _mm256_set1_epi8((char)'W');
    const __m256i ev = _mm256_set1_epi8((char)'E');
    const __m256i fv = _mm256_set1_epi8((char)'F');
    const __m256i tv = _mm256_set1_epi8((char)'T');
    size_t i = from;
    for (; i + 32 <= len; i += 29) {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(src + i));
        const uint32_t wm = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, wv));
        const uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, ev));
        const uint32_t fm = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, fv));
        const uint32_t tm = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, tv));
        // >>1/2/3 in the movemask domain aligns byte b's neighbor tests to bit b.
        uint32_t cand = wm & (em >> 1) & (fm >> 2) & (tm >> 3);
        cand &= 0x1FFFFFFFu;  // needles fully inside the window only
        if (cand != 0) {
            *out_off = i + (size_t)__builtin_ctz(cand);
            return 1;
        }
    }
    return scan_magic_scalar(src, len, i, out_off);
}

#endif  // __x86_64__

#if defined(__aarch64__)

#include <arm_neon.h>

// Byte-mask AND chain: candidate byte b in a 16-byte window iff
// src[b]=='W' & src[b+1]=='E' & src[b+2]=='F' & src[b+3]=='T'. vextq_u8
// aligns neighbor comparisons to byte lane b; lanes 0..12 cover needles
// fully inside the 16-byte window. Windows advance 13 so every start
// offset is covered exactly once across windows.
static int scan_magic_neon(const uint8_t* src, size_t len, size_t from, size_t* out_off) {
    const uint8x16_t wv = vdupq_n_u8((uint8_t)'W');
    const uint8x16_t ev = vdupq_n_u8((uint8_t)'E');
    const uint8x16_t fv = vdupq_n_u8((uint8_t)'F');
    const uint8x16_t tv = vdupq_n_u8((uint8_t)'T');
    const uint8x16_t zv = vdupq_n_u8(0);
    size_t i = from;
    for (; i + 16 <= len; i += 13) {
        const uint8x16_t v = vld1q_u8(src + i);
        const uint8x16_t wm = vceqq_u8(v, wv);
        const uint8x16_t em = vceqq_u8(v, ev);
        const uint8x16_t fm = vceqq_u8(v, fv);
        const uint8x16_t tm = vceqq_u8(v, tv);
        const uint8x16_t e1 = vextq_u8(em, zv, 1);
        const uint8x16_t f2 = vextq_u8(fm, zv, 2);
        const uint8x16_t t3 = vextq_u8(tm, zv, 3);
        const uint8x16_t cand = vandq_u8(vandq_u8(wm, e1), vandq_u8(f2, t3));
        uint8_t match[16];
        vst1q_u8(match, cand);
        for (int b = 0; b < 13; b++) {
            if (match[b]) {
                *out_off = i + (size_t)b;
                return 1;
            }
        }
    }
    return scan_magic_scalar(src, len, i, out_off);
}

#endif  // __aarch64__

int weft_vw_scan_magic(const uint8_t* src, size_t len, size_t from, size_t* out_off) {
    if (from > len) return 0;
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        return scan_magic_avx2(src, len, from, out_off);
    }
#endif
    return scan_magic_scalar(src, len, from, out_off);
#elif defined(__aarch64__)
    return scan_magic_neon(src, len, from, out_off);
#else
    return scan_magic_scalar(src, len, from, out_off);
#endif
}
