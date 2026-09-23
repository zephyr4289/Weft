// flight_runner.c — Series 10 full-fabric integration demo (RFC 0016 §8
// evidence): the frozen kernel, the flight recorder, the governor, and the
// trend estimator wired together over ONE deterministic scenario, exported
// to BOTH surfaces (.weftrec v4 + .wsid sidecar) and rendered by the
// Perfetto bridge.
//
// The scenario (injected time — zero wall-clock reads, Law 4):
//   frames 0..199    healthy   — claim every frame
//   frames 200..399  lagging   — claim every 3rd frame (backlog builds)
//   frames 400..449  stall     — no claims at all
//   frames 450..599  recovery  — catch-up claims, then steady
// The consumer's staleness feeds the trend (predictive verdicts) and the
// governor (reactive ladder); every kernel decision, verdict, action, and
// sample lands in the recorder; the export is a provable artifact.
//
// Usage: flight-runner [--out flight.weftrec] [--sidecar flight.wsid]

#define _GNU_SOURCE
#include "weft.h"
#include "weft_trace.h"
#include "governor.h"
#include "weft_trend.h"
#include "trace_rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 600
#define OBS_CAP (FRAMES * 8 + 64)

typedef struct {
    uint32_t seq;
    uint64_t t_ns;
} pub_stamp;

static pub_stamp stamps[256];  // publish-time table (seq -> t_ns) for
                               // latency (256 >= max phase lag 183)

