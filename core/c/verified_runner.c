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

#include "verified.h"
#include "weft.h"

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

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  verified-runner gen <out.bin> <frames> <payload_len> <secret_hex>\n"
                "  verified-runner validate <in.bin> <frames> <payload_len> <secret_hex>\n"
                "  verified-runner tamper <in.bin> <out.bin> <byte_offset>\n");
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
    fprintf(stderr, "bad args\n");
    return 2;
}
