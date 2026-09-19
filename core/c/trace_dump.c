// trace_dump.c — RFC 0014 deterministic scenario emitter + reference tools.
//
// THE PARITY SCENARIO (fixtures/xlang-trace/ — every port runs exactly this):
//
//   payload_max = 64, seed = 0x00C0FFEE (xorshift32, 04-LITMUS §0.2)
//   seq = 0
//   for step in 0..N-1:
//     state = xorshift32(state); plen = state % 65            # 0..64
//     seq += 1
//     payload = pat(seq, 0..plen-1); r = publish(seq, plen)
//     if r == DROPPED_REVOKED:  emit DROP(aux=epoch, data=seq)
//                               emit ACK (data=epoch)
//     else:                     emit PUBLISH(aux=plen, data=seq)
//     state = xorshift32(state)
//     if state % 3 == 0: claim(); emit CLAIM(data=r_seq())
//     if step == N/2: e0 = epoch(); emit REVOKE(data=e0); revoke()
//
// Single-threaded by design: no scheduler noise, no wall clock — the event
// stream is a pure function of (N, seed). Byte-identity across ports is
// then a *provable* property, not a statistical one.
//
// Modes:
//   ./trace-dump hex  [N SEED]      — packed stream (8 B/event) as hex; THE
//                                     cross-port byte-identity surface
//   ./trace-dump file OUT [N SEED]  — write the .weftrec v4 container
//   ./trace-dump json [N SEED]      — JSON-lines export (schema-validated)
//   ./trace-dump validate F.weftrec — structural + CRC validation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weft.h"
#include "trace_rec.h"

#define SCENARIO_PAYLOAD_MAX 64u
#define SCENARIO_DEFAULT_N 2000u
#define SCENARIO_REVOKE_DIVISOR 2u  // revoke at step N/2

#define WEFT_CANARY_OK 0
#define WEFT_CANARY_MISMATCH 1

static inline int weft_r_canary_check(weft_t* w) {
    if (!w || !w->buf[w->r_work]) return WEFT_CANARY_MISMATCH;
    uint64_t canary;
    memcpy(&canary, (const uint8_t*)w->buf[w->r_work] + w->buf_size - 8, 8);
    return (canary == (uint64_t)weft_r_seq(w)) ? WEFT_CANARY_OK : WEFT_CANARY_MISMATCH;
}

static uint32_t g_state;
static uint32_t xs(void) { return weft_xorshift32(&g_state); }

static uint64_t t_publish_n, t_claim_n, t_drop_n, t_revoke_n, t_ack_n;

// The shared scenario — one emitter callback per event keeps all modes honest
// about emitting the SAME stream.
typedef void (*emit_fn)(void* ud, const weft_trace_event* e);

static void scenario_run(uint32_t n, uint32_t seed, emit_fn emit, void* ud) {
    weft_t w;
    if (weft_init(&w, SCENARIO_PAYLOAD_MAX) != 0) {
        fprintf(stderr, "scenario: init failed\n");
        exit(2);
    }
    g_state = seed ? seed : 0x9E3779B9u;
    uint32_t seq = 0;
    uint8_t payload[SCENARIO_PAYLOAD_MAX];

    uint32_t revoke_step = n / SCENARIO_REVOKE_DIVISOR;
    for (uint32_t step = 0; step < n; step++) {
        uint32_t plen = xs() % (SCENARIO_PAYLOAD_MAX + 1);
        seq += 1;
        for (uint32_t i = 0; i < plen; i++) payload[i] = weft_pat(seq, i);
        weft_w_write_payload(&w, payload, plen);
        weft_pub_result_t r = weft_publish(&w, seq, plen);
        if (r == WEFT_PUB_DROPPED_REVOKED) {
            uint32_t ep = weft_epoch(&w);
            weft_trace_event e1 = { WEFT_TRACE_DROP, 0, seq };
            weft_trace_event e2 = { WEFT_TRACE_ACK, 0, ep };
            t_drop_n++; t_ack_n++;
            emit(ud, &e1);
            emit(ud, &e2);
        } else {
            weft_trace_event e = { WEFT_TRACE_PUBLISH, plen, seq };
            t_publish_n++;
            emit(ud, &e);
        }
        uint32_t s2 = xs();
        if (s2 % 3 == 0) {
            (void)weft_r_claim(&w);
            weft_trace_event e = { WEFT_TRACE_CLAIM, 0, weft_r_seq(&w) };
            t_claim_n++;
            emit(ud, &e);
            // The boundary must hold on every claimed frame (TIER4 §3).
            if (weft_r_canary_check(&w) != WEFT_CANARY_OK) {
                weft_trace_event bad = { WEFT_TRACE_CANARY_FAIL, 0, weft_r_seq(&w) };
                emit(ud, &bad);
                fprintf(stderr, "scenario: canary check FAILED at step %u\n", step);
                exit(3);
            }
        }
        if (step == revoke_step) {
            uint32_t e0 = weft_epoch(&w);
            weft_revoke(&w);
            weft_trace_event e = { WEFT_TRACE_REVOKE, 0, e0 };
            t_revoke_n++;
            emit(ud, &e);
        }
    }
    weft_destroy(&w);
}

