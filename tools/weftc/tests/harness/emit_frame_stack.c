/* weftc E2E emit harness — kernel-shaped buffer producer.
 *
 * Stands in for "Engineer 2's C harness" in the Pillar 1 integration
 * contract: builds a REAL Weft buffer using the frozen kernel's own
 * envelope encoder, writes it to disk, and the managed backends (TS,
 * Python) read it back in their integration tests. Any byte drift between
 * the C world and the managed views fails the pipeline.
 *
 * Buffer layout (core/c/weft.h): [0..16) envelope | [16..80) telemetry
 * payload | [80..88) canary u64 LE = seq.
 *
 * Build: cc -std=c11 -I core/c -o /tmp/weftc_emit \
 *            tools/weftc/tests/harness/emit_frame_stack.c core/c/weft.c
 * Run:   /tmp/weftc_emit <output.bin>
 * Exit 0 on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "weft.h"

static void put_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_f32le(uint8_t* p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    put_u64le(p, u); /* only low 4 bytes matter; write via loop below */
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(u >> (8 * i));
}
static void put_f64le(uint8_t* p, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(u >> (8 * i));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.bin>\n", argv[0]);
        return 2;
    }
    const uint32_t seq = 0x0000BEEFu;
    const uint32_t payload_len = 64;

    uint8_t buf[88];
    memset(buf, 0, sizeof(buf));

    /* envelope: the frozen kernel's own encoder */
    weft_envelope_encode_v1(buf, seq, payload_len);

    /* payload: TelemetryFrame IR layout (tools/weftc/schema/fixtures) */
    uint8_t* p = buf + 16;
    put_u64le(p + 0, 0x8F4C1120A9B30012ull);          /* schemaId    @0 */
    put_u64le(p + 8, 1737504000123456789ull);         /* timestampNs @8 */
    put_f32le(p + 16, 1.5f);                          /* velocity[0] @16 */
    put_f32le(p + 20, -2.25f);                        /* velocity[1] @20 */
    put_f32le(p + 24, 4000.125f);                     /* velocity[2] @24 */
    put_f32le(p + 28, 1013.25f);                      /* pressurePa  @28 */
    p[32] = 7;                                        /* state       @32 */
    /* reserved @33..36 stays zero; gap @36..40 stays zero */
    put_f64le(p + 40, 37.5);                          /* gps[0]      @40 */
    put_f64le(p + 48, -122.25);                       /* gps[1]      @48 */
    {                                                 /* hash        @56 */
        static const uint8_t h[8] = {0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE};
        memcpy(p + 56, h, 8);
    }

    /* canary: u64 LE == seq (kernel canary rule) */
    put_u64le(buf + 80, seq);

    /* self-check via the kernel decoder before writing (fail loudly) */
    uint16_t v, hs; uint32_t s, pl;
    if (weft_envelope_decode(buf, sizeof(buf), &v, &hs, &s, &pl) != WEFT_DECODE_OK ||
        v != 1 || hs != 16 || s != seq || pl != payload_len) {
        fprintf(stderr, "emit_frame_stack: kernel self-check FAILED\n");
        return 1;
    }

    FILE* f = fopen(argv[1], "wb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        fprintf(stderr, "emit_frame_stack: short write\n");
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("emit_frame_stack: wrote %zu bytes to %s\n", sizeof(buf), argv[1]);
    return 0;
}
