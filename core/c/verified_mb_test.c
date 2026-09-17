// verified_mb_test.c — VMB-series conformance for the multi-buffer batch path.
//
// Gates (any failure exits non-zero):
//   VMB1 transform bit-identity: weft_sha256_mb (AVX2 8-way when present)
//       == per-lane scalar reference across randomized lengths, staggered
//       lane finishes (snapshot semantics), zero-block lanes, and lane
//       counts 1..8 under the forced-scalar lane loop
//   VMB2 HMAC equivalence through the batch API: records signed by the
//       serial path (whose digests are pinned to RFC 4231 by V1) verify
//       through the multi-buffer path; tampered tags stop at the first bad
//       record with exact prefix counts
//   VMB3 batch-API equivalence sweep: generated streams (mixed payload
//       lengths, header_size gaps, boundary padding cases) — code,
//       n_verified, bytes_consumed, and every view field identical between
//       weft_vw_batch_decode_verify and weft_vw_batch_decode_verify_mb
//   VMB4 corruption matrix: every error class (magic bytes, header geometry,
//       body overflow, payload/tag/envelope tamper) — identical codes and
//       counters between the serial and multi-buffer paths
//   VMB5 duplicate-lane determinism: one message in all 8 lanes -> identical
//       snapshots; staggered nblocks never corrupt a finished lane
//   VMB6 scan_magic exhaustive: needle planted at EVERY offset of a random
//       buffer (including the 29-byte window seams) — vector scan finds
//       exactly the scalar scan's match set; near-misses and bounds
//   VMB7 crash-tolerant resync idiom: corrupted middle record, scan-based
//       resync, resumed verification, telescoping accounting stated
//   VMB8 SIMD payload-cap boundary: records at/around the 4096-byte cap
//       (4095/4096 in-lane, 4097/5000 serial-fallback) — equivalence holds
//
// Build: make -C core/c verified-mb-test   — then ./core/c/verified-mb-test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"
#include "sha256_mb.h"
#include "verified.h"
#include "verified_mb.h"
#include "weft.h"

static int g_fail = 0;

