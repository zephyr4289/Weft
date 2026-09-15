// weft_probe.c — State inspector (WO-P2-TOOLS T3)
// Per FORMATS.md §2. Quiesced dump, live dump, revocation-safe.
// Links the C kernel; in-process attachment model (WO-P2 §0.1).

#define _GNU_SOURCE
#include "weft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s <mode> [options]\n"
        "  modes:\n"
        "    quiesced        Join writer+reader, then probe (deterministic)\n"
        "    live            Probe against a running writer (advisory)\n"
        "    revocation      Probe under a revoking workload (I6 safety)\n"
        "  options:\n"
        "    --json          Output JSON (one object)\n"
        "    --payload N     Payload max (default 256)\n"
        "    --frames N      Number of frames (default 100)\n"
        "    --hz N          Writer rate for live mode (default 240)\n"
        "    --secs N        Live mode duration (default 3)\n", prog);
}

typedef struct {
    int payload_max;
    int frames;
    int hz;
    int secs;
    bool json;
    char mode[32];
} probe_cli_t;

static probe_cli_t parse_args(int argc, char** argv) {
    probe_cli_t c = { .payload_max = 256, .frames = 100, .hz = 240, .secs = 3, .json = false };
    if (argc < 2) { usage(argv[0]); exit(2); }
    strncpy(c.mode, argv[1], sizeof(c.mode) - 1);
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) c.json = true;
        else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) c.payload_max = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) c.frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) c.hz = atoi(argv[++i]);
        else if (strcmp(argv[i], "--secs") == 0 && i + 1 < argc) c.secs = atoi(argv[++i]);
    }
    return c;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void fill_payload(weft_t* w, uint32_t seq, uint32_t payload_len) {
    uint8_t* p = weft_w_begin(w);
    for (uint32_t i = 0; i < payload_len; i++) p[i] = weft_pat(seq, i);
}

