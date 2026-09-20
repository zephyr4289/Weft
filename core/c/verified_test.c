// verified_test.c — V-series conformance for RFC 0005 VerifiedWeft.
//
// Gates (any failure exits non-zero):
//   V1  HMAC-SHA256 vectors: RFC 4231 TC1/2/3/4/6/7 + Weft boundary cases,
//       digests byte-identical to the shared fixture
//       (fixtures/xlang-verifiedweft/hmac-vectors.json, cross-checked against
//       node:crypto at generation time)
//   V2  key derivation: domain-separated key differs from raw secret; stable
//   V3  record roundtrip: encode -> decode+verify OK, zero-copy views correct
//   V4  tamper detection: EVERY byte of envelope, payload, and tag flipped
//       individually must fail verification (exhaustive single-bit flip)
//   V5  wrong key / wrong domain -> rejected
//   V6  constant-time equality: ct_eq == memcmp semantics on equal/unequal
//   V7  performance vs RFC 0005 claims: encode + decode per-frame cost
//       (report + soft gate: < 10 us/frame — the RFC's hard claim is 2.1 us;
//        the gate exists to catch gross regressions, not to re-ratify)
//   V8  HW dispatch equivalence: scalar vs accelerated digests identical
//       (fixture tags + 512 randomized buffers swept under BOTH regimes)
//   V9  pre-keyed verifier: accept/reject identical to weft_vw_verify across
//       a 1000-frame stream; tamper still red; state reusable
//   V10 batch decode-verify: all-OK path, first-bad-record stop, zero-copy
//       views, exact bytes_consumed, views_cap semantics, truncated tail
//
// Build: make -C core/c verified-test   — then ./core/c/verified-test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hmac.h"
#include "sha256.h"
#include "verified.h"
#include "weft.h"

static int g_fail = 0;