static void check(int cond, const char* name) {
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

// Deterministic PRNG (house style: xorshift32, time-injection-free evidence).
static uint32_t xs32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

// --- record construction ------------------------------------------------------

/// Build one auth record with an arbitrary header_size (>= 16): the signed
/// data is envelope[0..16) || payload regardless; bytes [16, hs) are filler
/// the signature does not cover — the geometry gap both paths must honor.
static size_t make_record(uint8_t* dst, weft_vw_signer_t* s, uint32_t seq,
                          size_t hs, const uint8_t* payload, size_t plen) {
    uint8_t env[16];
    weft_envelope_encode_v1(env, seq, (uint32_t)plen);
    env[6] = (uint8_t)(hs & 0xFF);
    env[7] = (uint8_t)((hs >> 8) & 0xFF);
    uint8_t tag[WEFT_VW_TAG_LEN];
    weft_vw_sign(s, env, payload, plen, tag);
    memcpy(dst, env, 16);
    for (size_t i = 16; i < hs; i++) dst[i] = (uint8_t)(0xA0 + i);
    memcpy(dst + hs, payload, plen);
    memcpy(dst + hs + plen, tag, WEFT_VW_TAG_LEN);
    return hs + plen + WEFT_VW_TAG_LEN;
}

// --- VMB1: transform bit-identity ---------------------------------------------

static void test_vmb1_transform(void) {
    printf("VMB1: multi-buffer transform bit-identity\n");
    const int impl_lanes = weft_sha256_mb_lanes();
    printf("  backend: %s (lanes=%d)\n",
           weft_sha256_mb_active_impl() == WEFT_SHA256_MB_X86_AVX2 ? "x86-avx2"
           : weft_sha256_mb_active_impl() == WEFT_SHA256_MB_ARM_NEON ? "arm-neon"
           : weft_sha256_mb_active_impl() == WEFT_SHA256_MB_X86_AVX512 ? "x86-avx512"
           : "scalar",
           impl_lanes);

    uint32_t seed = 0xC0FFEE1u;
    int mismatches = 0;

    // Staggered/zero nblocks under the vector backend (when present).
    if (impl_lanes > 0) {
        for (int trial = 0; trial < 64; trial++) {
            uint8_t data[WEFT_SHA256_MB_MAX_LANES][10 * SHA256_BLOCK_LEN];
            size_t nb[WEFT_SHA256_MB_MAX_LANES] = {0};
            for (int j = 0; j < impl_lanes; j++) {
                for (size_t b = 0; b < sizeof(data[j]); b++) {
                    data[j][b] = (uint8_t)xs32(&seed);
                }
                nb[j] = (trial == 0) ? 3 : (xs32(&seed) % 11);  // 0..10 blocks
            }
            uint32_t st_in[WEFT_SHA256_MB_MAX_LANES][8];
            uint32_t st_out[WEFT_SHA256_MB_MAX_LANES][8];
            for (int j = 0; j < impl_lanes; j++) {
                for (int i = 0; i < 8; i++) st_in[j][i] = xs32(&seed);
            }
            const uint8_t* msg[WEFT_SHA256_MB_MAX_LANES];
            for (int j = 0; j < impl_lanes; j++) msg[j] = data[j];

            if (weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg, nb,
                               impl_lanes) != 0) {
                mismatches++;
                continue;
            }
            // Reference: per-lane scalar via the public streaming API.
            for (int j = 0; j < impl_lanes; j++) {
                sha256_ctx_t ctx;
                sha256_init(&ctx);
                memcpy(ctx.h, st_in[j], sizeof(ctx.h));
                sha256_update(&ctx, data[j], nb[j] * SHA256_BLOCK_LEN);
                if (nb[j] == 0 || memcmp(ctx.h, st_out[j], 32) != 0) {
                    if (nb[j] != 0) mismatches++;
                }
            }
        }
        check(mismatches == 0, "64 randomized staggered-lane trials bit-identical");
    } else {
        printf("  (no vector backend on this CPU — VMB1 vector leg skipped, "
               "scalar lane loop covered below)\n");
    }

    // Scalar lane loop: lane counts 1..16, fully scalar (Series-6 pin too).
    weft_sha256_force_scalar();
    weft_sha256_mb_force_scalar();
    int sm = 0;
    for (int lanes = 1; lanes <= WEFT_SHA256_MB_MAX_LANES; lanes++) {
        for (int trial = 0; trial < 16; trial++) {
            uint8_t data[WEFT_SHA256_MB_MAX_LANES][8 * SHA256_BLOCK_LEN];
            size_t nb[WEFT_SHA256_MB_MAX_LANES] = {0};
            for (int j = 0; j < lanes; j++) {
                for (size_t b = 0; b < sizeof(data[j]); b++) {
                    data[j][b] = (uint8_t)xs32(&seed);
                }
                nb[j] = xs32(&seed) % 9;  // 0..8 blocks
            }
            uint32_t st_in[WEFT_SHA256_MB_MAX_LANES][8], st_out[WEFT_SHA256_MB_MAX_LANES][8];
            for (int j = 0; j < lanes; j++) {
                for (int i = 0; i < 8; i++) st_in[j][i] = xs32(&seed);
            }
            const uint8_t* msg[WEFT_SHA256_MB_MAX_LANES];
            for (int j = 0; j < lanes; j++) msg[j] = data[j];
            if (weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg, nb, lanes) != 0) {
                sm++;
                continue;
            }
            for (int j = 0; j < lanes; j++) {
                sha256_ctx_t ctx;
                sha256_init(&ctx);
                memcpy(ctx.h, st_in[j], sizeof(ctx.h));
                sha256_update(&ctx, data[j], nb[j] * SHA256_BLOCK_LEN);
                if (nb[j] == 0) {
                    if (memcmp(st_in[j], st_out[j], 32) != 0) sm++;
                } else if (memcmp(ctx.h, st_out[j], 32) != 0) {
                    sm++;
                }
            }
        }
    }
    check(sm == 0, "scalar lane loop 1..16 lanes bit-identical (256 trials)");
    weft_sha256_mb_force_auto();
    weft_sha256_force_auto();
}

