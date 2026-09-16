// verified_runner.c — xlang sign/verify CLI for RFC 0005 VerifiedWeft.
//
// Modes (mirror of fanout-runner's dump-ring/validate-ring contract):
//   gen <out.bin> <frames> <payload_len> <secret_hex>
//       Write `frames` authenticated frame records: envelope v1 (seq =
//       record index, payload_len), payload = 04-LITMUS §0.1 mix32 stream,
//       32-byte HMAC tag per record. Deterministic.
//   validate <in.bin> <frames> <payload_len> <secret_hex>
//       Verify every record (zero-copy decode + constant-time tag check),
//       validating payload words bit-exactly. Any tamper/mismatch exits 1.
//   tamper <in.bin> <out.bin> <byte_offset>
//       Flip one bit at byte_offset — the negative leg of the xlang gate:
//       the peer's validate MUST reject the file.
//
// Build: make -C core/c verified-runner

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "verified.h"
#include "weft.h"

static double now_ms_runner(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// 04-LITMUS §0.1 mix32 — the shared deterministic payload generator.
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static int write_u64(FILE* f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return fwrite(b, 1, 8, f) == 8 ? 0 : -1;
}

static int read_u64(FILE* f, uint64_t* v) {
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return -1;
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r |= (uint64_t)b[i] << (8 * i);
    *v = r;
    return 0;
}

static int mode_gen(const char* path, long frames, long payload_len, const char* secret_hex) {
    const size_t plen = (size_t)payload_len;
    const size_t rec_len = 16 + plen + 32;
    uint8_t key[WEFT_VW_KEY_LEN];
    uint8_t envelope[16], tag[WEFT_VW_TAG_LEN];
    uint8_t* payload = malloc(plen ? plen : 1);
    uint8_t* record = malloc(rec_len);

    weft_vw_derive_key((const uint8_t*)secret_hex, strlen(secret_hex), key);
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);

    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen"); return 1; }
    // Header: magic u64, frames u64, payload_len u64 (little-endian).
    int rc = 0;
    rc |= write_u64(f, 0x57565731ULL);  // "VWV1"
    rc |= write_u64(f, (uint64_t)frames);
    rc |= write_u64(f, (uint64_t)plen);
    if (rc != 0) { fprintf(stderr, "header write failed\n"); fclose(f); return 1; }

    for (long i = 0; i < frames; i++) {
        for (size_t j = 0; j < plen; j += 4) {
            const uint32_t w = mix32((uint32_t)(i * 0x9E3779B9ULL + j));
            if (j + 4 <= plen) memcpy(payload + j, &w, 4);
            else memcpy(payload + j, &w, plen - j);
        }
        weft_envelope_encode_v1(envelope, (uint32_t)i, (uint32_t)plen);
        weft_vw_sign(&s, envelope, payload, plen, tag);
        const size_t n = weft_vw_record_encode(envelope, payload, plen, tag, record, rec_len);
        if (n != rec_len || fwrite(record, 1, rec_len, f) != rec_len) {
            fprintf(stderr, "record write failed at %ld\n", i);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    free(payload);
    free(record);
    printf("gen: %ld authenticated records (%zu B each) -> %s\n", frames, rec_len, path);
    return 0;
}

static int mode_validate(const char* path, long frames, long payload_len, const char* secret_hex) {
    const size_t plen = (size_t)payload_len;
    const size_t rec_len = 16 + plen + 32;
    uint8_t key[WEFT_VW_KEY_LEN];
    uint8_t* record = malloc(rec_len);

    weft_vw_derive_key((const uint8_t*)secret_hex, strlen(secret_hex), key);

    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    uint64_t magic = 0, hdr_frames = 0, hdr_plen = 0;
    int rc = 0;
    rc |= read_u64(f, &magic);
    rc |= read_u64(f, &hdr_frames);
    rc |= read_u64(f, &hdr_plen);
    if (rc != 0 || magic != 0x57565731ULL || hdr_frames != (uint64_t)frames ||
        hdr_plen != (uint64_t)plen) {
        fprintf(stderr, "bad file header (magic=%llx frames=%llu plen=%llu)\n",
                (unsigned long long)magic, (unsigned long long)hdr_frames,
                (unsigned long long)hdr_plen);
        fclose(f);
        return 1;
    }

    long verified = 0;
    for (long i = 0; i < frames; i++) {
        if (fread(record, 1, rec_len, f) != rec_len) {
            fprintf(stderr, "short read at record %ld\n", i);
            fclose(f);
            return 1;
        }
        const uint8_t* env = NULL;
        const uint8_t* pay = NULL;
        size_t got_plen = 0;
        const weft_vw_result_t vr =
            weft_vw_record_decode_verify(key, record, rec_len, &env, &pay, &got_plen);
        if (vr != WEFT_VW_OK) {
            fprintf(stderr, "record %ld: verify FAILED (code %d)\n", i, (int)vr);
            fclose(f);
            return 1;
        }
        // Bit-exact payload validation against the mix32 generator.
        uint32_t seq = 0;
        memcpy(&seq, env + 8, 4);
        if ((long)seq != i) {
            fprintf(stderr, "record %ld: seq mismatch (%u)\n", i, seq);
            fclose(f);
            return 1;
        }
        for (size_t j = 0; j < plen; j += 4) {
            const uint32_t w = mix32((uint32_t)(i * 0x9E3779B9ULL + j));
            uint32_t got = 0;
            const size_t take = (j + 4 <= plen) ? 4 : (plen - j);
            memcpy(&got, pay + j, take);
            const uint32_t mask = (take == 4) ? 0xFFFFFFFFu : ((1u << (8 * take)) - 1);
            if ((got & mask) != (w & mask)) {
                fprintf(stderr, "record %ld: payload word %zu mismatch\n", i, j);
                fclose(f);
                return 1;
            }
        }
        verified++;
    }
    fclose(f);
    free(record);
    printf("validate: %ld/%ld records verified (tag + payload bit-exact)\n", verified, frames);
    return verified == frames ? 0 : 1;
}

static int mode_tamper(const char* in, const char* out, long byte_offset) {
    FILE* fi = fopen(in, "rb");
    if (!fi) { perror("fopen in"); return 1; }
    FILE* fo = fopen(out, "wb");
    if (!fo) { perror("fopen out"); fclose(fi); return 1; }
    int c;
    long pos = 0;
    while ((c = fgetc(fi)) != EOF) {
        if (pos == byte_offset) {
            c ^= 0x01;  // single-bit flip
        }
        fputc(c, fo);
        pos++;
    }
    fclose(fi);
    fclose(fo);
    printf("tamper: bit flipped at byte %ld -> %s\n", byte_offset, out);
    return pos > byte_offset ? 0 : 1;
}


// ---------------------------------------------------------------------------
// bench — Series 6 evidence: scalar vs hardware-accelerated rates, plus the
// pre-keyed verifier and the batch stream API. The same measurement shape as
// V7 (per-frame microseconds over a long run) so the numbers are comparable.
// ---------------------------------------------------------------------------

static const char* impl_name(int impl) {
    return impl == 1 ? "x86-sha-ni" : impl == 2 ? "armv8-ce" : "scalar";
}

static int mode_bench(long frames, long payload_len, const char* secret_hex) {
    const size_t plen = (size_t)payload_len;
    if (plen == 0 || plen > 4096) {
        fprintf(stderr, "bench: payload_len must be in [1, 4096]\n");
        return 2;
    }
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)secret_hex, strlen(secret_hex), key);

    uint8_t* payload = malloc(plen);
    for (size_t j = 0; j < plen; j++) payload[j] = (uint8_t)(j * 131 + 17);
    uint8_t envelope[16];
    weft_envelope_encode_v1(envelope, 1, (uint32_t)plen);
    uint8_t* tags = malloc((size_t)frames * WEFT_VW_TAG_LEN);

    printf("bench: %ld frames, %zu B payload, %zu B records\n",
           frames, plen, (size_t)16 + plen + 32);
    printf("    cpu probe: %s\n", impl_name(weft_sha256_hw_probe()));

    // -- sign (encode), scalar regime --
    weft_sha256_force_scalar();
    weft_vw_signer_t s;
    weft_vw_signer_init(&s, key);
    double t0 = now_ms_runner();
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;
        weft_vw_sign(&s, envelope, payload, plen, tags + (size_t)i * WEFT_VW_TAG_LEN);
    }
    const double enc_scalar = (now_ms_runner() - t0) * 1000.0 / (double)frames;

    // -- sign, auto regime (HW when available) --
    weft_sha256_force_auto();
    printf("    active:   %s\n", impl_name(weft_sha256_active_impl()));
    weft_vw_signer_init(&s, key);
    t0 = now_ms_runner();
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;
        weft_vw_sign(&s, envelope, payload, plen, tags + (size_t)i * WEFT_VW_TAG_LEN);
    }
    const double enc_auto = (now_ms_runner() - t0) * 1000.0 / (double)frames;

    // -- one-shot verify (per-call key schedule; the RFC 0005 shape) --
    weft_sha256_force_scalar();
    t0 = now_ms_runner();
    long ok = 0;
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;
        ok += (weft_vw_verify(key, envelope, payload, plen,
                              tags + (size_t)i * WEFT_VW_TAG_LEN) == WEFT_VW_OK);
    }
    const double dec_scalar = (now_ms_runner() - t0) * 1000.0 / (double)frames;

    // -- pre-keyed verifier (Series 6) --
    weft_sha256_force_auto();
    weft_vw_verifier_t v;
    weft_vw_verifier_init(&v, key);
    t0 = now_ms_runner();
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;
        ok += (weft_vw_verifier_verify(&v, envelope, payload, plen,
                                       tags + (size_t)i * WEFT_VW_TAG_LEN) == WEFT_VW_OK);
    }
    const double dec_prekeyed = (now_ms_runner() - t0) * 1000.0 / (double)frames;

    // -- batch stream API (sign a record stream once, then verify in one walk) --
    const size_t rec_len = 16 + plen + 32;
    uint8_t* stream = malloc((size_t)frames * rec_len);
    weft_vw_signer_init(&s, key);
    for (long i = 0; i < frames; i++) {
        envelope[8] = (uint8_t)i;
        weft_vw_sign(&s, envelope, payload, plen, tags);
        weft_vw_record_encode(envelope, payload, plen, tags,
                              stream + (size_t)i * rec_len, rec_len);
    }
    size_t n_verified = 0, consumed = 0;
    t0 = now_ms_runner();
    const weft_vw_result_t br = weft_vw_batch_decode_verify(
        key, stream, (size_t)frames * rec_len, NULL, 0, &n_verified, &consumed);
    const double batch_ms = now_ms_runner() - t0;

    printf("\n    %-28s %10s %10s\n", "path", "us/frame", "frames/s");
    printf("    %-28s %10.3f %10.0f\n", "sign (scalar)", enc_scalar, 1e6 / enc_scalar);
    printf("    %-28s %10.3f %10.0f\n", "sign (auto/HW)", enc_auto, 1e6 / enc_auto);
    printf("    %-28s %10.3f %10.0f\n", "verify one-shot (scalar)", dec_scalar, 1e6 / dec_scalar);
    printf("    %-28s %10.3f %10.0f\n", "verify pre-keyed (auto/HW)", dec_prekeyed, 1e6 / dec_prekeyed);
    printf("    %-28s %10.3f %10.0f\n", "batch decode-verify (auto/HW)",
           batch_ms * 1000.0 / (double)n_verified, n_verified / (batch_ms / 1000.0));
    printf("    batch verdict: %s (%zu verified, %zu bytes)\n",
           br == WEFT_VW_OK ? "OK" : "REJECTED", n_verified, consumed);
    printf("    all verifies OK: %s\n", (ok == frames * 2 && br == WEFT_VW_OK) ? "yes" : "NO");

    free(payload); free(tags); free(stream);
    return (ok == frames * 2 && br == WEFT_VW_OK && n_verified == (size_t)frames) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// batch-validate — validate a gen'd file through the batch stream API