static void check(int cond, const char* name) {
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

static void hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

// --- V1: fixture vectors -----------------------------------------------------

typedef struct {
    const char* name;
    const char* key_hex;
    const char* data_hex;
    const char* tag_hex;
} vw_vector;

// Byte-identical to fixtures/xlang-verifiedweft/hmac-vectors.json (generated
// and cross-checked against node:crypto; RFC 4231 TC1-4,6,7 + Weft edges).
static const vw_vector VECTORS[] = {
    { "rfc4231-tc1",
      "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
      "4869205468657265",
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
    { "rfc4231-tc2",
      "4a656665",
      "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
    { "rfc4231-tc3",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
      "dddddddddddddddddddddddddddddddddddd",
      "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe" },
    { "rfc4231-tc4",
      "0102030405060708090a0b0c0d0e0f10111213141516171819",
      "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
      "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
      "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b" },
    { "rfc4231-tc6",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaa",
      "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a"
      "65204b6579202d2048617368204b6579204669727374",
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" },
    { "rfc4231-tc7",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaa",
      "5468697320697320612074657374207573696e672061206c6172676572207468"
      "616e20626c6f636b2d73697a65206b657920616e642061206c61726765722074"
      "68616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565"
      "647320746f20626520686173686564206265666f7265206265696e6720757365"
      "642062792074686520484d414320616c676f726974686d2e",
      "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2" },
    { "weft-empty-payload",
      "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a",
      "",
      "87a26610b4e32f22d6d403b2397f534fb64c83b15aa53deaec60b1afa31dbb74" },
    { "weft-one-byte",
      "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a",
      "ff",
      "869b6896716dbdbce95aa32d75657fae807c82b8d52c25c83b7afa617271b7c8" },
};


static void test_v1_vectors(void) {
    printf("V1: HMAC-SHA256 fixture vectors (RFC 4231 + boundaries)\n");
    // Byte-identical to fixtures/xlang-verifiedweft/hmac-vectors.json (RFC 4231
    // TC1-4,6,7 + Weft boundary cases; digests cross-checked against
    // node:crypto at fixture generation time).
    for (size_t i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++) {
        const vw_vector* v = &VECTORS[i];
        const size_t key_len = strlen(v->key_hex) / 2;
        const size_t data_len = strlen(v->data_hex) / 2;
        uint8_t* key = malloc(key_len ? key_len : 1);
        uint8_t* data = malloc(data_len ? data_len : 1);
        uint8_t tag[HMAC_SHA256_TAG_LEN];
        char tag_hex[HMAC_SHA256_TAG_LEN * 2 + 1];

        hex_to_bytes(v->key_hex, key, key_len);
        hex_to_bytes(v->data_hex, data, data_len);
        hmac_sha256(key, key_len, data, data_len, tag);
        for (int j = 0; j < HMAC_SHA256_TAG_LEN; j++) {
            sprintf(tag_hex + 2 * j, "%02x", tag[j]);
        }

        const size_t expect_len = strlen(v->tag_hex);
        const int ok = strncmp(tag_hex, v->tag_hex, expect_len) == 0;
        if (!ok) {
            printf("    %s: got %s want %s*\n", v->name, tag_hex, v->tag_hex);
        }
        check(ok, v->name);
        free(key);
        free(data);
    }
}

// --- V2: key derivation ------------------------------------------------------

static void test_v2_derive_key(void) {
    printf("V2: domain-separated key derivation\n");
    uint8_t k1[WEFT_VW_KEY_LEN], k2[WEFT_VW_KEY_LEN], k3[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"secret-abc", 10, k1);
    weft_vw_derive_key((const uint8_t*)"secret-abc", 10, k2);
    weft_vw_derive_key((const uint8_t*)"secret-XYZ", 10, k3);

    int stable = memcmp(k1, k2, WEFT_VW_KEY_LEN) == 0;
    check(stable, "derivation is deterministic");
    int differs = memcmp(k1, k3, WEFT_VW_KEY_LEN) != 0;
    check(differs, "different secret -> different key");

    uint8_t secret_tag[HMAC_SHA256_TAG_LEN];
    hmac_sha256((const uint8_t*)"secret-abc", 10,
                (const uint8_t*)"Weft-VerifiedWeft-v1:key", 24, secret_tag);
    check(memcmp(k1, secret_tag, WEFT_VW_KEY_LEN) == 0,
          "key = HMAC(secret, domain) exactly");
}

// --- V3: record roundtrip ------------------------------------------------------

static void test_v3_roundtrip(void) {
    printf("V3: auth record roundtrip (zero-copy decode)\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"stream-key", 10, key);

    for (size_t plen = 0; plen <= 300; plen += 64) {
        uint8_t envelope[16];
        uint8_t payload[300];
        uint8_t record[16 + 300 + 32];
        weft_envelope_encode_v1(envelope, (uint32_t)(plen + 1), (uint32_t)plen);
        for (size_t i = 0; i < plen; i++) {
            payload[i] = (uint8_t)(i * 7 + 1);
        }

        weft_vw_signer_t s;
        weft_vw_signer_init(&s, key);
        uint8_t tag[WEFT_VW_TAG_LEN];
        weft_vw_sign(&s, envelope, payload, plen, tag);

        const size_t rec_len =
            weft_vw_record_encode(envelope, payload, plen, tag, record, sizeof(record));
        if (rec_len == 0) {
            check(0, "record encode size");
            return;
        }

        const uint8_t* env2 = NULL;
        const uint8_t* pay2 = NULL;
        size_t plen2 = 0;
        const weft_vw_result_t vr =
            weft_vw_record_decode_verify(key, record, rec_len, &env2, &pay2, &plen2);
        if (vr != WEFT_VW_OK) {
            check(0, "record decode+verify");
            return;
        }
        if (plen2 != plen || memcmp(pay2, payload, plen) != 0 ||
            memcmp(env2, envelope, 16) != 0) {
            check(0, "zero-copy views match inputs");
            return;
        }
    }
    check(1, "encode -> decode+verify across payload sizes 0..300");
}

// --- V4: exhaustive tamper detection -------------------------------------------

static void test_v4_tamper(void) {
    printf("V4: exhaustive single-bit tamper detection\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"tamper-key", 10, key);

    const size_t plen = 48;
    uint8_t envelope[16], payload[48], tag[WEFT_VW_TAG_LEN];
    weft_envelope_encode_v1(envelope, 7, (uint32_t)plen);
    for (size_t i = 0; i < plen; i++) payload[i] = (uint8_t)(i * 13 + 5);

    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);
    weft_vw_sign(&s, envelope, payload, plen, tag);

    // Flip every single bit of every region; each must be rejected.
    long rejected = 0;
    long total = 0;
    for (size_t byte_i = 0; byte_i < 16; byte_i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[16];
            memcpy(bad, envelope, 16);
            bad[byte_i] ^= (uint8_t)(1 << bit);
            if (weft_vw_verify(key, bad, payload, plen, tag) == WEFT_VW_ERR_TAG) rejected++;
            total++;
        }
    }
    for (size_t byte_i = 0; byte_i < plen; byte_i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[48];
            memcpy(bad, payload, plen);
            bad[byte_i] ^= (uint8_t)(1 << bit);
            if (weft_vw_verify(key, envelope, bad, plen, tag) == WEFT_VW_ERR_TAG) rejected++;
            total++;
        }
    }
    for (size_t byte_i = 0; byte_i < WEFT_VW_TAG_LEN; byte_i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[WEFT_VW_TAG_LEN];
            memcpy(bad, tag, WEFT_VW_TAG_LEN);
            bad[byte_i] ^= (uint8_t)(1 << bit);
            if (weft_vw_verify(key, envelope, payload, plen, bad) == WEFT_VW_ERR_TAG) rejected++;
            total++;
        }
    }
    check(rejected == total, "all 768 single-bit flips rejected");
}

// --- V5: wrong key / short record / bad magic -----------------------------------

static void test_v5_rejections(void) {
    printf("V5: wrong key, short record, bad magic\n");
    uint8_t key[WEFT_VW_KEY_LEN], other[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"stream-key", 10, key);
    weft_vw_derive_key((const uint8_t*)"other-key", 9, other);

    const size_t plen = 24;
    uint8_t envelope[16], payload[24], tag[WEFT_VW_TAG_LEN];
    weft_envelope_encode_v1(envelope, 3, (uint32_t)plen);
    memset(payload, 0xAB, plen);

    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);
    weft_vw_sign(&s, envelope, payload, plen, tag);

    check(weft_vw_verify(other, envelope, payload, plen, tag) == WEFT_VW_ERR_TAG,
          "wrong key rejected");

    uint8_t short_rec[40];  // < 16 + 32
    memset(short_rec, 0, sizeof(short_rec));
    check(weft_vw_record_decode_verify(key, short_rec, sizeof(short_rec), NULL, NULL, NULL) ==
              WEFT_VW_ERR_SHORT,
          "short record rejected");

    uint8_t bad_magic[16 + plen + 32];
    uint8_t env_bad[16];
    weft_envelope_encode_v1(env_bad, 3, (uint32_t)plen);
    env_bad[0] = 'X';
    weft_vw_record_encode(env_bad, payload, plen, tag, bad_magic, sizeof(bad_magic));
    check(weft_vw_record_decode_verify(key, bad_magic, sizeof(bad_magic), NULL, NULL, NULL) ==
              WEFT_VW_ERR_BAD_MAGIC,
          "bad magic rejected");

    // Truncated record (payload claims more bytes than exist).
    uint8_t trunc[16 + 8];
    memcpy(trunc, envelope, 16);
    check(weft_vw_record_decode_verify(key, trunc, sizeof(trunc), NULL, NULL, NULL) ==
              WEFT_VW_ERR_SHORT,
          "geometry overflow rejected");
}

// --- V6: constant-time equality semantics ----------------------------------------

static void test_v6_ct_eq(void) {
    printf("V6: constant-time equality semantics\n");
    const uint8_t a[32] = { 0 };
    uint8_t b[32] = { 0 };
    check(weft_vw_ct_eq(a, b, 32) == 1, "equal buffers -> 1");
    b[0] = 1;
    check(weft_vw_ct_eq(a, b, 32) == 0, "first-byte diff -> 0");
    b[0] = 0;
    b[31] = 0x80;
    check(weft_vw_ct_eq(a, b, 32) == 0, "last-byte diff -> 0");
    check(weft_vw_ct_eq(a, a, 0) == 1, "zero-length -> 1");
}

// --- V7: performance vs RFC claims ------------------------------------------------

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void test_v7_perf(void) {
    printf("V7: per-frame cost vs RFC 0005 claims (2.10 us enc / 2.09 us dec / 4.19 us rtt)\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"bench-key", 9, key);

    const size_t plen = 64;  // RFC benchmark payload size
    const long frames = 200000;
    uint8_t envelope[16], payload[64], tag[WEFT_VW_TAG_LEN];
    weft_envelope_encode_v1(envelope, 1, (uint32_t)plen);
    memset(payload, 0x5A, plen);

    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    // Per-frame tags: every verify call checks its own frame's tag (no
    // last-frame shortcut). 32 B x 200k = 6.4 MB, heap.
    uint8_t* tags = malloc((size_t)frames * WEFT_VW_TAG_LEN);

    // Sign (encode).
    double t0 = now_ms();
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;  // seq varies — keeps the signer honest
        weft_vw_sign(&s, envelope, payload, plen, tags + (size_t)i * WEFT_VW_TAG_LEN);
    }
    const double enc_us = (now_ms() - t0) * 1000.0 / (double)frames;

    // Verify (decode).
    t0 = now_ms();
    long ok = 0;
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;
        ok += (weft_vw_verify(key, envelope, payload, plen,
                              tags + (size_t)i * WEFT_VW_TAG_LEN) == WEFT_VW_OK);
    }
    const double dec_us = (now_ms() - t0) * 1000.0 / (double)frames;
    free(tags);

    printf("    measured: enc %.2f us | dec %.2f us | rtt %.2f us | auth-throughput %.0f frames/s\n",
           enc_us, dec_us, enc_us + dec_us, 1e6 / (enc_us + dec_us));

    check(ok == frames, "all verify calls OK during bench");
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    check(enc_us + dec_us < 50.0, "rtt < 50 us under ASan");
#else
    check(enc_us + dec_us < 10.0, "rtt < 10 us (soft gate vs RFC 4.19 us claim)");
#endif
}