// --- VMB2: HMAC equivalence through the batch API ------------------------------

static void test_vmb2_hmac_equivalence(void) {
    printf("VMB2: multi-buffer HMAC == serial HMAC (chain to RFC 4231 via V1)\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"vmb2-key", 8, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    const size_t N = 64, plen = 96;
    const size_t rec_len = 16 + plen + 32;
    uint8_t* stream = malloc(N * rec_len);
    uint8_t payload[plen];
    for (size_t i = 0; i < N; i++) {
        for (size_t j = 0; j < plen; j++) payload[j] = (uint8_t)(i * 7 + j);
        make_record(stream + i * rec_len, &s, (uint32_t)(i + 1), 16, payload, plen);
    }

    weft_vw_record_view_t views[N];
    size_t n = 0, consumed = 0;
    weft_vw_result_t r = weft_vw_batch_decode_verify_mb(key, stream, N * rec_len,
                                                        views, N, &n, &consumed);
    check(r == WEFT_VW_OK && n == N, "64 serial-signed records verify in lanes");
    check(consumed == N * rec_len, "bytes_consumed exact");

    // Tamper record 20 and record 40: must stop at 20 with prefix 19.
    stream[20 * rec_len + 40] ^= 0x21;
    stream[40 * rec_len + 130] ^= 0x11;
    n = 0;
    consumed = 0;
    r = weft_vw_batch_decode_verify_mb(key, stream, N * rec_len, views, N, &n, &consumed);
    check(r == WEFT_VW_ERR_TAG && n == 20, "first tampered record stops the batch (20)");
    check(consumed == 20 * rec_len, "prefix end exact");
    free(stream);
}

// --- VMB3: batch-API equivalence sweep -----------------------------------------

static void run_batch_pair(const uint8_t* key, const uint8_t* stream, size_t len,
                           weft_vw_record_view_t* vserial, weft_vw_record_view_t* vmb,
                           size_t cap, int* agree) {
    size_t n1 = 0, c1 = 0, n2 = 0, c2 = 0;
    const weft_vw_result_t r1 =
        weft_vw_batch_decode_verify(key, stream, len, vserial, cap, &n1, &c1);
    const weft_vw_result_t r2 =
        weft_vw_batch_decode_verify_mb(key, stream, len, vmb, cap, &n2, &c2);
    int ok = (r1 == r2) && (n1 == n2) && (c1 == c2);
    if (ok) {
        for (size_t i = 0; i < n1 && i < cap; i++) {
            if (vserial[i].envelope != vmb[i].envelope ||
                vserial[i].payload != vmb[i].payload ||
                vserial[i].payload_len != vmb[i].payload_len ||
                vserial[i].seq != vmb[i].seq) {
                ok = 0;
                break;
            }
        }
    }
    if (!ok) {
        (*agree)++;
        printf("    disagree: r=%d/%d n=%zu/%zu c=%zu/%zu\n", r1, r2, n1, n2, c1, c2);
    }
}

static void test_vmb3_equivalence(void) {
    printf("VMB3: batch-API equivalence sweep (serial vs multi-buffer)\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"vmb3-key", 8, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    const size_t PLENS[] = {0, 1, 8, 39, 40, 47, 48, 55, 56, 63, 64, 103, 128, 200, 301};
    const size_t NP = sizeof(PLENS) / sizeof(PLENS[0]);
    const size_t HS[] = {16, 16, 24, 32, 16};
    uint32_t seed = 0xBEEF5EEDu;

    uint8_t* stream = malloc(40 * (32 + 500 + 32));
    weft_vw_record_view_t* vs = malloc(40 * sizeof(weft_vw_record_view_t));
    weft_vw_record_view_t* vm = malloc(40 * sizeof(weft_vw_record_view_t));
    int agree = 0;
    int trials = 0;

    for (int t = 0; t < 120; t++) {
        const size_t nrec = t % 41;  // 0..40 records
        size_t off = 0;
        uint8_t payload[500];
        for (size_t i = 0; i < nrec; i++) {
            const size_t plen = PLENS[xs32(&seed) % NP];
            const size_t hs = HS[xs32(&seed) % 5];
            for (size_t j = 0; j < plen; j++) payload[j] = (uint8_t)xs32(&seed);
            off += make_record(stream + off, &s, (uint32_t)(i + 1), hs, payload, plen);
        }
        // Truncated tail variants: exercise the partial-record boundary.
        size_t len = off;
        if (t % 3 == 1 && len > 5) len -= 5;
        run_batch_pair(key, stream, len, vs, vm, 40, &agree);
        trials++;
        if (nrec > 0 && t % 4 == 0) {
            // Corrupt a random record's payload byte: both must agree on the stop.
            const size_t k = xs32(&seed) % nrec;
            size_t roff = 0;
            for (size_t i = 0; i < k; i++) {
                // re-walk geometry to find record k's offset
                const uint16_t hs2 = (uint16_t)(stream[roff + 6] | (stream[roff + 7] << 8));
                const uint32_t p2 = (uint32_t)stream[roff + 12] | ((uint32_t)stream[roff + 13] << 8) |
                                   ((uint32_t)stream[roff + 14] << 16) | ((uint32_t)stream[roff + 15] << 24);
                roff += hs2 + p2 + 32;
            }
            stream[roff + 20] ^= (uint8_t)(xs32(&seed) | 1);
            run_batch_pair(key, stream, off, vs, vm, 40, &agree);
            trials++;
            stream[roff + 20] ^= 0xFF;  // restore-ish; next trial re-signs anyway
        }
    }
    check(agree == 0 && trials == 149, "149 generated streams: code/counters/views identical");
    free(stream);
    free(vs);
    free(vm);
}

// --- VMB4: corruption matrix ----------------------------------------------------

static void test_vmb4_corruption(void) {
    printf("VMB4: corruption matrix — identical codes and counters\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"vmb4-key", 8, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    const size_t N = 12, plen = 64, rec_len = 16 + plen + 32;
    uint8_t* stream = malloc(N * rec_len);
    uint8_t payload[plen];
    for (size_t i = 0; i < N; i++) {
        for (size_t j = 0; j < plen; j++) payload[j] = (uint8_t)(i + j);
        make_record(stream + i * rec_len, &s, (uint32_t)(i + 1), 16, payload, plen);
    }
    const size_t k = 5;  // corrupt record 5

    struct {
        const char* name;
        size_t off;      // byte offset within record k
        uint8_t value;   // replacement (0 = leave, use xor below)
        uint8_t xor_;
        weft_vw_result_t expect;
    } cases[] = {
        {"magic byte 0 -> X", 0, 'X', 0, WEFT_VW_ERR_BAD_MAGIC},
        {"magic byte 3 -> X", 3, 'X', 0, WEFT_VW_ERR_BAD_MAGIC},
        {"header_size -> 15", 6, 15, 0, WEFT_VW_ERR_BAD_MAGIC},
        {"payload_len overflow", 15, 0xFF, 0, WEFT_VW_ERR_SHORT},
        {"payload tamper", 40, 0, 0x40, WEFT_VW_ERR_TAG},
        {"tag tamper", 16 + plen + 4, 0, 0x02, WEFT_VW_ERR_TAG},
        {"envelope seq tamper", 9, 0, 0x08, WEFT_VW_ERR_TAG},
    };

    int agree = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t* rec = stream + k * rec_len;
        const uint8_t save = rec[cases[c].off];
        if (cases[c].value) rec[cases[c].off] = cases[c].value;
        else rec[cases[c].off] ^= cases[c].xor_;

        size_t n1 = 0, c1 = 0, n2 = 0, c2 = 0;
        const weft_vw_result_t r1 = weft_vw_batch_decode_verify(
            key, stream, N * rec_len, NULL, 0, &n1, &c1);
        const weft_vw_result_t r2 = weft_vw_batch_decode_verify_mb(
            key, stream, N * rec_len, NULL, 0, &n2, &c2);
        const int ok = (r1 == r2) && (r1 == cases[c].expect) &&
                       (n1 == n2) && (n1 == k) && (c1 == c2) && (c1 == k * rec_len);
        check(ok, cases[c].name);
        if (!ok) agree++;
        rec[cases[c].off] = save;
    }
    check(agree == 0, "all corruption classes agree (code, prefix, consumed)");
    free(stream);
}

// --- VMB5: duplicate-lane determinism + snapshot isolation ----------------------

static void test_vmb5_snapshot(void) {
    printf("VMB5: duplicate lanes + snapshot isolation\n");
    const int lanes = weft_sha256_mb_lanes();
    if (lanes == 0) {
        printf("  (no vector backend — VMB5 vector leg skipped)\n");
        return;
    }
    uint8_t data[7 * SHA256_BLOCK_LEN];
    uint32_t seed = 0x5EED5EEDu;
    for (size_t b = 0; b < sizeof(data); b++) data[b] = (uint8_t)xs32(&seed);

    uint32_t st_in[WEFT_SHA256_MB_MAX_LANES][8];
    uint32_t st_out[WEFT_SHA256_MB_MAX_LANES][8];
    for (int j = 0; j < lanes; j++) {
        for (int i = 0; i < 8; i++) st_in[j][i] = 0x1000 + (uint32_t)i;
    }
    const uint8_t* msg[WEFT_SHA256_MB_MAX_LANES];
    size_t nb[WEFT_SHA256_MB_MAX_LANES];
    for (int j = 0; j < lanes; j++) {
        msg[j] = data;
        nb[j] = 7;
    }
    check(weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg, nb, lanes) == 0,
          "duplicate-lane call succeeds");
    int same = 1;
    for (int j = 1; j < lanes; j++) {
        if (memcmp(st_out[0], st_out[j], 32) != 0) same = 0;
    }
    check(same, "identical message in every lane -> identical snapshots");

    // Staggered: lane finishes early; later groups must not disturb it.
    // (16 entries — one per AVX-512 lane; shorter backends read the prefix.)
    size_t nb2[WEFT_SHA256_MB_MAX_LANES] = {0};
    const uint8_t* msg2[WEFT_SHA256_MB_MAX_LANES];
    const size_t stagger[WEFT_SHA256_MB_MAX_LANES] =
        {5, 1, 3, 0, 7, 2, 1, 4, 6, 0, 2, 5, 3, 1, 7, 0};
    for (int j = 0; j < lanes; j++) {
        msg2[j] = data;
        nb2[j] = (lanes == 8) ? stagger[j]
                  : (lanes == 16) ? (stagger[j] == 0 ? 0 : stagger[j] % 5 + 1)
                  : stagger[j] % 5 + 1;
    }
    check(weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg2, nb2, lanes) == 0,
          "staggered-lane call succeeds");
    int iso = 1;
    for (int j = 0; j < lanes; j++) {
        if (nb2[j] == 0) continue;
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        memcpy(ctx.h, st_in[j], sizeof(ctx.h));
        sha256_update(&ctx, data, nb2[j] * SHA256_BLOCK_LEN);
        if (memcmp(ctx.h, st_out[j], 32) != 0) iso = 0;
    }
    check(iso, "staggered finishes: each lane's snapshot exact, undisturbed");
}

