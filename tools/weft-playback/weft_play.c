// weft_play.c — Playback and structural validation tool for .weftrec v1
// Per FORMATS.md §1 and Directive 16 (T16.3, T16.4).
// Bit-exact, little-endian, structural validation, L8 skip-unknown foreign frame support.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define WREC_MAGIC 0x43455257u      // "WREC" LE
#define WREC_FORMAT_VERSION 1
#define WREC_HEADER_SIZE 32
#define WEFT_ENVELOPE_MAGIC 0x54464557u // "WEFT" LE

// ---------------------------------------------------------------------------
// CRC-32/zlib (reflected poly 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF)
// ---------------------------------------------------------------------------

static uint32_t crc_table[256];
static bool crc_table_init = false;

static void init_crc_table(void) {
    if (crc_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
    crc_table_init = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t crc32_compute(const uint8_t* buf, size_t len) {
    init_crc_table();
    return crc32_update(0, buf, len);
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s <cmd> <file.weftrec> [options]\n"
        "commands:\n"
        "  validate    Validate file header, record structure, CRCs, and L8 extensions\n"
        "  dump        Dump envelope headers and frame summary\n"
        "  stats       Print frame count, stale count, payload stats, and stream CRC\n"
        "  step        Step-by-step interactive / sequential record inspection\n",
        prog);
}

typedef struct {
    uint32_t frame_index;
    uint32_t rec_len;
    uint32_t magic;
    uint32_t seq;
    uint16_t version;
    uint16_t header_size;
    uint32_t payload_len;
    uint32_t crc;
    bool is_foreign;
    bool is_stale;
} frame_meta_t;

// ---------------------------------------------------------------------------
// Playback & Validation Engine
// ---------------------------------------------------------------------------

static int process_weftrec(const char* file, const char* mode, bool verbose) {
    FILE* f = fopen(file, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open %s\n", file);
        return 1;
    }

    uint8_t header[WREC_HEADER_SIZE];
    if (fread(header, 1, WREC_HEADER_SIZE, f) != WREC_HEADER_SIZE) {
        fprintf(stderr, "error: short read on 32-byte header\n");
        fclose(f);
        return 1;
    }

    uint32_t magic, flags, env_ver, header_frame_count, hdr_crc;
    uint16_t fmt_ver, hdr_size;
    memcpy(&magic, header + 0, 4);
    memcpy(&fmt_ver, header + 4, 2);
    memcpy(&hdr_size, header + 6, 2);
    memcpy(&flags, header + 8, 4);
    memcpy(&env_ver, header + 12, 4);
    memcpy(&header_frame_count, header + 16, 4);
    memcpy(&hdr_crc, header + 20, 4);

    if (magic != WREC_MAGIC) {
        fprintf(stderr, "error: bad magic 0x%08X (expected 0x%08X)\n", magic, WREC_MAGIC);
        fclose(f);
        return 1;
    }
    if (fmt_ver != WREC_FORMAT_VERSION) {
        fprintf(stderr, "error: bad format_version %u (expected %u)\n", fmt_ver, WREC_FORMAT_VERSION);
        fclose(f);
        return 1;
    }
    if (hdr_size < WREC_HEADER_SIZE) {
        fprintf(stderr, "error: bad header_size %u (< %d)\n", hdr_size, WREC_HEADER_SIZE);
        fclose(f);
        return 1;
    }

    uint32_t computed_hdr_crc = crc32_compute(header, 20);
    if (computed_hdr_crc != hdr_crc) {
        fprintf(stderr, "error: header CRC mismatch (computed=0x%08X, stored=0x%08X)\n",
                computed_hdr_crc, hdr_crc);
        fclose(f);
        return 1;
    }

    // Seek past any self-describing header extension
    if (hdr_size > WREC_HEADER_SIZE) {
        fseek(f, hdr_size, SEEK_SET);
    }

    printf("=== WEFTREC VALIDATION: %s ===\n", file);
    printf("Header: magic=WREC fmt_ver=%u env_ver=%u hdr_size=%u hdr_crc=0x%08X (VALID)\n",
           fmt_ver, env_ver, hdr_size, hdr_crc);
    printf("Declared header frame_count: %u\n", header_frame_count);

    uint64_t actual_count = 0;
    uint64_t foreign_count = 0;
    uint64_t stale_count = 0;
    uint32_t last_seq = 0;
    uint64_t total_payload_bytes = 0;
    uint32_t stream_crc = 0;

    while (1) {
        uint8_t rec_len_buf[4];
        size_t n = fread(rec_len_buf, 1, 4, f);
        if (n == 0) break; // Clean EOF
        if (n < 4) {
            fprintf(stderr, "error: partial record length at frame %lu\n", actual_count);
            fclose(f);
            return 1;
        }

        uint32_t rec_len;
        memcpy(&rec_len, rec_len_buf, 4);
        if (rec_len < 24) {
            fprintf(stderr, "error: record %lu rec_len %u is less than minimum 24 bytes\n", actual_count, rec_len);
            fclose(f);
            return 1;
        }

        uint8_t* body = (uint8_t*)malloc(rec_len - 4);
        if (!body) {
            fprintf(stderr, "error: malloc failed for record %lu (%u bytes)\n", actual_count, rec_len);
            fclose(f);
            return 1;
        }

        if (fread(body, 1, rec_len - 4, f) != rec_len - 4) {
            fprintf(stderr, "error: short read on record %lu body (%u bytes)\n", actual_count, rec_len - 4);
            free(body);
            fclose(f);
            return 1;
        }

        // Body layout: [16 B envelope][payload: rec_len - 24][4 B CRC]
        uint32_t payload_len = rec_len - 24;
        uint32_t stored_crc;
        memcpy(&stored_crc, body + 16 + payload_len, 4);

        // Verify CRC over envelope + payload (bytes 0 .. 16 + payload_len)
        uint32_t computed_crc = crc32_compute(body, 16 + payload_len);
        if (computed_crc != stored_crc) {
            fprintf(stderr, "error: record %lu CRC mismatch (computed=0x%08X, stored=0x%08X)\n",
                    actual_count, computed_crc, stored_crc);
            free(body);
            fclose(f);
            return 1;
        }

        // Check envelope magic
        uint32_t env_magic;
        memcpy(&env_magic, body, 4);

        frame_meta_t meta;
        meta.frame_index = (uint32_t)actual_count;
        meta.rec_len = rec_len;
        meta.magic = env_magic;
        meta.crc = stored_crc;
        meta.payload_len = payload_len;

        if (env_magic != WEFT_ENVELOPE_MAGIC) {
            // L8 skip-unknown foreign frame
            foreign_count++;
            meta.is_foreign = true;
            meta.seq = 0;
            meta.version = 0;
            meta.header_size = 0;
            meta.is_stale = false;
            if (verbose || strcmp(mode, "dump") == 0) {
                printf("[Frame %4lu] FOREIGN/UNKNOWN magic=0x%08X rec_len=%u (L8 skipped)\n",
                       actual_count, env_magic, rec_len);
            }
        } else {
            meta.is_foreign = false;
            memcpy(&meta.seq, body + 4, 4);
            memcpy(&meta.version, body + 8, 2);
            memcpy(&meta.header_size, body + 10, 2);

            meta.is_stale = (meta.seq <= last_seq && actual_count > 0);
            if (meta.is_stale) stale_count++;
            if (meta.seq > last_seq) last_seq = meta.seq;
            total_payload_bytes += payload_len;

            if (strcmp(mode, "dump") == 0 || (strcmp(mode, "step") == 0 && actual_count < 100)) {
                printf("[Frame %4lu] seq=%-6u ver=%u hdr_sz=%u payload_len=%-5u crc=0x%08X %s\n",
                       actual_count, meta.seq, meta.version, meta.header_size,
                       meta.payload_len, meta.crc, meta.is_stale ? "[STALE]" : "[FRESH]");
            }
        }

        stream_crc = crc32_update(stream_crc, body, 16 + payload_len);
        free(body);
        actual_count++;
    }

    fclose(f);

    printf("=== SUMMARY ===\n");
    printf("Total records processed: %lu\n", actual_count);
    printf("Foreign/unknown records (L8 skipped): %lu\n", foreign_count);
    printf("Fresh records: %lu\n", actual_count - foreign_count - stale_count);
    printf("Stale records: %lu\n", stale_count);
    printf("Total payload bytes: %lu\n", total_payload_bytes);
    printf("Stream checksum (CRC32): 0x%08X\n", stream_crc);

    if (header_frame_count > 0 && actual_count != header_frame_count) {
        fprintf(stderr, "error: frame_count mismatch (header=%u, actual=%lu)\n",
                header_frame_count, actual_count);
        return 1;
    }

    printf("Result: PASS (All %lu frames verified bit-exact and structurally sound)\n", actual_count);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    const char* cmd = argv[1];
    const char* file = argv[2];
    bool verbose = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) verbose = true;
    }

    if (strcmp(cmd, "validate") == 0 || strcmp(cmd, "dump") == 0 ||
        strcmp(cmd, "stats") == 0 || strcmp(cmd, "step") == 0) {
        return process_weftrec(file, cmd, verbose);
    }

    usage(argv[0]);
    return 2;
}
