/* weftrec_inspect.c — .weftrec trace inspector (Engineer 2's smoke tool).
 *
 * Loads a WEFTREC1 file into memory (setup allocation, outside Law 1's
 * steady state), validates it with the engine's reader, and prints the
 * header, a window of the index, and a full frame walk with CRC results.
 * Exit codes: 0 valid, 1 ladder refusal (see stderr).
 *
 * usage: weftrec-inspect <file.weftrec> [--index N] [--seek TS_NS]
 */
#define _POSIX_C_SOURCE 199309L
#include "weft_studio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint64_t seek_ts = 0;
    int have_seek = 0;
    int i;
    FILE *f;
    long fsize;
    uint8_t *buf;
    weftrec_reader_t r;
    int rc;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seek") == 0 && i + 1 < argc) {
            seek_ts = strtoull(argv[++i], NULL, 10);
            have_seek = 1;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: weftrec-inspect <file.weftrec> "
                "[--seek TS_NS]\n");
        return 2;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "weftrec-inspect: cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 64) {
        fprintf(stderr, "weftrec-inspect: ETRUNC (%ld bytes)\n", fsize);
        fclose(f);
        return 1;
    }
    /* the reader requires a 64 B-aligned pinned buffer (EALIGN gate) */
    {
        size_t rounded = ((size_t)fsize + 63u) & ~(size_t)63u;
        buf = (uint8_t *)aligned_alloc(64, rounded ? rounded : 64);
    }
    if (!buf) { fclose(f); return 2; }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "weftrec-inspect: short read\n");
        fclose(f);
        free(buf);
        return 2;
    }
    fclose(f);

    rc = weftrec_reader_open(&r, buf, (uint64_t)fsize);
    if (rc != WEFT_STUDIO_OK) {
        fprintf(stderr, "weftrec-inspect: open refused: %s\n",
                weft_studio_strerror(rc));
        free(buf);
        return 1;
    }
    printf("WEFTREC1 trace: %s\n", path);
    printf("  frames:        %llu\n", (unsigned long long)r.hdr->frame_count);
    printf("  first_ts_ns:   %llu\n",
           (unsigned long long)r.hdr->first_ts_ns);
    printf("  last_ts_ns:    %llu\n",
           (unsigned long long)r.hdr->last_ts_ns);
    printf("  streams:       %u\n", r.hdr->stream_cardinality);
    printf("  index_offset:  %llu\n",
           (unsigned long long)r.hdr->index_offset);
    printf("  crc32c kernel: %s\n",
           weftrec_crc32c_hw_active() ? "SSE4.2" : "slicing-by-8");

    if (have_seek) {
        weftrec_frame_view_t v;
        rc = weftrec_seek_timestamp(&r, seek_ts, &v);
        if (rc == WEFT_STUDIO_OK) {
            printf("  seek(%llu) -> frame #%llu @ %llu (ts %llu, stream "
                   "%llu, payload %llu, codec %s)\n",
                   (unsigned long long)seek_ts,
                   (unsigned long long)v.hdr->frame_seq,
                   (unsigned long long)v.file_offset,
                   (unsigned long long)v.hdr->timestamp_ns,
                   (unsigned long long)v.hdr->stream_id,
                   (unsigned long long)v.hdr->payload_size,
                   v.hdr->codec == WEFTREC_CODEC_DZV ? "dzv" : "raw");
        } else {
            printf("  seek(%llu) refused: %s\n",
                   (unsigned long long)seek_ts, weft_studio_strerror(rc));
        }
    }

    {
        weftrec_walker_t w;
        static uint8_t scratch[65536];
        weftrec_frame_view_t v;
        unsigned long long walked = 0;
        weftrec_walker_init(&w, &r, 0, scratch, sizeof scratch);
        printf("  frames (first 8):\n");
        while ((rc = weftrec_frame_next(&w, &v)) == WEFT_STUDIO_OK) {
            if (walked < 8) {
                printf("    #%llu ts=%llu stream=%llu payload=%llu "
                       "stored=%llu codec=%s crc=ok\n",
                       (unsigned long long)v.hdr->frame_seq,
                       (unsigned long long)v.hdr->timestamp_ns,
                       (unsigned long long)v.hdr->stream_id,
                       (unsigned long long)v.hdr->payload_size,
                       (unsigned long long)v.hdr->stored_size,
                       v.hdr->codec == WEFTREC_CODEC_DZV ? "dzv" : "raw");
            }
            walked++;
        }
        printf("  walked %llu frames; end: %s%s\n", walked,
               weft_studio_strerror(rc),
               (rc == WEFT_STUDIO_ETRUNC && w.truncated)
               ? " (torn tail)" : "");
    }
    free(buf);
    return 0;
}