// --- VMB6: scan_magic exhaustive -----------------------------------------------

static int scan_scalar_ref(const uint8_t* src, size_t len, size_t from, size_t* out) {
    for (size_t i = from; i + 4 <= len; i++) {
        if (src[i] == 'W' && src[i + 1] == 'E' && src[i + 2] == 'F' && src[i + 3] == 'T') {
            *out = i;
            return 1;
        }
    }
    return 0;
}

static void test_vmb6_scan(void) {
    printf("VMB6: scan_magic exhaustive (window seams included)\n");
    uint8_t buf[512];
    uint32_t seed = 0x5CA61111u;
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(xs32(&seed) | 0x80);

    // 1) Needle at EVERY offset: first-find must match scalar exactly.
    int bad = 0;
    for (size_t plant = 0; plant + 4 <= sizeof(buf); plant++) {
        uint8_t save[4];
        memcpy(save, buf + plant, 4);
        memcpy(buf + plant, "WEFT", 4);
        size_t a = 0, b = 0;
        // No earlier accidental needle: filler's high bit is set (>= 0x80).
        const int ra = weft_vw_scan_magic(buf, sizeof(buf), 0, &a);
        const int rb = scan_scalar_ref(buf, sizeof(buf), 0, &b);
        if (ra != rb || (ra && a != b)) bad++;
        memcpy(buf + plant, save, 4);
    }
    check(bad == 0, "needle at every offset 0..508: vector == scalar first-find");

    // 2) Enumerate all matches by repeated scanning; match sets identical.
    uint8_t multi[256];
    for (size_t i = 0; i < sizeof(multi); i++) multi[i] = (uint8_t)(0x80 | (i % 41));
    memcpy(multi + 10, "WEFT", 4);
    memcpy(multi + 39, "WEFT", 4);   // seam: window advance 29 -> 10+29
    memcpy(multi + 68, "WEFT", 4);
    memcpy(multi + 97, "WEFT", 4);
    memcpy(multi + 252 - 96, "WEFT", 4);  // 156
    size_t va[8], sa[8];
    size_t nv = 0, ns = 0;
    size_t from = 0;
    for (;;) {
        size_t o = 0;
        if (!weft_vw_scan_magic(multi, sizeof(multi), from, &o)) break;
        va[nv++] = o;
        from = o + 1;
    }
    from = 0;
    for (;;) {
        size_t o = 0;
        if (!scan_scalar_ref(multi, sizeof(multi), from, &o)) break;
        sa[ns++] = o;
        from = o + 1;
    }
    int ok = nv == ns && nv == 5;
    for (size_t i = 0; i < nv && i < 8; i++) {
        if (va[i] != sa[i]) ok = 0;
    }
    check(ok, "multi-match enumeration identical (5 needles incl. seams)");

    // 3) Near-misses and bounds.
    uint8_t nm[64];
    memset(nm, 0x81, sizeof(nm));
    memcpy(nm + 20, "WEFX", 4);
    memcpy(nm + 40, "XWEFT", 5);
    memcpy(nm + 60 - 4, "WEF", 3);  // needle would exceed the buffer
    size_t o = 0;
    check(weft_vw_scan_magic(nm, sizeof(nm), 0, &o) == 1 && o == 41,
          "near-misses skipped; real needle at 41 found");
    check(weft_vw_scan_magic(nm, sizeof(nm), 42, &o) == 0, "from past last match -> 0");
    check(weft_vw_scan_magic(nm, sizeof(nm), sizeof(nm) + 1, &o) == 0,
          "from > len -> 0");
    check(weft_vw_scan_magic(nm, 0, 0, &o) == 0, "empty buffer -> 0");
}