int main(int argc, char** argv) {
    const char* out_path = "flight.weftrec";
    const char* side_path = "flight.wsid";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--sidecar") == 0 && i + 1 < argc) side_path = argv[++i];
    }

    weft_t w;
    if (weft_init(&w, 256) != 0) {
        fprintf(stderr, "flight-runner: weft_init failed\n");
        return 1;
    }
    weft_trace_t rec;
    if (weft_trace_init(&rec, 4096) != 0) {
        fprintf(stderr, "flight-runner: recorder init failed\n");
        return 1;
    }
    weft_governor_t gov;
    weft_governor_init_default(&gov);
    weft_trend_t trend;
    weft_trend_init(&trend);

    uint8_t payload[256];
    uint32_t seq = 0, claimed_seq = 0;
    uint64_t t_ns = 0;
    const uint64_t FRAME_NS = 16666667ull;  // 60 Hz, injected
    uint32_t behind = 0;
    uint64_t decisions = 0;

    for (uint32_t f = 0; f < FRAMES; f++) {
        t_ns += FRAME_NS;

        // ---- producer: publish every frame (the river never stops) ----
        seq++;
        for (int j = 0; j < 256; j++) payload[j] = weft_pat(seq, (uint32_t)j);
        weft_w_write_payload(&w, payload, sizeof(payload));
        weft_pub_result_t pr = weft_publish(&w, seq, sizeof(payload));
        if (pr == WEFT_PUB_OK) {
            stamps[seq & 255] = (pub_stamp){seq, t_ns};
            weft_trace_event ev = {WEFT_TRACE_PUBLISH,
                                   (uint16_t)sizeof(payload), seq};
            weft_trace_emit_kernel(&rec, WEFT_TP_WRITER, &ev, t_ns);
        }

        // ---- consumer: the phase schedule ----
        uint32_t claims = 1;
        if (f >= 200 && f < 400) claims = (f % 3 == 0) ? 1 : 0;  // lag
        if (f >= 400 && f < 450) claims = 0;                      // stall
        if (f >= 450 && f < 460) claims = 3;                      // catch-up
        for (uint32_t c = 0; c < claims; c++) {
            uint32_t mine = weft_r_claim(&w);
            uint32_t got = weft_r_seq(&w);
            if (got != 0) claimed_seq = got;
            weft_trace_event ev = {WEFT_TRACE_CLAIM, 0, got};
            weft_trace_emit_kernel(&rec, WEFT_TP_READER, &ev, t_ns);
            // claim latency from the publish stamp (advisory metric)
            pub_stamp* st = &stamps[got & 255];
            if (st->seq == got && t_ns >= st->t_ns) {
                uint64_t lat = t_ns - st->t_ns;
                weft_trace_emit(&rec, WEFT_TP_READER, WEFT_XT_CLAIM_LATENCY,
                                0, (uint32_t)(lat > 0xFFFFFFFFull
                                              ? 0xFFFFFFFFull : lat), t_ns);
            }
        }

        // freshness (the governor's input — same count the cursor would give)
        behind = seq - claimed_seq;

        // ---- predictive: the trend estimator watches ----
        weft_trend_out_t to;
        weft_trend_verdict_t tv = weft_trend_observe(&trend, behind, &to);
        weft_trace_emit(&rec, WEFT_TP_GOVERNOR, WEFT_XT_TREND_VERDICT, 0,
                        ((uint32_t)tv << 24) | (to.pred_raw & 0xFFFFFF), t_ns);

        // ---- reactive: the governor ladder acts ----
        const weft_gov_action_t* act =
            weft_governor_step(&gov, behind, (int64_t)(t_ns / 1000000));
        decisions++;
        weft_trace_emit(&rec, WEFT_TP_GOVERNOR, WEFT_XT_GOVERNOR_ACTION, 0,
                        ((uint32_t)act->kind << 24) |
                        (((uint32_t)act->skip_n) & 0xFFFFFF), t_ns);

        // ---- periodic metric samples ----
        if (f % 16 == 0) {
            weft_trace_emit(&rec, WEFT_TP_GOVERNOR, WEFT_XT_FRESHNESS, 0,
                            behind, t_ns);
            weft_trace_emit(&rec, WEFT_TP_GOVERNOR, WEFT_XT_RING_DEPTH, 0,
                            behind, t_ns);
        }

        // ---- phase markers (devtools annotations) ----
        if (f == 200) weft_trace_emit(&rec, 5, WEFT_XT_MARKER, 0, 1, t_ns);  // lag begins
        if (f == 400) weft_trace_emit(&rec, 5, WEFT_XT_MARKER, 0, 2, t_ns);  // stall
        if (f == 450) weft_trace_emit(&rec, 5, WEFT_XT_MARKER, 0, 3, t_ns);  // recovery
    }

    // ---- export both surfaces (exhaustive drain: the demo wants EVERY
    // observation, so we loop weft_trace_drain to exhaustion — each call
    // advances the shard tickets, laps resync — then write the container
    // and sidecar directly through the public codecs; export_v4's single
    // conservative pass is the bounded-window API, the runner's is the
    // whole-tape API) ----
    static weft_trace_obs obs[OBS_CAP];
    size_t obs_n = 0;
    for (;;) {
        size_t n = weft_trace_drain(&rec, obs + obs_n, OBS_CAP - obs_n);
        if (n == 0) break;
        obs_n += n;
        if (obs_n >= OBS_CAP) break;  // declared cap; the recorder never
                                      // blocks (Law 1) and the lap is
                                      // counted in the shards
    }
    // v4 container: kernel kinds in drained order, back-refs assigned here
    static uint8_t v4buf[32 + 12 * OBS_CAP];
    static uint8_t sidbuf[32 + 20 * OBS_CAP];
    weft_trace_writer wr;
    weft_trace_writer_open(&wr, v4buf, sizeof(v4buf));
    uint32_t v4_index = 0;
    for (size_t i = 0; i < obs_n; i++) {
        if (obs[i].kind >= WEFT_TRACE_PUBLISH &&
            obs[i].kind <= WEFT_TRACE_CANARY_FAIL) {
            weft_trace_event ev = {obs[i].kind, obs[i].aux, obs[i].data};
            weft_trace_writer_event(&wr, &ev);
            obs[i].back_ref = v4_index++;
        } else {
            obs[i].back_ref = WEFT_SIDECAR_NO_REF;
        }
    }
    weft_trace_writer_close(&wr);
    size_t v4len = wr.off;
    size_t sidlen = weft_trace_export_sidecar(obs, obs_n, sidbuf, sizeof(sidbuf), 0);
    if (sidlen == 0) {
        fprintf(stderr, "flight-runner: sidecar export overflow\n");
        return 1;
    }

    FILE* f1 = fopen(out_path, "wb");
    FILE* f2 = fopen(side_path, "wb");
    if (!f1 || !f2) {
        fprintf(stderr, "flight-runner: cannot open outputs\n");
        return 1;
    }
    fwrite(v4buf, 1, v4len, f1);
    fwrite(sidbuf, 1, sidlen, f2);
    fclose(f1);
    fclose(f2);

    // validate what we wrote (the export must round-trip the codec)
    uint32_t count = 0;
    if (weft_trace_validate(v4buf, v4len, &count) != 0) {
        fprintf(stderr, "flight-runner: exported container INVALID\n");
        return 1;
    }

    printf("flight-runner: %u frames -> %u kernel events, %zu observations\n"
           "  v4 container: %s (%zu bytes, validated)\n"
           "  sidecar:      %s (%zu bytes)\n"
           "  final behind: %u | governor decisions: %llu | trend samples: %llu\n"
           "  render with: node tools/perfetto/weftrec2perfetto.mjs %s "
           "--sidecar %s --out trace.json\n",
           FRAMES, count, obs_n, out_path, v4len, side_path, sidlen,
           behind, (unsigned long long)decisions,
           (unsigned long long)trend.t_samples, out_path, side_path);

    weft_trace_destroy(&rec);
    weft_destroy(&w);
    return 0;
}