// (the flight-recorder/bridge ingestion shape). Same acceptance as validate:
// every record verified, payload words bit-exact.
// ---------------------------------------------------------------------------

static int mode_batch_validate(const char* path, long frames, long payload_len,
                               const char* secret_hex) {
    const size_t plen = (size_t)payload_len;
    const size_t rec_len = 16 + plen + 32;
    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)secret_hex, strlen(secret_hex), key);

    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    uint64_t magic = 0, hdr_frames = 0, hdr_plen = 0;
    read_u64(f, &magic); read_u64(f, &hdr_frames); read_u64(f, &hdr_plen);
    if (magic != 0x57565731ULL || hdr_frames != (uint64_t)frames || hdr_plen != (uint64_t)plen) {
        fprintf(stderr, "bad header\n");
        fclose(f);
        return 1;
    }
    uint8_t* stream = malloc((size_t)frames * rec_len);
    if (fread(stream, 1, (size_t)frames * rec_len, f) != (size_t)frames * rec_len) {
        fprintf(stderr, "short read\n");
        fclose(f); free(stream);
        return 1;
    }
    fclose(f);

    weft_vw_record_view_t* views = malloc((size_t)frames * sizeof *views);
    size_t n_verified = 0, consumed = 0;
    const weft_vw_result_t r = weft_vw_batch_decode_verify(
        key, stream, (size_t)frames * rec_len, views, (size_t)frames,
        &n_verified, &consumed);
    if (r != WEFT_VW_OK || n_verified != (size_t)frames) {
        fprintf(stderr, "batch-validate: rejected at record %zu (code %d)\n",
                n_verified, (int)r);
        free(stream); free(views);
        return 1;
    }
    // Bit-exact payload check against mix32 — same contract as validate.
    for (long i = 0; i < frames; i++) {
        for (size_t j = 0; j < plen; j += 4) {
            const uint32_t w = mix32((uint32_t)(i * 0x9E3779B9ULL + j));
            uint32_t got = 0;
            const size_t take = (j + 4 <= plen) ? 4 : (plen - j);
            memcpy(&got, views[i].payload + j, take);
            const uint32_t mask = (take == 4) ? 0xFFFFFFFFu : ((1u << (8 * take)) - 1);
            if ((got & mask) != (w & mask)) {
                fprintf(stderr, "batch-validate: payload mismatch at record %ld\n", i);
                free(stream); free(views);
                return 1;
            }
        }
    }
    printf("batch-validate: %zu/%ld records verified in one walk (tag + payload bit-exact)\n",
           n_verified, frames);
    free(stream); free(views);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  verified-runner gen <out.bin> <frames> <payload_len> <secret_hex>\n"
                "  verified-runner validate <in.bin> <frames> <payload_len> <secret_hex>\n"
                "  verified-runner tamper <in.bin> <out.bin> <byte_offset>\n"
                "  verified-runner bench <frames> <payload_len> <secret_hex>\n"
                "  verified-runner batch-validate <in.bin> <frames> <payload_len> <secret_hex>\n");
        return 2;
    }

    if (strcmp(argv[1], "gen") == 0 && argc == 6) {
        return mode_gen(argv[2], atol(argv[3]), atol(argv[4]), argv[5]);
    }
    if (strcmp(argv[1], "validate") == 0 && argc == 6) {
        return mode_validate(argv[2], atol(argv[3]), atol(argv[4]), argv[5]);
    }
    if (strcmp(argv[1], "tamper") == 0 && argc == 5) {
        return mode_tamper(argv[2], argv[3], atol(argv[4]));
    }
    if (strcmp(argv[1], "bench") == 0 && argc == 5) {
        return mode_bench(atol(argv[2]), atol(argv[3]), argv[4]);
    }
    if (strcmp(argv[1], "batch-validate") == 0 && argc == 6) {
        return mode_batch_validate(argv[2], atol(argv[3]), atol(argv[4]), argv[5]);
    }
    fprintf(stderr, "bad args\n");
    return 2;
}