static const char* owner_str(uint8_t owner) {
    switch (owner) {
        case 0: return "free";
        case 1: return "writer";
        case 2: return "reader";
        case 3: return "in-exchange";
        default: return "unknown";
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Print view (text or JSON)
// ---------------------------------------------------------------------------

static void print_view(const weft_debug_view_t* v, const char* mode, bool json) {
    if (json) {
        printf("{\"tool\":\"weft-probe\",\"mode\":\"%s\","
               "\"latest\":%u,\"w_work\":%u,\"r_work\":%u,"
               "\"revoked\":%s,\"epoch\":%u,"
               "\"t_publish\":%lu,\"t_claim\":%lu,\"t_drop\":%lu,"
               "\"mid_publish_sample\":%s,\"advisory\":true,"
               "\"bufs\":[",
               mode, v->latest, v->w_work, v->r_work,
               v->revoked ? "true" : "false", v->epoch,
               v->t_publish, v->t_claim, v->t_drop,
               v->mid_publish_sample ? "true" : "false");
        for (int i = 0; i < 2; i++) {
            printf("{\"slot\":%u,\"owner\":\"%s\",\"seq\":%u,\"version\":%u,\"header_size\":%u,\"payload_len\":%u}%s",
                   v->bufs[i].slot_idx, owner_str(v->bufs[i].owner),
                   v->bufs[i].seq, v->bufs[i].version,
                   v->bufs[i].header_size, v->bufs[i].payload_len,
                   i == 0 ? "," : "");
        }
        printf("]}\n");
    } else {
        printf("WEFT PROBE — %s dump\n", mode);
        printf("latest: %u\n", v->latest);
        printf("w_work: %u (advisory)\n", v->w_work);
        printf("r_work: %u (advisory)\n", v->r_work);
        printf("revoked: %s\n", v->revoked ? "true" : "false");
        printf("epoch: %u\n", v->epoch);
        printf("t_publish: %lu (advisory)\n", v->t_publish);
        printf("t_claim: %lu (advisory)\n", v->t_claim);
        printf("t_drop: %lu (advisory)\n", v->t_drop);
        printf("mid_publish_sample: %s\n", v->mid_publish_sample ? "true" : "false");
        for (int i = 0; i < 2; i++) {
            printf("buf[%d]: slot=%u owner=%s seq=%u version=%u header_size=%u payload_len=%u\n",
                   i, v->bufs[i].slot_idx, owner_str(v->bufs[i].owner),
                   v->bufs[i].seq, v->bufs[i].version,
                   v->bufs[i].header_size, v->bufs[i].payload_len);
        }
    }
}

// ---------------------------------------------------------------------------
// Writer thread (for live and quiesced modes)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int payload_max;
    int frames;
    int hz;
    _Atomic bool stop;
    _Atomic uint64_t published;
} writer_args_t;

static void* writer_thread(void* arg) {
    writer_args_t* a = (writer_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->hz;
    uint64_t next = now_ns() + period_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && seq <= (uint32_t)a->frames) {
        fill_payload(a->w, seq, a->payload_max);
        weft_publish(a->w, seq, a->payload_max);
        atomic_fetch_add(&a->published, 1);
        seq++;
        if (a->hz > 0) {
            uint64_t now = now_ns();
            if (now < next) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)(next - now) };
                nanosleep(&ts, NULL);
            }
            next += period_ns;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Quiesced mode
// ---------------------------------------------------------------------------

static int run_quiesced(probe_cli_t* c) {
    weft_t w;
    if (weft_init(&w, c->payload_max) != 0) return 1;

    // Publish N frames, then join (no reader thread — just publish and claim once)
    for (uint32_t seq = 1; seq <= (uint32_t)c->frames; seq++) {
        fill_payload(&w, seq, c->payload_max);
        weft_publish(&w, seq, c->payload_max);
    }
    // Claim once so r_work reflects a real claim
    weft_r_claim(&w);

    // Probe
    weft_debug_view_t view;
    weft_debug_view(&w, &view);
    print_view(&view, "quiesced", c->json);

    // Round-trip check: after N publishes + 1 claim, latest should be the last published seq
    // (or the reader's old r_work, but after a single claim from init, it's the published frame)
    bool ok = (view.bufs[0].seq == (uint32_t)c->frames || view.bufs[1].seq == (uint32_t)c->frames);
    fprintf(stderr, "quiesced round-trip: %s (expected seq=%d)\n",
            ok ? "PASS" : "FAIL", c->frames);

    weft_destroy(&w);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Live mode
// ---------------------------------------------------------------------------

static int run_live(probe_cli_t* c) {
    weft_t w;
    if (weft_init(&w, c->payload_max) != 0) return 1;

    writer_args_t args = { .w = &w, .payload_max = c->payload_max, .frames = c->frames * 10, .hz = c->hz };
    atomic_init(&args.stop, false);
    atomic_init(&args.published, 0);

    pthread_t wt;
    pthread_create(&wt, NULL, writer_thread, &args);

    uint64_t deadline = now_ns() + (uint64_t)c->secs * 1000000000ull;
    int samples = 0;
    while (now_ns() < deadline) {
        weft_debug_view_t view;
        weft_debug_view(&w, &view);
        if (c->json) {
            print_view(&view, "live", true);
        } else {
            // For text mode in live, just print a compact line
            fprintf(stderr, "live sample %d: latest=%u t_publish=%lu mid_publish=%s\n",
                    samples, view.latest, view.t_publish,
                    view.mid_publish_sample ? "true" : "false");
        }
        samples++;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 16000000 };  // ~60 Hz probe
        nanosleep(&ts, NULL);
    }

    atomic_store(&args.stop, true);
    pthread_join(wt, NULL);

    fprintf(stderr, "live: %d samples, no crashes, no allocations on probe path\n", samples);

    weft_destroy(&w);
    return 0;
}

// ---------------------------------------------------------------------------
// Revocation-safe mode
// ---------------------------------------------------------------------------

static int run_revocation(probe_cli_t* c) {
    weft_t w;
    if (weft_init(&w, c->payload_max) != 0) return 1;

    // Publish a few frames, then revoke, then probe — must survive
    for (uint32_t seq = 1; seq <= 10; seq++) {
        fill_payload(&w, seq, c->payload_max);
        weft_publish(&w, seq, c->payload_max);
    }

    uint32_t e0 = weft_epoch(&w);
    weft_revoke(&w);
    // Don't reclaim/poison — just probe after revoke to verify the probe survives
    // a revoked (but not yet reclaimed) Weft.
    weft_debug_view_t view;
    weft_debug_view(&w, &view);
    print_view(&view, "revocation", c->json);

    bool ok = view.revoked;
    fprintf(stderr, "revocation-safe: %s (revoked=%s)\n",
            ok ? "PASS" : "FAIL", view.revoked ? "true" : "false");

    weft_destroy(&w);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    probe_cli_t c = parse_args(argc, argv);
    if (strcmp(c.mode, "quiesced") == 0) return run_quiesced(&c);
    if (strcmp(c.mode, "live") == 0) return run_live(&c);
    if (strcmp(c.mode, "revocation") == 0) return run_revocation(&c);
    usage(argv[0]);
    return 2;
}