// ---- emit backends --------------------------------------------------------

typedef struct { weft_trace_writer w; } file_emit_t;

static void emit_file(void* ud, const weft_trace_event* e) {
    file_emit_t* fe = (file_emit_t*)ud;
    if (weft_trace_writer_event(&fe->w, e) != 0) {
        fprintf(stderr, "scenario: writer overflow\n");
        exit(2);
    }
}

typedef struct { uint8_t* buf; size_t cap; size_t off; } hex_emit_t;

static void emit_hex(void* ud, const weft_trace_event* e) {
    hex_emit_t* hx = (hex_emit_t*)ud;
    uint8_t rec[8];
    weft_trace_event_pack(e, rec);
    for (int i = 0; i < 8 && hx->off + 2 < hx->cap; i++) {
        static const char* hexd = "0123456789abcdef";
        hx->buf[hx->off++] = hexd[rec[i] >> 4];
        hx->buf[hx->off++] = hexd[rec[i] & 0xF];
    }
}

int main(int argc, char** argv) {
    const char* mode = (argc > 1) ? argv[1] : "hex";

    if (strcmp(mode, "validate") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: trace-dump validate F.weftrec\n"); return 2; }
        FILE* f = fopen(argv[2], "rb");
        if (!f) { perror("open"); return 2; }
        static uint8_t buf[32 + 12 * 100000];
        size_t len = fread(buf, 1, sizeof buf, f);
        fclose(f);
        uint32_t count = 0;
        if (weft_trace_validate(buf, len, &count) != 0) {
            printf("INVALID\n");
            return 1;
        }
        // full CRC + kind walk
        size_t cur = WEFT_TRACE_HDR_SIZE;
        weft_trace_event e;
        while (weft_trace_next(buf, len, &cur, &e) == 1) {}
        printf("VALID events=%u\n", count);
        return 0;
    }

    uint32_t n = SCENARIO_DEFAULT_N;
    uint32_t seed = 0x00C0FFEEu;
    if (mode && argc > 2 && strcmp(mode, "file") == 0) {
        if (argc > 3) n = (uint32_t)strtoul(argv[3], NULL, 0);
        if (argc > 4) seed = (uint32_t)strtoul(argv[4], NULL, 0);
    } else {
        if (argc > 2) n = (uint32_t)strtoul(argv[2], NULL, 0);
        if (argc > 3) seed = (uint32_t)strtoul(argv[3], NULL, 0);
    }

    if (strcmp(mode, "hex") == 0) {
        static uint8_t hex[8 * 12 * 400000 + 2];
        hex_emit_t hx = { hex, sizeof hex, 0 };
        scenario_run(n, seed, emit_hex, &hx);
        fwrite(hex, 1, hx.off, stdout);
        printf("\n");
        return 0;
    }

    if (strcmp(mode, "file") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: trace-dump file OUT [N SEED]\n"); return 2; }
        static uint8_t buf[32 + 12 * 400000];
        file_emit_t fe;
        weft_trace_writer_open(&fe.w, buf, sizeof buf);
        scenario_run(n, seed, emit_file, &fe);
        if (weft_trace_writer_close(&fe.w) != 0) { fprintf(stderr, "overflow\n"); return 2; }
        FILE* f = fopen(argv[2], "wb");
        if (!f) { perror("open"); return 2; }
        fwrite(buf, 1, fe.w.off, f);
        fclose(f);
        fprintf(stderr, "wrote %s (%zu bytes, %u events)\n", argv[2], fe.w.off, fe.w.count);
        return 0;
    }

    if (strcmp(mode, "json") == 0) {
        static uint8_t container[32 + 12 * 400000];
        file_emit_t fe;
        weft_trace_writer_open(&fe.w, container, sizeof container);
        scenario_run(n, seed, emit_file, &fe);
        weft_trace_writer_close(&fe.w);
        static uint8_t json[64 * 400000];
        size_t used = weft_trace_to_json(container, fe.w.off, json, sizeof json);
        if (used == 0) { fprintf(stderr, "json export failed\n"); return 1; }
        fwrite(json, 1, used, stdout);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s (hex|file|json|validate)\n", mode);
    return 2;
}