// --- V8: HW dispatch equivalence (Series 6) ------------------------------------
// The accelerated compression must be DIGEST-IDENTICAL to the scalar
// reference: same fixture vectors, same HMAC tags, same record roundtrip —
// swept under BOTH regimes (force_scalar, then force_auto). Acceleration is
// a runtime decision, never a semantic one (sha256.h contract).

static void test_v8_hw_equivalence(void) {
    printf("V8: HW dispatch equivalence (scalar vs accelerated digests)\n");

    const weft_sha256_impl_t probe = weft_sha256_hw_probe();
    printf("    probe: %s\n", probe == WEFT_SHA256_X86_SHA_NI ? "x86-sha-ni"
                                 : probe == WEFT_SHA256_ARM_CE ? "armv8-ce"
                                 : "scalar-only");

    // 1) Fixture vectors under the scalar regime (already covered by V1 under
    //    auto, but re-check under force so the sweep is explicit).
    weft_sha256_force_scalar();
    check(weft_sha256_active_impl() == WEFT_SHA256_SCALAR, "force_scalar pins scalar");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"v8-sweep", 8, key);
    uint8_t env[16], tag_s[WEFT_VW_TAG_LEN], tag_hw[WEFT_VW_TAG_LEN];
    uint8_t payload[197];  // awkward length: crosses block boundaries
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 31 + 7);
    weft_envelope_encode_v1(env, 0xC0FFEE, (uint32_t)sizeof payload);

    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);
    weft_vw_sign(&s, env, payload, sizeof payload, tag_s);

    // 2) Same input under the auto/HW regime: byte-identical tag.
    weft_sha256_force_auto();
    const weft_sha256_impl_t active = weft_sha256_active_impl();
    printf("    active: %s\n", active == WEFT_SHA256_X86_SHA_NI ? "x86-sha-ni"
                               : active == WEFT_SHA256_ARM_CE ? "armv8-ce"
                               : "scalar (no accelerator on this CPU)");
    weft_vw_signer_init(&s, key);
    weft_vw_sign(&s, env, payload, sizeof payload, tag_hw);
    check(memcmp(tag_s, tag_hw, WEFT_VW_TAG_LEN) == 0,
          "signer tag identical across regimes");

    // 3) Randomized buffer sweep, both regimes, digests compared pairwise.
    uint32_t rng = 0x9E3779B9u;
    int mismatches = 0;
    for (int t = 0; t < 512; t++) {
        size_t n = (size_t)(rng % 512);
        uint8_t* buf = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            buf[i] = (uint8_t)rng;
        }
        uint8_t d_s[SHA256_DIGEST_LEN], d_hw[SHA256_DIGEST_LEN];
        weft_sha256_force_scalar();
        sha256(buf, n, d_s);
        weft_sha256_force_auto();
        sha256(buf, n, d_hw);
        if (memcmp(d_s, d_hw, SHA256_DIGEST_LEN) != 0) mismatches++;
        free(buf);
    }
    check(mismatches == 0, "512 random buffers (0..511 B) digest-identical");
    check(weft_vw_verify(key, env, payload, sizeof payload, tag_s) == WEFT_VW_OK,
          "cross-regime verify: scalar-signed tag verifies under HW");
}