// --- VMB7: crash-tolerant resync idiom ------------------------------------------

static void test_vmb7_resync(void) {
    printf("VMB7: crash-tolerant resync after corruption\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"vmb7-key", 8, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    const size_t N = 40, plen = 64, rec_len = 16 + plen + 32;
    uint8_t* stream = malloc(N * rec_len);
    uint8_t payload[plen];
    for (size_t i = 0; i < N; i++) {
        for (size_t j = 0; j < plen; j++) payload[j] = (uint8_t)(i * 3 + j);
        make_record(stream + i * rec_len, &s, (uint32_t)(i + 1), 16, payload, plen);
    }

    // Corrupt record 5's whole span with W-free garbage (0xAA pattern has no
    // 'W', so the scan cannot land inside the corrupted record).
    memset(stream + 5 * rec_len, 0xAA, rec_len);

    size_t verified = 0, off = 0;
    size_t resyncs = 0;
    for (;;) {
        size_t n = 0, consumed = 0;
        const weft_vw_result_t r = weft_vw_batch_decode_verify_mb(
            key, stream + off, N * rec_len - off, NULL, 0, &n, &consumed);
        verified += n;
        off += consumed;
        if (r == WEFT_VW_OK) break;
        // Resync: find the next magic strictly past the failure point.
        size_t next = 0;
        if (!weft_vw_scan_magic(stream, N * rec_len, off + 1, &next)) break;
        off = next;
        resyncs++;
    }
    check(verified == 39, "39 of 40 records verified across one corruption (drop counted)");
    check(resyncs == 1, "exactly one resync");
    check(off == N * rec_len, "stream walked to the end");

    // Telescoping accounting: seqs 1..5 and 7..40; dropped = seq 6 only.
    size_t n2 = 0, consumed2 = 0;
    weft_vw_record_view_t v[8];
    const weft_vw_result_t r1 = weft_vw_batch_decode_verify_mb(
        key, stream, 5 * rec_len, v, 8, &n2, &consumed2);
    check(r1 == WEFT_VW_OK && n2 == 5, "prefix batch: 5 good records");
    size_t n3 = 0, consumed3 = 0;
    size_t next = 0;
    check(weft_vw_scan_magic(stream, N * rec_len, 5 * rec_len, &next) == 1 &&
              next == 6 * rec_len, "scan lands exactly on record 6's magic");
    const weft_vw_result_t r2 = weft_vw_batch_decode_verify_mb(
        key, stream + next, N * rec_len - next, v, 8, &n3, &consumed3);
    check(r2 == WEFT_VW_OK && n3 == 34, "post-resync batch: remaining 34 records");
    check(v[0].seq == 7, "resumed stream starts at seq 7 (dropped seq 6)");
    free(stream);
}

// --- VMB8: SIMD payload-cap boundary ---------------------------------------------

static void test_vmb8_cap(void) {
    printf("VMB8: SIMD payload-cap boundary (4095/4096/4097/5000)\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"vmb8-key", 8, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    const size_t PLENS[] = {4095, 4096, 4097, 5000, 48, 4096, 4095};
    const size_t N = sizeof(PLENS) / sizeof(PLENS[0]);
    uint8_t* stream = malloc(N * (16 + 5000 + 32));
    uint8_t* payload = malloc(5000);
    size_t off = 0;
    size_t ends[7];
    for (size_t i = 0; i < N; i++) {
        for (size_t j = 0; j < PLENS[i]; j++) payload[j] = (uint8_t)(i * 5 + j);
        const size_t rec_len = 16 + PLENS[i] + 32;
        make_record(stream + off, &s, (uint32_t)(i + 1), 16, payload, PLENS[i]);
        off += rec_len;
        ends[i] = off;
    }

    size_t n1 = 0, c1 = 0, n2 = 0, c2 = 0;
    const weft_vw_result_t r1 = weft_vw_batch_decode_verify(
        key, stream, off, NULL, 0, &n1, &c1);
    const weft_vw_result_t r2 = weft_vw_batch_decode_verify_mb(
        key, stream, off, NULL, 0, &n2, &c2);
    check(r1 == r2 && r1 == WEFT_VW_OK && n1 == n2 && n1 == N,
          "mixed cap-boundary stream: both paths verify all 7");
    check(c1 == c2 && c1 == off, "bytes_consumed identical (full stream)");

    // Tamper the 4096-byte record (index 5): both must stop at end of record 4.
    const size_t rec5 = ends[4];
    stream[rec5 + 100] ^= 0x55;
    n1 = 0;
    c1 = 0;
    n2 = 0;
    c2 = 0;
    const weft_vw_result_t t1 = weft_vw_batch_decode_verify(
        key, stream, off, NULL, 0, &n1, &c1);
    const weft_vw_result_t t2 = weft_vw_batch_decode_verify_mb(
        key, stream, off, NULL, 0, &n2, &c2);
    check(t1 == t2 && t1 == WEFT_VW_ERR_TAG && n1 == n2 && n1 == 5,
          "cap-boundary tamper: identical stop (prefix 5)");
    check(c1 == c2 && c1 == rec5, "prefix end exact at the oversized seam");
    free(stream);
    free(payload);
}

// --- VMB9: cross-impl bit-identity (Series 8 — RFC-0012) -------------------------

// Every multi-buffer backend compiled in AND offered by this CPU must produce
// BIT-IDENTICAL lane snapshots for the same inputs — the multi-kernel form of
// V8's HW-dispatch equivalence. The scalar lane loop (16 lanes) is the
// reference; AVX2 (8 lanes) and AVX-512 (16 lanes) are compared lane-by-lane
// against it, staggered block counts included so snapshot semantics are
// exercised in every regime.
static void test_vmb9_cross_impl(void) {
    printf("VMB9: cross-impl bit-identity (every available kernel vs scalar)\n");

    uint8_t data[WEFT_SHA256_MB_MAX_LANES][10 * SHA256_BLOCK_LEN];
    size_t nb[WEFT_SHA256_MB_MAX_LANES] = {0};
    uint32_t st_in[WEFT_SHA256_MB_MAX_LANES][8];
    const uint8_t* msg[WEFT_SHA256_MB_MAX_LANES];

    uint32_t seed = 0x58BADF00u;  // Series-8 seed
    for (int j = 0; j < WEFT_SHA256_MB_MAX_LANES; j++) {
        for (size_t b = 0; b < sizeof(data[j]); b++) {
            data[j][b] = (uint8_t)xs32(&seed);
        }
        nb[j] = xs32(&seed) % 11;  // 0..10 blocks
        for (int i = 0; i < 8; i++) st_in[j][i] = xs32(&seed);
        msg[j] = data[j];
    }

    // Reference: fully scalar lane loop over all 16 lanes.
    weft_sha256_force_scalar();
    weft_sha256_mb_force_scalar();
    uint32_t ref[WEFT_SHA256_MB_MAX_LANES][8];
    if (weft_sha256_mb(ref, (const uint32_t(*)[8])st_in, msg, nb,
                       WEFT_SHA256_MB_MAX_LANES) != 0) {
        check(0, "scalar reference call failed");
        weft_sha256_mb_force_auto();
        weft_sha256_force_auto();
        return;
    }

    // AVX-512 leg: 16 lanes, staggered counts, vs the reference.
    if (weft_sha256_mb_available(WEFT_SHA256_MB_X86_AVX512)) {
        weft_sha256_mb_force_impl(WEFT_SHA256_MB_X86_AVX512);
        uint32_t out[WEFT_SHA256_MB_MAX_LANES][8];
        const int rc = weft_sha256_mb(out, (const uint32_t(*)[8])st_in, msg, nb,
                                      WEFT_SHA256_MB_MAX_LANES);
        int same = (rc == 0);
        for (int j = 0; same && j < WEFT_SHA256_MB_MAX_LANES; j++) {
            if (nb[j] == 0) {
                same = memcmp(st_in[j], out[j], 32) == 0;
            } else {
                same = memcmp(ref[j], out[j], 32) == 0;
            }
        }
        check(same, "avx512 16-lane snapshots bit-identical to scalar (staggered)");
    } else {
        printf("  (avx512 not offered by this CPU — leg skipped, declared)\n");
    }

    // AVX2 leg: first 8 lanes vs the reference's first 8.
    if (weft_sha256_mb_available(WEFT_SHA256_MB_X86_AVX2)) {
        weft_sha256_mb_force_impl(WEFT_SHA256_MB_X86_AVX2);
        uint32_t out[8][8];
        size_t nb8[8];
        const uint8_t* msg8[8];
        for (int j = 0; j < 8; j++) {
            nb8[j] = nb[j];
            msg8[j] = msg[j];
        }
        const int rc = weft_sha256_mb(out, (const uint32_t(*)[8])st_in, msg8, nb8, 8);
        int same = (rc == 0);
        for (int j = 0; same && j < 8; j++) {
            if (nb[j] == 0) {
                same = memcmp(st_in[j], out[j], 32) == 0;
            } else {
                same = memcmp(ref[j], out[j], 32) == 0;
            }
        }
        check(same, "avx2 8-lane snapshots bit-identical to scalar (staggered)");
    } else {
        printf("  (avx2 not offered by this CPU — leg skipped, declared)\n");
    }

    weft_sha256_mb_force_auto();
    weft_sha256_force_auto();
}

int main(void) {
    printf("VerifiedWeft VMB-series (multi-buffer SIMD batch) — C driver layer\n");
    test_vmb1_transform();
    test_vmb2_hmac_equivalence();
    test_vmb3_equivalence();
    test_vmb4_corruption();
    test_vmb5_snapshot();
    test_vmb6_scan();
    test_vmb7_resync();
    test_vmb8_cap();
    test_vmb9_cross_impl();
    printf("\nverdict: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