// --- V9: pre-keyed verifier (Series 6) ------------------------------------------
// weft_vw_verifier_verify must accept/reject EXACTLY like weft_vw_verify,
// across a long stream of frames — the key schedule is amortized, the
// semantics are not.

static void test_v9_prekeyed_verifier(void) {
    printf("V9: pre-keyed verifier == one-shot verifier semantics\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"v9-stream", 10, key);

    weft_vw_verifier_t v;
    weft_vw_verifier_init(&v, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    long agree = 0, tamper_rejected = 0;
    const long frames = 1000;
    uint8_t env[16], tag[WEFT_VW_TAG_LEN], payload[64];
    for (long i = 0; i < frames; i++) {
        weft_envelope_encode_v1(env, (uint32_t)i, 64);
        for (int j = 0; j < 64; j++) payload[j] = (uint8_t)(i + j);
        weft_vw_sign(&s, env, payload, 64, tag);

        const weft_vw_result_t a = weft_vw_verifier_verify(&v, env, payload, 64, tag);
        const weft_vw_result_t b = weft_vw_verify(key, env, payload, 64, tag);
        if (a == b && a == WEFT_VW_OK) agree++;

        // Tampered tag must be rejected by the pre-keyed path too (and the
        // verifier must still be usable afterwards — pad reseeded).
        tag[0] ^= 0x01;
        if (weft_vw_verifier_verify(&v, env, payload, 64, tag) == WEFT_VW_ERR_TAG)
            tamper_rejected++;
        tag[0] ^= 0x01;
    }
    check(agree == frames, "1000 frames: pre-keyed == one-shot, all OK");
    check(tamper_rejected == frames, "1000 tampered tags rejected, state reusable");

    // Re-verify the FIRST frame's tag after 1000 others: key state intact.
    weft_envelope_encode_v1(env, 0, 64);
    for (int j = 0; j < 64; j++) payload[j] = (uint8_t)j;
    weft_vw_sign(&s, env, payload, 64, tag);
    check(weft_vw_verifier_verify(&v, env, payload, 64, tag) == WEFT_VW_OK,
          "verifier state stable after long stream");
}

// --- V10: batch decode-verify (Series 6) ----------------------------------------
// The stream-consumer API: all-OK path, first-bad-record stop semantics,
// zero-copy views, exact bytes_consumed, drop-and-count contract.

static void test_v10_batch(void) {
    printf("V10: batch decode-verify (stream consumer path)\n");
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"v10-batch", 9, key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    const size_t N = 500, plen = 48;
    const size_t rec_len = WEFT_VW_ENVELOPE_LEN + plen + WEFT_VW_TAG_LEN;
    uint8_t* stream = malloc(N * rec_len);
    uint8_t env[16], tag[WEFT_VW_TAG_LEN], payload[plen];
    for (size_t i = 0; i < N; i++) {
        weft_envelope_encode_v1(env, (uint32_t)(i * 3 + 1), (uint32_t)plen);
        for (size_t j = 0; j < plen; j++) payload[j] = (uint8_t)(i ^ j);
        weft_vw_sign(&s, env, payload, plen, tag);

        const size_t n = weft_vw_record_encode(env, payload, plen, tag,
                                               stream + i * rec_len, rec_len);
        (void)n;
    }

    weft_vw_record_view_t views[N];
    size_t n_verified = 0, consumed = 0;
    weft_vw_result_t r = weft_vw_batch_decode_verify(key, stream, N * rec_len,
                                                     views, N, &n_verified, &consumed);
    check(r == WEFT_VW_OK && n_verified == N, "500-record stream all OK");
    check(consumed == N * rec_len, "bytes_consumed exact (full stream)");
    int views_ok = 1;
    for (size_t i = 0; i < N; i++) {
        if (views[i].envelope != stream + i * rec_len) views_ok = 0;
        if (views[i].payload != stream + i * rec_len + 16) views_ok = 0;
        if (views[i].payload_len != plen) views_ok = 0;
        if (views[i].seq != i * 3 + 1) views_ok = 0;
    }
    check(views_ok, "views zero-copy into src, seq decoded");

    // Tamper record 137's payload: batch stops there, prefix stays good.
    const size_t k = 137;
    stream[k * rec_len + 20] ^= 0x40;  // payload byte 4
    r = weft_vw_batch_decode_verify(key, stream, N * rec_len, views, N,
                                    &n_verified, &consumed);
    check(r == WEFT_VW_ERR_TAG, "tampered record rejected (code 3)");
    check(n_verified == k, "good prefix counted (137)");
    check(consumed == k * rec_len, "bytes_consumed stops at the bad record");

    // views_cap smaller than the stream: still verifies all, fills only cap.
    n_verified = 0;
    r = weft_vw_batch_decode_verify(key, stream, k * rec_len, views, 10,
                                    &n_verified, &consumed);
    check(r == WEFT_VW_OK && n_verified == k, "cap-limited views still verify all");

    // Truncated tail (< envelope+tag): ignored, not an error.
    n_verified = 0; consumed = 0;
    r = weft_vw_batch_decode_verify(key, stream, k * rec_len + 17, NULL, 0,
                                    &n_verified, &consumed);
    check(r == WEFT_VW_OK && n_verified == k && consumed == k * rec_len,
          "truncated tail ignored (caller decides truncation policy)");

    free(stream);
}

int main(void) {
    printf("VerifiedWeft V-series (RFC 0005) — C driver layer\n");
    test_v1_vectors();
    test_v2_derive_key();
    test_v3_roundtrip();
    test_v4_tamper();
    test_v5_rejections();
    test_v6_ct_eq();
    test_v7_perf();
    test_v8_hw_equivalence();
    test_v9_prekeyed_verifier();
    test_v10_batch();
    printf("\nverdict: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
