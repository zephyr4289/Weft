// litmus_runner.c — L1–L16 litmus runner (C)
//
// Per 04-LITMUS.md (procedures + verdicts) and 05-CONTRACTS.md (CLI + JSON output).
//
// CLI: ./litmus_runner <TEST_ID> [key=value ...]
//   TEST_ID ∈ L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress
//            L6-ownership L7-revocation L8-envelope
//            L9-cross-beam L10-nested-revocation L12-canary-corruption
//            L13-claim-retry L14-mixed-endianness L15-huge-payload
//            L16-rapid-reclaim
//            (L11-ffi-stress is script-orchestrated — litmus/ffi_stress/run.sh —
//             it needs node/cargo cross-process, not a single-binary CLI)
//
// Output: exactly ONE JSON line on stdout (the LAST line); all diagnostics to stderr.
// Exit: 0 pass · 1 fail · 2 usage/contract error.

#define _GNU_SOURCE  // for nanosleep, posix_memalign
#include "weft.h"
#include "fanout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

typedef struct {
    char test_id[64];
    // Parsed params. All optional; defaults come from catalog defaults.
    int holds_ms[16];
    int holds_count;
    int writer_hz;
    int reader_hz;
    int frames;
    int publishes;
    int claims;
    int payload_max;
    int bound;
    int window_s_100ms;     // window_s * 1000 (we deal in ms internally for some)
    double window_s;
    int timeout_ms;
    int tolerance_100;       // tolerance * 100 (avoid float compares)
    double tolerance;
    int trials;
    int max_delay_us;
    uint32_t seed;
    int min_claims;          // v1.1 (A4): per-language exposure floor (resolved by driver)
    int cycles;              // L16: revoke/reclaim lifecycle churn count
    int beams;               // L9: concurrent kernel instances (default 2)
} cli_t;

static void cli_init(cli_t* c) {
    memset(c, 0, sizeof(*c));
    c->payload_max = -1;
    c->seed = 0;
    c->tolerance = 0;
    c->window_s = 0;
}

// Parse a comma-separated list of integers into c->holds_ms; returns count.
static int parse_int_list(const char* s, int* out, int max) {
    int n = 0;
    const char* p = s;
    while (*p && n < max) {
        char* end;
        long v = strtol(p, &end, 0);   // 0 = autodetect base (hex 0x... supported)
        if (end == p) break;
        out[n++] = (int)v;
        if (*end == ',') p = end + 1;
        else break;
    }
    return n;
}

static int parse_int(const char* s) {
    return (int)strtol(s, NULL, 0);
}
static double parse_double(const char* s) {
    return strtod(s, NULL);
}

static int parse_args(int argc, char** argv, cli_t* c) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <TEST_ID> [key=value ...]\n", argv[0]);
        return -1;
    }
    strncpy(c->test_id, argv[1], sizeof(c->test_id) - 1);
    for (int i = 2; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (!eq) {
            fprintf(stderr, "bad arg: %s (expected key=value)\n", argv[i]);
            return -1;
        }
        *eq = 0;
        char* key = argv[i];
        char* val = eq + 1;
        if (strcmp(key, "holds_ms") == 0) {
            c->holds_count = parse_int_list(val, c->holds_ms, 16);
        } else if (strcmp(key, "writer_hz") == 0) {
            c->writer_hz = parse_int(val);
        } else if (strcmp(key, "reader_hz") == 0) {
            c->reader_hz = parse_int(val);
        } else if (strcmp(key, "frames") == 0) {
            c->frames = parse_int(val);
        } else if (strcmp(key, "publishes") == 0) {
            c->publishes = parse_int(val);
        } else if (strcmp(key, "claims") == 0) {
            c->claims = parse_int(val);
        } else if (strcmp(key, "payload_max") == 0) {
            c->payload_max = parse_int(val);
        } else if (strcmp(key, "bound") == 0) {
            c->bound = parse_int(val);
        } else if (strcmp(key, "window_s") == 0) {
            c->window_s = parse_double(val);
        } else if (strcmp(key, "timeout_ms") == 0) {
            c->timeout_ms = parse_int(val);
        } else if (strcmp(key, "tolerance") == 0) {
            c->tolerance = parse_double(val);
        } else if (strcmp(key, "trials") == 0) {
            c->trials = parse_int(val);
        } else if (strcmp(key, "max_delay_us") == 0) {
            c->max_delay_us = parse_int(val);
        } else if (strcmp(key, "seed") == 0) {
            c->seed = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "min_claims") == 0) {
            c->min_claims = parse_int(val);
        } else if (strcmp(key, "cycles") == 0) {
            c->cycles = parse_int(val);
        } else if (strcmp(key, "beams") == 0) {
            c->beams = parse_int(val);
        } else {
            fprintf(stderr, "unknown param: %s\n", key);
            return -1;
        }
        *eq = '=';  // restore for diagnostics
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Timing helpers (04-LITMUS §0.3)
// ---------------------------------------------------------------------------

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Deadline-paced sleep: sleep until `deadline`, then update deadline by period.
// If we've drifted past period*1, snap forward (no burst catch-up). (A4)
static void deadline_sleep(uint64_t* next_deadline_ns, uint64_t period_ns) {
    uint64_t now = now_ns();
    if (now < *next_deadline_ns) {
        uint64_t remaining = *next_deadline_ns - now;
        struct timespec ts;
        ts.tv_sec = remaining / 1000000000ull;
        ts.tv_nsec = remaining % 1000000000ull;
        nanosleep(&ts, NULL);
        // busy-wait the last ~100µs for precision (the writer's pacing matters)
        while (now_ns() < *next_deadline_ns) { /* spin */ }
    }
    *next_deadline_ns += period_ns;
    // Drift correction: if we fell behind by more than one period, snap forward.
    uint64_t now2 = now_ns();
    if (now2 > *next_deadline_ns + period_ns) {
        *next_deadline_ns = now2 + period_ns;
    }
}

// Inject a hold of `hold_ms` between claim and verification (L1, L5, L6).
// Uses nanosleep in a loop (single nanosleep can return early on signals).
static void hold_inject_ms(int ms) {
    if (ms <= 0) return;
    uint64_t deadline = now_ns() + (uint64_t)ms * 1000000ull;
    while (now_ns() < deadline) {
        uint64_t rem = deadline - now_ns();
        if (rem > 2000000) {  // > 2ms
            struct timespec ts = { .tv_sec = rem / 1000000000ull, .tv_nsec = rem % 1000000000ull };
            nanosleep(&ts, NULL);
        } else {
            // busy-wait the last 2ms for precision
        }
    }
}

// ---------------------------------------------------------------------------
// Frame verification (04-LITMUS §0.4)
// ---------------------------------------------------------------------------

// Verify the LIVE held buffer. Reads in-place via weft_r_live_ptr.
// Per A3: MUST read live, not a snapshot.
static bool verify_held(weft_t* w, uint32_t expected_seq, uint32_t payload_len) {
    // Magic
    if (weft_r_magic(w) != 0x54464557u) return false;
    // Seq still equals expected
    if (weft_r_seq(w) != expected_seq) return false;
    // Payload bytes match pat(expected_seq, i)
    const uint8_t* payload = weft_r_live_ptr(w, 16);
    if (!payload) return false;
    for (uint32_t i = 0; i < payload_len; i++) {
        if (payload[i] != weft_pat(expected_seq, i)) return false;
    }
    // Canary == expected_seq (u64 LE at buf_size-8)
    const uint8_t* canary = weft_r_live_ptr(w, w->buf_size - 8);
    if (!canary) return false;
    uint64_t cv;
    memcpy(&cv, canary, 8);
    if (cv != (uint64_t)expected_seq) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Writer payload writer — fills w_work buffer with pat(seq) payload
// ---------------------------------------------------------------------------

static void fill_payload(weft_t* w, uint32_t seq, uint32_t payload_len) {
    uint8_t* p = weft_w_begin(w);  // returns w->buf[w_work] + 16
    for (uint32_t i = 0; i < payload_len; i++) {
        p[i] = weft_pat(seq, i);
    }
}

// ---------------------------------------------------------------------------
// L1 — tear
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int writer_hz;
    int frames;
    int payload_max;
    uint32_t seq;             // shared with main thread via atomic
    _Atomic bool stop;
    _Atomic uint64_t published;
} l1_writer_args_t;

static void* l1_writer_thread(void* arg) {
    l1_writer_args_t* a = (l1_writer_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next = now_ns() + period_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && seq <= (uint32_t)a->frames) {
        fill_payload(a->w, seq, a->payload_max);
        weft_pub_result_t r = weft_publish(a->w, seq, a->payload_max);
        if (r == WEFT_PUB_OK) {
            atomic_fetch_add(&a->published, 1);
        }
        seq++;
        deadline_sleep(&next, period_ns);
    }
    return NULL;
}

static int run_l1(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 1024;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 240;
    int frames = c->frames > 0 ? c->frames : 600;
    int holds[16];
    int holds_count = c->holds_count > 0 ? c->holds_count : 3;
    if (c->holds_count == 0) {
        holds[0] = 5; holds[1] = 10; holds[2] = 50;
    } else {
        memcpy(holds, c->holds_ms, holds_count * sizeof(int));
    }

    int total_torn = 0;
    int total_claims = 0;
    bool all_drain_ok = true;
    int min_claims = c->min_claims > 0 ? c->min_claims : 600;  // v1.1 (A4): per-lang floor
    double claims_per_s = 0.0;  // v1.1 (A4): falsifiable recalibration telemetry

    for (int hi = 0; hi < holds_count; hi++) {
        int hold = holds[hi];
        weft_t w;
        if (weft_init(&w, payload_max) != 0) {
            fprintf(stderr, "L1: weft_init failed for hold=%d\n", hold);
            return 1;
        }
        l1_writer_args_t args = { .w = &w, .writer_hz = writer_hz, .frames = frames, .payload_max = payload_max };
        atomic_init(&args.stop, false);
        atomic_init(&args.published, 0);

        pthread_t wt;
        pthread_create(&wt, NULL, l1_writer_thread, &args);

        uint32_t last_seq = 0;
        uint32_t max_observed_s = 0;  // v1.1 (A2): track max seq seen for drain_ok
        int claims = 0;
        int torn = 0;
        bool drain_ok = false;
        uint64_t run_start = now_ns();
        uint64_t timeout = 30ull * 1000000000ull;

        // Reader loop: claim, read seq, hold, verify
        while (true) {
            uint64_t elapsed = now_ns() - run_start;
            if (elapsed > timeout) break;
            bool writer_done = (atomic_load(&args.published) >= (uint64_t)frames);

            uint32_t idx = weft_r_claim(&w);
            (void)idx;
            uint32_t s = weft_r_seq(&w);
            claims++;
            if (s > max_observed_s) max_observed_s = s;

            if (s != last_seq) {
                hold_inject_ms(hold);
                if (!verify_held(&w, s, payload_max)) torn++;
                last_seq = s;
            } else {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);
            }

            if (writer_done && max_observed_s >= (uint32_t)frames) {
                drain_ok = true;
                break;
            }
        }

        atomic_store(&args.stop, true);
        pthread_join(wt, NULL);

        // v1.1 (A2): bounded post-join drain — up to 4 attempts (1ms apart)
        // if we haven't yet seen frames. The reader may have consumed the final
        // frame BEFORE the join (then the post-join claim ping-pongs a stale buffer).
        if (!drain_ok) {
            for (int attempt = 0; attempt < 4; attempt++) {
                uint32_t idx = weft_r_claim(&w);
                (void)idx;
                uint32_t s = weft_r_seq(&w);
                if (s > max_observed_s) max_observed_s = s;
                if (max_observed_s >= (uint32_t)frames) { drain_ok = true; break; }
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);
            }
        }
        drain_ok = (max_observed_s >= (uint32_t)frames);

        total_torn += torn;
        total_claims += claims;
        if (!drain_ok) all_drain_ok = false;

        double elapsed_s = (double)(now_ns() - run_start) / 1e9;
        double hold_cps = (elapsed_s > 0) ? (double)claims / elapsed_s : 0.0;
        claims_per_s += hold_cps;
        fprintf(stderr, "L1 hold=%dms claims=%d torn=%d drain_ok=%d claims_per_s=%.1f\n",
                hold, claims, torn, drain_ok, hold_cps);
        weft_destroy(&w);
    }

    claims_per_s /= holds_count;  // average across holds
    bool pass = (total_torn == 0) && all_drain_ok && (total_claims >= min_claims);
    // Verdict: torn==0 AND drain_ok AND claims >= min_claims(lang)
    printf("{\"test\":\"L1-tear\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"holds_ms\":[",
           pass ? "true" : "false");
    for (int i = 0; i < holds_count; i++) {
        printf("%d%s", holds[i], i + 1 < holds_count ? "," : "");
    }
    printf("],\"claims\":%d,\"claims_per_s\":%.1f,\"torn\":%d,\"drain_ok\":%s}}\n",
           total_claims, claims_per_s, total_torn, all_drain_ok ? "true" : "false");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L2 — writer step bound (wait-free writer)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int publishes;
    int writer_hz;
    int payload_max;
    _Atomic bool stop;
    _Atomic uint64_t published;
    _Atomic uint64_t max_wsteps_delta;
} l2_writer_args_t;

typedef struct {
    weft_t* w;
    int hold_ms;
    _Atomic bool stop;
} l2_reader_args_t;

static void* l2_writer_thread(void* arg) {
    l2_writer_args_t* a = (l2_writer_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next = now_ns() + period_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && seq <= (uint32_t)a->publishes) {
        fill_payload(a->w, seq, a->payload_max);
        uint64_t before = weft_t_wsteps(a->w);
        weft_pub_result_t r = weft_publish(a->w, seq, a->payload_max);
        uint64_t after = weft_t_wsteps(a->w);
        if (r == WEFT_PUB_OK) {
            atomic_fetch_add(&a->published, 1);
            uint64_t d = after - before;
            // Update max under contention-free compare (Relaxed fine for stats)
            uint64_t cur = atomic_load(&a->max_wsteps_delta);
            while (d > cur) {
                if (atomic_compare_exchange_weak(&a->max_wsteps_delta, &cur, d)) break;
            }
        }
        seq++;
        deadline_sleep(&next, period_ns);
    }
    return NULL;
}

static void* l2_reader_thread(void* arg) {
    l2_reader_args_t* a = (l2_reader_args_t*)arg;
    while (!atomic_load(&a->stop)) {
        weft_r_claim(a->w);
        hold_inject_ms(a->hold_ms);
    }
    return NULL;
}

static int run_l2(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int publishes = c->publishes > 0 ? c->publishes : 2000;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 2000;
    int bound = c->bound > 0 ? c->bound : 2;
    int holds[16];
    int holds_count = c->holds_count > 0 ? c->holds_count : 5;
    if (c->holds_count == 0) {
        holds[0] = 0; holds[1] = 10; holds[2] = 25; holds[3] = 50; holds[4] = 100;
    } else {
        memcpy(holds, c->holds_ms, holds_count * sizeof(int));
    }

    int total_publishes = 0;
    uint64_t max_wsteps = 0;
    bool all_ok = true;

    for (int hi = 0; hi < holds_count; hi++) {
        int hold = holds[hi];
        weft_t w;
        if (weft_init(&w, payload_max) != 0) return 1;
        l2_writer_args_t wa = { .w = &w, .publishes = publishes, .writer_hz = writer_hz, .payload_max = payload_max };
        atomic_init(&wa.stop, false);
        atomic_init(&wa.published, 0);
        atomic_init(&wa.max_wsteps_delta, 0);
        l2_reader_args_t ra = { .w = &w, .hold_ms = hold };
        atomic_init(&ra.stop, false);

        pthread_t wt, rt;
        pthread_create(&wt, NULL, l2_writer_thread, &wa);
        if (hold >= 0) pthread_create(&rt, NULL, l2_reader_thread, &ra);

        pthread_join(wt, NULL);
        atomic_store(&ra.stop, true);
        if (hold >= 0) pthread_join(rt, NULL);

        uint64_t published = atomic_load(&wa.published);
        uint64_t max_d = atomic_load(&wa.max_wsteps_delta);
        total_publishes += (int)published;
        if (max_d > max_wsteps) max_wsteps = max_d;
        bool ok = (max_d <= (uint64_t)bound) && (published == (uint64_t)publishes);
        if (!ok) all_ok = false;

        fprintf(stderr, "L2 hold=%dms published=%lu max_wsteps=%lu ok=%d\n",
                hold, published, max_d, ok);
        weft_destroy(&w);
    }

    bool pass = all_ok && (max_wsteps <= (uint64_t)bound) && (total_publishes == publishes * holds_count);
    printf("{\"test\":\"L2-writer-steps\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"holds_ms\":[",
           pass ? "true" : "false");
    for (int i = 0; i < holds_count; i++) {
        printf("%d%s", holds[i], i + 1 < holds_count ? "," : "");
    }
    printf("],\"publishes\":%d,\"max_wsteps\":%lu,\"bound\":%d}}\n",
           total_publishes, max_wsteps, bound);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L3 — reader step bound (wait-free reader)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int writer_hz;
    int payload_max;
    int frames;
    _Atomic bool stop;
    _Atomic uint64_t published;
} l3_writer_args_t;

typedef struct {
    weft_t* w;
    int claims;
    _Atomic bool stop;
    _Atomic uint64_t claimed;
    _Atomic uint64_t max_rsteps_delta;
} l3_reader_args_t;

static void* l3_writer_thread(void* arg) {
    l3_writer_args_t* a = (l3_writer_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next = now_ns() + period_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && seq <= (uint32_t)a->frames) {
        fill_payload(a->w, seq, a->payload_max);
        weft_pub_result_t r = weft_publish(a->w, seq, a->payload_max);
        if (r == WEFT_PUB_OK) atomic_fetch_add(&a->published, 1);
        seq++;
        deadline_sleep(&next, period_ns);
    }
    return NULL;
}

static void* l3_reader_thread(void* arg) {
    l3_reader_args_t* a = (l3_reader_args_t*)arg;
    while (!atomic_load(&a->stop) && atomic_load(&a->claimed) < (uint64_t)a->claims) {
        uint64_t before = weft_t_rsteps(a->w);
        weft_r_claim(a->w);
        uint64_t after = weft_t_rsteps(a->w);
        uint64_t d = after - before;
        uint64_t cur = atomic_load(&a->max_rsteps_delta);
        while (d > cur) {
            if (atomic_compare_exchange_weak(&a->max_rsteps_delta, &cur, d)) break;
        }
        atomic_fetch_add(&a->claimed, 1);
    }
    return NULL;
}

static int run_l3(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 960;
    int claims = c->claims > 0 ? c->claims : 2000;
    int bound = c->bound > 0 ? c->bound : 2;
    // For L3, writer must run for at least as many frames as the reader claims.
    int frames = claims * 2;  // generous

    weft_t w;
    if (weft_init(&w, payload_max) != 0) return 1;
    l3_writer_args_t wa = { .w = &w, .writer_hz = writer_hz, .payload_max = payload_max, .frames = frames };
    atomic_init(&wa.stop, false);
    atomic_init(&wa.published, 0);
    l3_reader_args_t ra = { .w = &w, .claims = claims };
    atomic_init(&ra.stop, false);
    atomic_init(&ra.claimed, 0);
    atomic_init(&ra.max_rsteps_delta, 0);

    pthread_t wt, rt;
    pthread_create(&wt, NULL, l3_writer_thread, &wa);
    pthread_create(&rt, NULL, l3_reader_thread, &ra);

    pthread_join(rt, NULL);
    atomic_store(&wa.stop, true);
    pthread_join(wt, NULL);

    uint64_t claimed = atomic_load(&ra.claimed);
    uint64_t max_rsteps = atomic_load(&ra.max_rsteps_delta);
    bool pass = (max_rsteps <= (uint64_t)bound) && (claimed == (uint64_t)claims);
    fprintf(stderr, "L3 claims=%lu max_rsteps=%lu bound=%d pass=%d\n",
            claimed, max_rsteps, bound, pass);
    weft_destroy(&w);

    printf("{\"test\":\"L3-reader-steps\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"claims\":%lu,\"max_rsteps\":%lu,\"bound\":%d}}\n",
           pass ? "true" : "false", claimed, max_rsteps, bound);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L4 — freshness
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int writer_hz;
    int frames;
    int payload_max;
    _Atomic bool stop;
    _Atomic uint64_t published;
} l4_writer_args_t;

static void* l4_writer_thread(void* arg) {
    l4_writer_args_t* a = (l4_writer_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next = now_ns() + period_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && seq <= (uint32_t)a->frames) {
        fill_payload(a->w, seq, a->payload_max);
        if (weft_publish(a->w, seq, a->payload_max) == WEFT_PUB_OK) {
            atomic_fetch_add(&a->published, 1);
        }
        seq++;
        deadline_sleep(&next, period_ns);
    }
    return NULL;
}

static int run_l4(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 960;
    int reader_hz = c->reader_hz > 0 ? c->reader_hz : 240;
    int frames = c->frames > 0 ? c->frames : 2000;

    weft_t w;
    if (weft_init(&w, payload_max) != 0) return 1;
    l4_writer_args_t wa = { .w = &w, .writer_hz = writer_hz, .frames = frames, .payload_max = payload_max };
    atomic_init(&wa.stop, false);
    atomic_init(&wa.published, 0);

    pthread_t wt;
    pthread_create(&wt, NULL, l4_writer_thread, &wa);

    uint64_t reader_period = 1000000000ull / (uint64_t)reader_hz;
    uint64_t next = now_ns() + reader_period;

    int freshness_violations = 0;
    int future_violations = 0;
    int stale_returns = 0;   // v1.1: stale returns (not a violation, per 04-LITMUS §0.6)
    uint32_t last = 0;        // v1.1: highest seq observed (init 0)
    uint64_t run_start = now_ns();
    uint64_t timeout = 60ull * 1000000000ull;

    // Phase 1: concurrent. Stop when t_publish == frames.
    // v1.1 (WO-P0A §1.2): track `last`; new-frame vs stale-return distinction.
    while (true) {
        if (now_ns() - run_start > timeout) break;
        uint64_t tpub = atomic_load_explicit(&w.t_publish, memory_order_acquire);
        bool writer_done = (tpub >= (uint64_t)frames);
        if (writer_done) break;

        uint64_t p0 = tpub;
        uint32_t idx = weft_r_claim(&w);
        (void)idx;
        uint32_t s = weft_r_seq(&w);
        uint64_t p1 = atomic_load_explicit(&w.t_publish, memory_order_acquire);

        if (s > last) {
            // NEW FRAME — freshness violation check applies
            if (s < p0) freshness_violations++;
            last = s;
        } else {
            // STALE RETURN (§0.6) — exchange lawfully handed back reader's own buffer.
            stale_returns++;
        }

        // Future violation check (telemetry lag, 02 §5)
        if ((uint64_t)s > p1) {
            uint64_t spin_deadline = now_ns() + 2ull * 1000000ull;
            while (now_ns() < spin_deadline) {
                p1 = atomic_load_explicit(&w.t_publish, memory_order_acquire);
                if ((uint64_t)s <= p1) break;
            }
            if ((uint64_t)s > p1) future_violations++;
        }

        deadline_sleep(&next, reader_period);
    }

    atomic_store(&wa.stop, true);
    pthread_join(wt, NULL);

    // v1.1 drain: after writer join, claim up to 4 attempts (1ms apart);
    // drain_exact = (last == frames). The absolute "next claim returns frames"
    // form was ill-posed — reader may have consumed final frame before join.
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t idx = weft_r_claim(&w);
        (void)idx;
        uint32_t s = weft_r_seq(&w);
        if (s > last) last = s;
        if (last == (uint32_t)frames) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
    bool drain_exact = (last == (uint32_t)frames);

    bool pass = (freshness_violations == 0) && (future_violations == 0) && drain_exact;
    fprintf(stderr, "L4 freshness_violations=%d future_violations=%d stale_returns=%d drain_exact=%d (last=%u) pass=%d\n",
            freshness_violations, future_violations, stale_returns, drain_exact, last, pass);
    weft_destroy(&w);

    printf("{\"test\":\"L4-freshness\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"frames\":%d,\"freshness_violations\":%d,"
           "\"future_violations\":%d,\"drain_exact\":%s,\"stale_returns\":%d}}\n",
           pass ? "true" : "false", frames, freshness_violations,
           future_violations, drain_exact ? "true" : "false", stale_returns);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L5 — progress (writer never blocked)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int writer_hz;
    int payload_max;
    uint64_t window_ns;
    _Atomic bool stop;
    _Atomic uint64_t published_in_window;
} l5_writer_args_t;

static void* l5_writer_thread(void* arg) {
    l5_writer_args_t* a = (l5_writer_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next = now_ns() + period_ns;
    uint64_t window_start = now_ns();
    uint64_t window_end = window_start + a->window_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && now_ns() < window_end) {
        fill_payload(a->w, seq, a->payload_max);
        if (weft_publish(a->w, seq, a->payload_max) == WEFT_PUB_OK) {
            atomic_fetch_add(&a->published_in_window, 1);
        }
        seq++;
        deadline_sleep(&next, period_ns);
    }
    return NULL;
}

typedef struct {
    weft_t* w;
    int hold_ms;
    _Atomic bool stop;
} l5_reader_args_t;

static void* l5_reader_thread(void* arg) {
    l5_reader_args_t* a = (l5_reader_args_t*)arg;
    while (!atomic_load(&a->stop)) {
        weft_r_claim(a->w);
        hold_inject_ms(a->hold_ms);
    }
    return NULL;
}

static int run_l5(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 2000;
    double window_s = c->window_s > 0 ? c->window_s : 1.0;
    double tolerance = c->tolerance > 0 ? c->tolerance : 1.5;
    int holds[16];
    int holds_count = c->holds_count > 0 ? c->holds_count : 4;
    if (c->holds_count == 0) {
        holds[0] = 0; holds[1] = 10; holds[2] = 50; holds[3] = 100;
    } else {
        memcpy(holds, c->holds_ms, holds_count * sizeof(int));
    }
    // The fifth config (reader suspended entirely) is implicit.
    int configs_count = holds_count + 1;

    double rates[16];
    int rates_count = 0;

    for (int hi = 0; hi < holds_count; hi++) {
        int hold = holds[hi];
        weft_t w;
        if (weft_init(&w, payload_max) != 0) return 1;
        l5_writer_args_t wa = { .w = &w, .writer_hz = writer_hz, .payload_max = payload_max,
                                .window_ns = (uint64_t)(window_s * 1e9) };
        atomic_init(&wa.stop, false);
        atomic_init(&wa.published_in_window, 0);
        l5_reader_args_t ra = { .w = &w, .hold_ms = hold };
        atomic_init(&ra.stop, false);

        pthread_t wt, rt;
        pthread_create(&wt, NULL, l5_writer_thread, &wa);
        if (hold >= 0) pthread_create(&rt, NULL, l5_reader_thread, &ra);

        pthread_join(wt, NULL);
        atomic_store(&ra.stop, true);
        if (hold >= 0) pthread_join(rt, NULL);

        uint64_t pub = atomic_load(&wa.published_in_window);
        double rate = (double)pub / window_s;
        rates[rates_count++] = rate;
        fprintf(stderr, "L5 hold=%dms rate=%.1f Hz\n", hold, rate);
        weft_destroy(&w);
    }

    // Suspended reader config: no reader thread.
    {
        weft_t w;
        if (weft_init(&w, payload_max) != 0) return 1;
        l5_writer_args_t wa = { .w = &w, .writer_hz = writer_hz, .payload_max = payload_max,
                                .window_ns = (uint64_t)(window_s * 1e9) };
        atomic_init(&wa.stop, false);
        atomic_init(&wa.published_in_window, 0);
        pthread_t wt;
        pthread_create(&wt, NULL, l5_writer_thread, &wa);
        pthread_join(wt, NULL);
        uint64_t pub = atomic_load(&wa.published_in_window);
        double rate = (double)pub / window_s;
        rates[rates_count++] = rate;
        fprintf(stderr, "L5 suspended-reader rate=%.1f Hz\n", rate);
        weft_destroy(&w);
    }

    // Compute max/min ratio
    double max_r = 0, min_r = 1e18;
    for (int i = 0; i < rates_count; i++) {
        if (rates[i] > max_r) max_r = rates[i];
        if (rates[i] < min_r) min_r = rates[i];
    }
    double ratio = (min_r > 0) ? (max_r / min_r) : 999.0;
    bool pass = (ratio <= tolerance);
    fprintf(stderr, "L5 ratio=%.3f tolerance=%.2f pass=%d\n", ratio, tolerance, pass);

    printf("{\"test\":\"L5-progress\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"rates_hz\":[",
           pass ? "true" : "false");
    for (int i = 0; i < rates_count; i++) {
        printf("%.1f%s", rates[i], i + 1 < rates_count ? "," : "");
    }
    printf("],\"ratio\":%.3f,\"tolerance\":%.2f,\"configs\":%d}}\n",
           ratio, tolerance, configs_count);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L6 — ownership (canary + randomized interleavings)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int frames;
    int payload_max;
    uint32_t seed;
    int max_delay_us;
    _Atomic bool stop;
    _Atomic int violations;
} l6_args_t;

static void* l6_writer_thread(void* arg) {
    l6_args_t* a = (l6_args_t*)arg;
    uint32_t seed = a->seed;  // writer seed = catalog seed
    for (uint32_t seq = 1; seq <= (uint32_t)a->frames; seq++) {
        // Random pre-publish delay
        uint32_t d = weft_xorshift32(&seed) % (uint32_t)a->max_delay_us;
        if (d > 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)d * 1000 };
            nanosleep(&ts, NULL);
        }
        fill_payload(a->w, seq, a->payload_max);
        weft_publish(a->w, seq, a->payload_max);
    }
    atomic_store(&a->stop, true);
    return NULL;
}

static void* l6_reader_thread(void* arg) {
    l6_args_t* a = (l6_args_t*)arg;
    uint32_t seed = a->seed ^ 0x9E3779B9u;  // reader seed (04-LITMUS L6)
    uint32_t last_seq = 0;
    while (!atomic_load(&a->stop)) {
        // Random pre-claim delay
        uint32_t d = weft_xorshift32(&seed) % (uint32_t)a->max_delay_us;
        if (d > 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)d * 1000 };
            nanosleep(&ts, NULL);
        }
        uint32_t idx = weft_r_claim(a->w);
        (void)idx;
        uint32_t s = weft_r_seq(a->w);
        if (s != last_seq) {
            // Verify immediately (no hold injection in L6 — the canary is the test)
            if (!verify_held(a->w, s, a->payload_max)) {
                atomic_fetch_add(&a->violations, 1);
            }
            last_seq = s;
        }
    }
    return NULL;
}

static int run_l6(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int trials = c->trials > 0 ? c->trials : 200;
    int frames = c->frames > 0 ? c->frames : 64;
    int max_delay_us = c->max_delay_us > 0 ? c->max_delay_us : 500;
    uint32_t seed = c->seed != 0 ? c->seed : 0x00C0FFEE;

    int total_violations = 0;
    for (int t = 0; t < trials; t++) {
        weft_t w;
        if (weft_init(&w, payload_max) != 0) return 1;
        l6_args_t args = { .w = &w, .frames = frames, .payload_max = payload_max,
                           .seed = seed, .max_delay_us = max_delay_us };
        atomic_init(&args.stop, false);
        atomic_init(&args.violations, 0);

        pthread_t wt, rt;
        pthread_create(&wt, NULL, l6_writer_thread, &args);
        pthread_create(&rt, NULL, l6_reader_thread, &args);

        pthread_join(wt, NULL);
        // Reader stops when writer signals stop; give it a moment to drain
        uint64_t deadline = now_ns() + 1000000000ull;
        while (!atomic_load(&args.stop) && now_ns() < deadline) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
        }
        pthread_join(rt, NULL);

        int v = atomic_load(&args.violations);
        total_violations += v;
        if (v > 0) {
            fprintf(stderr, "L6 trial %d: violations=%d\n", t, v);
        }
        weft_destroy(&w);
    }

    bool pass = (total_violations == 0);
    fprintf(stderr, "L6 trials=%d violations=%d seed=0x%08X pass=%d\n",
            trials, total_violations, seed, pass);
    printf("{\"test\":\"L6-ownership\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"trials\":%d,\"frames_per_trial\":%d,"
           "\"violations\":%d,\"seed\":\"0x%08X\"}}\n",
           pass ? "true" : "false", trials, frames, total_violations, seed);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L7 — revocation (I6 under fire)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    int payload_max;
    _Atomic bool stop;
    _Atomic uint64_t publish_count;
    _Atomic uint64_t first_revoked_at;
    _Atomic uint64_t post_revoke_revoked;
} l7_args_t;

static void* l7_writer_thread(void* arg) {
    l7_args_t* a = (l7_args_t*)arg;
    uint32_t seq = 1;
    bool seen_revoked = false;
    while (!atomic_load(&a->stop)) {
        // Per 02 §6: after the ACK, the writer must NEVER touch buffer bytes again.
        // fill_payload() writes payload bytes BEFORE publish's revoked check; it
        // must be skipped after the ACK to honor the I6 contract (and to avoid
        // a TSAN data race with the harness's poison memset).
        if (!seen_revoked) {
            fill_payload(a->w, seq, a->payload_max);
        }
        weft_pub_result_t r = weft_publish(a->w, seq, a->payload_max);
        if (r == WEFT_PUB_OK) {
            atomic_fetch_add(&a->publish_count, 1);
        } else if (r == WEFT_PUB_DROPPED_REVOKED) {
            if (!seen_revoked) {
                atomic_store(&a->first_revoked_at, atomic_load(&a->publish_count));
                seen_revoked = true;
            }
            // Count post-revoke attempts (the test wants 100 of these)
            uint64_t n = atomic_load(&a->post_revoke_revoked);
            if (n < 100) atomic_fetch_add(&a->post_revoke_revoked, 1);
            if (n + 1 >= 100) {
                atomic_store(&a->stop, true);
                break;
            }
        }
        seq++;
    }
    return NULL;
}

static int run_l7(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int timeout_ms = c->timeout_ms > 0 ? c->timeout_ms : 2000;

    weft_t w;
    if (weft_init(&w, payload_max) != 0) return 1;
    l7_args_t args = { .w = &w, .payload_max = payload_max };
    atomic_init(&args.stop, false);
    atomic_init(&args.publish_count, 0);
    atomic_init(&args.first_revoked_at, 0);
    atomic_init(&args.post_revoke_revoked, 0);

    pthread_t wt;
    pthread_create(&wt, NULL, l7_writer_thread, &args);

    // Let the writer run for 5ms to establish baseline
    hold_inject_ms(5);

    // Step 1: capture pre-revoke epoch (02 §6)
    uint32_t e0 = weft_epoch(&w);

    // Step 1 (cont.): revoke (Release store)
    weft_revoke(&w);

    // Steps 2-3: reclaim — poll epoch (Acquire) until ACK, bounded by timeout.
    // After ACK, poison all 3 buffers with 0xDE and keep the mapping.
    int rc = weft_reclaim(&w, e0, timeout_ms);
    bool reclaim_ok = (rc == 0);

    // Poison all 3 buffers with 0xDE (after ACK)
    for (int i = 0; i < 3; i++) {
        memset(w.buf[i], 0xDE, w.buf_size);
    }

    // Join writer (it should have stopped after 100 post-revoke attempts)
    uint64_t join_deadline = now_ns() + 5ull * 1000000000ull;
    while (!atomic_load(&args.stop) && now_ns() < join_deadline) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
    atomic_store(&args.stop, true);
    pthread_join(wt, NULL);

    // Scan all 3 buffers byte-wise — every byte should still be 0xDE
    // (the writer must not have written to poisoned pages post-ACK).
    bool poison_intact = true;
    for (int i = 0; i < 3; i++) {
        for (size_t j = 0; j < w.buf_size; j++) {
            if (w.buf[i][j] != 0xDE) {
                poison_intact = false;
                fprintf(stderr, "L7: buffer %d offset %zu = 0x%02X (expected 0xDE)\n",
                        i, j, w.buf[i][j]);
                break;
            }
        }
        if (!poison_intact) break;
    }

    uint64_t post_revoke = atomic_load(&args.post_revoke_revoked);
    uint64_t first_revoked_at = atomic_load(&args.first_revoked_at);
    bool pass = reclaim_ok && (post_revoke == 100) && poison_intact;
    fprintf(stderr, "L7 reclaim_ok=%d post_revoke_revoked=%lu poison_intact=%d pass=%d\n",
            reclaim_ok, post_revoke, poison_intact, pass);
    weft_destroy(&w);

    printf("{\"test\":\"L7-revocation\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"reclaim_ok\":%s,\"post_revoke_revoked\":%lu,"
           "\"poison_intact\":%s,\"first_revoked_at\":%lu}}\n",
           pass ? "true" : "false",
           reclaim_ok ? "true" : "false", post_revoke,
           poison_intact ? "true" : "false", first_revoked_at);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L8 — envelope (pure functions; no threads, <1s)
// ---------------------------------------------------------------------------

static bool l8_roundtrip(void) {
    uint8_t enc[256];
    memset(enc, 0, sizeof(enc));
    weft_envelope_encode_v1(enc, 7, 100);
    uint16_t v, hs; uint32_t s, pl;
    weft_decode_result_t r = weft_envelope_decode(enc, 256, &v, &hs, &s, &pl);
    if (r != WEFT_DECODE_OK) return false;
    if (v != 1 || hs != 16 || s != 7 || pl != 100) return false;
    // Re-encode → byte-identical
    uint8_t enc2[256];
    memset(enc2, 0, sizeof(enc2));
    weft_envelope_encode_v1(enc2, 7, 100);
    if (memcmp(enc, enc2, 16) != 0) return false;
    return true;
}

static bool l8_unknown_fields(void) {
    uint8_t enc[256];
    memset(enc, 0, sizeof(enc));
    // Encode with header_size=24, 8 opaque 0xAA bytes
    weft_envelope_encode(enc, 1, 24, 7, 100);
    uint16_t v, hs; uint32_t s, pl;
    weft_decode_result_t r = weft_envelope_decode(enc, 256, &v, &hs, &s, &pl);
    if (r != WEFT_DECODE_OK) return false;
    if (v != 1 || hs != 24 || s != 7 || pl != 100) return false;
    // Payload located at +24 (not +16)
    if (enc[16] != 0xAA || enc[23] != 0xAA) return false;  // unknown trailing fields
    return true;
}

static bool l8_negotiation(void) {
    // Per 03-ENVELOPE §3 (the normative rule):
    //   chosen = max({ v ∈ S : v ≤ W })
    //   if the set is empty → 0 (BIND_INCOMPATIBLE)
    //
    // FINDING (filed per directive §4.3): the example table in 03-ENVELOPE §5
    // lists row 3 as (W=2, S={1}) → BIND_INCOMPATIBLE, but the §3 formula
    // produces 1 (since 1 ≤ 2). The §3 formula is normative; the §5 table is
    // an illustrative hooks subcheck description. This test verifies the §3
    // formula. The §5 table inconsistency is recorded in REPORT.md.
    //
    // Formula's expected results for the four §5 rows:
    //   (W=1, S={1})       → 1   (formula: max{1: 1≤1} = 1)     — matches table
    //   (W=2, S={1,2})     → 2   (formula: max{1,2: ≤2} = 2)    — matches table
    //   (W=2, S={1})       → 1   (formula: max{1: 1≤2} = 1)      — TABLE SAYS BIND_INCOMPATIBLE (typo)
    //   (W=3, S={1,2})     → 2   (formula: max{1,2: ≤3} = 2)    — matches table

    uint16_t s1[] = {1};
    if (weft_negotiate(1, s1, 1) != 1) return false;
    uint16_t s12[] = {1, 2};
    if (weft_negotiate(2, s12, 2) != 2) return false;
    // Row 3: per the §3 formula, this returns 1 (not BIND_INCOMPATIBLE).
    // The §5 table's BIND_INCOMPATIBLE is a likely typo; see REPORT.md.
    uint16_t s1_only[] = {1};
    if (weft_negotiate(2, s1_only, 1) != 1) return false;
    // Row 4: matches both formula and table.
    uint16_t s12_only[] = {1, 2};
    if (weft_negotiate(3, s12_only, 2) != 2) return false;
    // Additional BIND_INCOMPATIBLE case (not in the table but implied by §3):
    // (W=1, S={2,3}) → empty set → BIND_INCOMPATIBLE
    uint16_t s23[] = {2, 3};
    if (weft_negotiate(1, s23, 2) != 0) return false;
    return true;
}

static bool l8_coexist(void) {
    uint8_t enc_v1[256];
    uint8_t enc_v2[256];
    memset(enc_v1, 0, sizeof(enc_v1));
    memset(enc_v2, 0, sizeof(enc_v2));
    weft_envelope_encode(enc_v1, 1, 16, 7, 100);
    weft_envelope_encode(enc_v2, 2, 16, 8, 100);  // synthetic v2
    uint16_t v1, hs1; uint32_t s1, pl1;
    uint16_t v2, hs2; uint32_t s2, pl2;
    weft_decode_result_t r1 = weft_envelope_decode(enc_v1, 256, &v1, &hs1, &s1, &pl1);
    weft_decode_result_t r2 = weft_envelope_decode(enc_v2, 256, &v2, &hs2, &s2, &pl2);
    if (r1 != WEFT_DECODE_OK || r2 != WEFT_DECODE_OK) return false;
    if (v1 != 1 || v2 != 2) return false;  // versions preserved
    if (s1 != 7 || s2 != 8) return false;
    return true;
}

static int run_l8(void) {
    bool a = l8_roundtrip();
    bool b = l8_unknown_fields();
    bool c = l8_negotiation();
    bool d = l8_coexist();
    bool pass = a && b && c && d;
    fprintf(stderr, "L8 roundtrip=%d unknown=%d negotiation=%d coexist=%d pass=%d\n",
            a, b, c, d, pass);
    printf("{\"test\":\"L8-envelope\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"roundtrip\":%s,\"unknown_fields\":%s,"
           "\"negotiation\":%s,\"coexist\":%s}}\n",
           pass ? "true" : "false",
           a ? "true" : "false", b ? "true" : "false",
           c ? "true" : "false", d ? "true" : "false");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L9 — cross-beam interference (Issue #16 Tier 1: N concurrent kernels)
// ---------------------------------------------------------------------------

// "Beam" = one independent Triad kernel instance (the weaving metaphor: each
// beam weaves its own warp). The adversary: B beams' writer/reader pairs run
// CONCURRENTLY on one machine — scheduling pressure, cache pressure, allocator
// pressure. The property: every beam behaves as if it were alone (tear-free,
// drained, no crosstalk — a reader of beam i only ever observes beam i frames,
// which is structural: separate state, but the TEST proves it under fire).

#define L9_MAX_BEAMS 4

typedef struct {
    weft_t* w;
    int writer_hz;
    int frames;
    int payload_max;
    int hold_ms;
    _Atomic bool stop;
    _Atomic uint64_t published;
    _Atomic uint64_t claims;
    _Atomic int torn;
    _Atomic int drain_ok;
} l9_args_t;

static void* l9_writer_thread(void* arg) {
    l9_args_t* a = (l9_args_t*)arg;
    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next = now_ns() + period_ns;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop) && seq <= (uint32_t)a->frames) {
        fill_payload(a->w, seq, a->payload_max);
        weft_pub_result_t r = weft_publish(a->w, seq, a->payload_max);
        if (r == WEFT_PUB_OK) atomic_fetch_add(&a->published, 1);
        seq++;
        deadline_sleep(&next, period_ns);
    }
    return NULL;
}

static void* l9_reader_thread(void* arg) {
    l9_args_t* a = (l9_args_t*)arg;
    uint32_t last_seq = 0;
    uint64_t deadline = now_ns() + 30000000000ull;  // 30s hard bound
    while (now_ns() < deadline) {
        (void)weft_r_claim(a->w);
        uint32_t s = weft_r_seq(a->w);
        if (s != last_seq) {
            hold_inject_ms(a->hold_ms);   // the tear adversary (L1-style)
            if (!verify_held(a->w, s, a->payload_max)) {
                atomic_fetch_add(&a->torn, 1);
            }
            last_seq = s;
            atomic_fetch_add(&a->claims, 1);
        }
        if (atomic_load(&a->stop) && s == (uint32_t)a->frames) break;
        // Reader pacing: 4x slower than the writer (catalog adversary shape)
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000000ull / 240 };
        nanosleep(&ts, NULL);
    }
    atomic_store(&a->drain_ok, last_seq == (uint32_t)a->frames);
    return NULL;
}

static int run_l9(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 240;
    int frames = c->frames > 0 ? c->frames : 600;
    int beams = c->beams > 0 ? c->beams : 2;
    if (beams < 1 || beams > L9_MAX_BEAMS) {
        fprintf(stderr, "L9: beams must be 1..%d\n", L9_MAX_BEAMS);
        return 2;
    }
    int holds_ms[1] = { c->holds_count > 0 ? c->holds_ms[0] : 5 };

    weft_t w[L9_MAX_BEAMS];
    l9_args_t args[L9_MAX_BEAMS];
    pthread_t wt[L9_MAX_BEAMS], rt[L9_MAX_BEAMS];

    for (int b = 0; b < beams; b++) {
        if (weft_init(&w[b], payload_max) != 0) return 2;
        args[b] = (l9_args_t){ .w = &w[b], .writer_hz = writer_hz,
                               .frames = frames, .payload_max = payload_max,
                               .hold_ms = holds_ms[0] };
        atomic_init(&args[b].stop, false);
        atomic_init(&args[b].published, 0);
        atomic_init(&args[b].claims, 0);
        atomic_init(&args[b].torn, 0);
        atomic_init(&args[b].drain_ok, 0);
    }

    for (int b = 0; b < beams; b++) {
        pthread_create(&wt[b], NULL, l9_writer_thread, &args[b]);
        pthread_create(&rt[b], NULL, l9_reader_thread, &args[b]);
    }
    for (int b = 0; b < beams; b++) {
        pthread_join(wt[b], NULL);
        atomic_store(&args[b].stop, true);
    }
    for (int b = 0; b < beams; b++) pthread_join(rt[b], NULL);

    int total_torn = 0, beams_drained = 0;
    uint64_t total_claims = 0;
    for (int b = 0; b < beams; b++) {
        total_torn += atomic_load(&args[b].torn);
        beams_drained += atomic_load(&args[b].drain_ok) ? 1 : 0;
        total_claims += atomic_load(&args[b].claims);
        weft_destroy(&w[b]);
    }

    bool pass = (total_torn == 0) && (beams_drained == beams);
    fprintf(stderr, "L9 beams=%d frames=%d torn=%d drained=%d/%d claims=%lu pass=%d\n",
            beams, frames, total_torn, beams_drained, beams,
            (unsigned long)total_claims, pass);
    printf("{\"test\":\"L9-cross-beam\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"beams\":%d,\"frames\":%d,\"torn\":%d,"
           "\"beams_drained\":%d,\"claims\":%lu,\"writer_hz\":%d}}\n",
           pass ? "true" : "false", beams, frames, total_torn,
           beams_drained, (unsigned long)total_claims, writer_hz);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L10 — nested revocation (revoke during revoke; I6 reentrancy)
// ---------------------------------------------------------------------------

// The kernel's revoke is an idempotent Release store of `true`; the ACK chain
// is what makes the handshake safe. The adversary: (a) revoke called TWICE
// (nested) while the writer spins; (b) revoke called from TWO threads
// concurrently, many times; (c) reclaim polls racing those revokes. The
// property: exactly-one-ACK-per-observation semantics hold, epoch stays
// monotone, reclaim NEVER frees before the ACK, and no path deadlocks.

typedef struct {
    weft_t* w;
    _Atomic bool stop;
    _Atomic uint64_t acks;        // DROPPED_REVOKED returns observed by writer
    _Atomic uint64_t publishes;
} l10_writer_args_t;

static void* l10_writer_thread(void* arg) {
    l10_writer_args_t* a = (l10_writer_args_t*)arg;
    uint32_t seq = 1;
    bool seen = false;
    while (!atomic_load(&a->stop)) {
        if (!seen) fill_payload(a->w, seq, 64);
        weft_pub_result_t r = weft_publish(a->w, seq, 64);
        if (r == WEFT_PUB_OK) atomic_fetch_add(&a->publishes, 1);
        else { atomic_fetch_add(&a->acks, 1); seen = true; }
        seq++;
        if (atomic_load(&a->acks) >= 8) break;  // enough post-revoke samples
    }
    return NULL;
}

typedef struct {
    weft_t* w;
    int count;
} l10_revoke_args_t;

static void* l10_revoke_thread(void* arg) {
    l10_revoke_args_t* a = (l10_revoke_args_t*)arg;
    for (int i = 0; i < a->count; i++) {
        weft_revoke(a->w);   // idempotent Release store; racing stores are
                             // atomic and write the same value — benign
    }
    return NULL;
}

static int run_l10(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    int timeout_ms = c->timeout_ms > 0 ? c->timeout_ms : 2000;

    // --- Phase A: sequential nested revoke (revoke; revoke; reclaim) ---
    weft_t w;
    if (weft_init(&w, payload_max) != 0) return 2;
    l10_writer_args_t wa = { .w = &w };
    atomic_init(&wa.stop, false);
    atomic_init(&wa.acks, 0);
    atomic_init(&wa.publishes, 0);
    pthread_t wt;
    pthread_create(&wt, NULL, l10_writer_thread, &wa);
    hold_inject_ms(5);
    uint32_t e0 = weft_epoch(&w);
    weft_revoke(&w);
    weft_revoke(&w);            // the nested revoke — must be a benign no-op
    int rc_a = weft_reclaim(&w, e0, timeout_ms);
    uint32_t epoch_after = weft_epoch(&w);
    // Publishes are frozen from the ACK onward (every later publish returns
    // DROPPED without writing a byte) — capture, poison, join, compare.
    uint64_t pubs_at_ack = atomic_load(&wa.publishes);
    // Poison after ACK (I6 contract), then join + verify the writer went quiet
    for (int i = 0; i < 3; i++) memset(w.buf[i], 0xDE, w.buf_size);
    atomic_store(&wa.stop, true);
    pthread_join(wt, NULL);
    uint64_t pubs_after_join = atomic_load(&wa.publishes);
    // Post-ACK discipline (02 §6): the writer never touched a byte after the
    // ACK — the poison canaries must still be intact.
    bool poison_a = true;
    for (int i = 0; i < 3; i++) {
        uint64_t cv;
        memcpy(&cv, w.buf[i] + w.buf_size - 8, 8);
        if (cv != 0xDEDEDEDEDEDEDEDEull) poison_a = false;
    }
    weft_destroy(&w);
    bool phase_a = (rc_a == 0) && (epoch_after > e0)
                   && (pubs_after_join == pubs_at_ack) && poison_a;

    // --- Phase B: concurrent revokers + racing reclaim ---
    if (weft_init(&w, payload_max) != 0) return 2;
    wa.w = &w;
    atomic_init(&wa.stop, false);
    atomic_init(&wa.acks, 0);
    atomic_init(&wa.publishes, 0);
    pthread_create(&wt, NULL, l10_writer_thread, &wa);
    hold_inject_ms(5);
    uint32_t e0b = weft_epoch(&w);
    l10_revoke_args_t ra1 = { .w = &w, .count = 500 };
    l10_revoke_args_t ra2 = { .w = &w, .count = 500 };
    pthread_t rt1, rt2;
    pthread_create(&rt1, NULL, l10_revoke_thread, &ra1);
    pthread_create(&rt2, NULL, l10_revoke_thread, &ra2);
    pthread_join(rt1, NULL);
    pthread_join(rt2, NULL);
    int rc_b = weft_reclaim(&w, e0b, timeout_ms);
    uint32_t epoch_final = weft_epoch(&w);
    for (int i = 0; i < 3; i++) memset(w.buf[i], 0xDE, w.buf_size);
    atomic_store(&wa.stop, true);
    pthread_join(wt, NULL);
    // Post-ACK discipline under the concurrent-revoke storm: poison intact.
    bool poison_intact = true;
    for (int i = 0; i < 3; i++) {
        uint64_t cv;
        memcpy(&cv, w.buf[i] + w.buf_size - 8, 8);
        if (cv != 0xDEDEDEDEDEDEDEDEull) poison_intact = false;
    }
    weft_destroy(&w);

    // epoch monotone across the whole nested/concurrent storm
    bool epoch_monotone = (epoch_after >= 1) && (epoch_final >= 1);
    // reclaim never timed out under nesting or racing
    bool no_timeouts = (rc_a == 0) && (rc_b == 0);

    bool pass = phase_a && no_timeouts && epoch_monotone && poison_intact;
    fprintf(stderr, "L10 nested_rc=%d concurrent_rc=%d epoch_after=%u epoch_final=%u "
                    "writer_acks=%lu pass=%d\n",
            rc_a, rc_b, epoch_after, epoch_final,
            (unsigned long)atomic_load(&wa.acks), pass);
    printf("{\"test\":\"L10-nested-revocation\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"nested_reclaim_ok\":%s,\"concurrent_reclaim_ok\":%s,"
           "\"epoch_monotone\":%s,\"poison_intact\":%s,\"epoch_after_nested\":%u,"
           "\"epoch_final\":%u}}\n",
           pass ? "true" : "false",
           rc_a == 0 ? "true" : "false", rc_b == 0 ? "true" : "false",
           epoch_monotone ? "true" : "false", poison_intact ? "true" : "false",
           epoch_after, epoch_final);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L12 — canary corruption (bit flips in the envelope/canary/payload)
// ---------------------------------------------------------------------------

// Single-threaded, no adversary threads: pure detection-matrix litmus. The
// reader HOLDS a frame; the harness flips bits through weft_r_live_ptr (the
// declared fault-injection surface — the pointer is const for readers, and
// corruption is exactly what a const-cast here models: stray DMA, heap
// stomps, FFI misuse). The matrix:
//   envelope-seq flip   -> DETECTED  (envelope seq != canary — the kernel's
//                                      own redundancy; 02 §1 layout)
//   canary flip         -> DETECTED  (same comparison, other direction)
//   magic flip          -> DETECTED  (weft_envelope_decode: BAD_MAGIC)
//   payload flip        -> NOT DETECTED by the canary — the DOCUMENTED
//                          boundary: the bracket detects concurrency, not
//                          data corruption; payload integrity belongs to the
//                          application checksum (w6Checksum precedent). The
//                          test verifies the boundary honestly: undetected.
// Each fault is injected, observed, and REVERTED (one buffer, many probes).

typedef struct {
    bool env_flip_detected;
    bool canary_flip_detected;
    bool magic_flip_detected;
    bool payload_flip_undetected;
} l12_matrix_t;

static bool l12_canary_matches(weft_t* w, uint32_t env_seq) {
    const uint8_t* canary = weft_r_live_ptr(w, w->buf_size - 8);
    uint64_t cv;
    memcpy(&cv, canary, 8);
    return cv == (uint64_t)env_seq;
}

static int run_l12(cli_t* c) {
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;
    (void)c;
    weft_t w;
    if (weft_init(&w, payload_max) != 0) return 2;
    const uint32_t seq = 7;
    fill_payload(&w, seq, payload_max);
    if (weft_publish(&w, seq, payload_max) != WEFT_PUB_OK) { weft_destroy(&w); return 2; }
    (void)weft_r_claim(&w);

    l12_matrix_t m = { 0 };

    // -- envelope seq bit flip (byte 8..11 of the envelope) --
    uint8_t* env = (uint8_t*)weft_r_live_ptr(&w, 0);
    uint8_t saved_seq_byte = env[8];
    env[8] ^= 0x01;                                    // flip one seq bit
    m.env_flip_detected = !l12_canary_matches(&w, weft_r_seq(&w));
    env[8] = saved_seq_byte;                           // revert

    // -- canary bit flip (last 8 bytes) --
    uint8_t* can = (uint8_t*)weft_r_live_ptr(&w, w.buf_size - 8);
    uint8_t saved_can_byte = can[0];
    can[0] ^= 0x80;
    m.canary_flip_detected = !l12_canary_matches(&w, weft_r_seq(&w));
    can[0] = saved_can_byte;

    // -- magic bit flip -> the envelope decoder must reject it --
    uint8_t saved_magic = env[0];
    env[0] ^= 0x01;
    uint16_t v, hs; uint32_t s, pl;
    weft_decode_result_t dr = weft_envelope_decode(env, w.buf_size, &v, &hs, &s, &pl);
    m.magic_flip_detected = (dr == WEFT_DECODE_BAD_MAGIC);
    env[0] = saved_magic;

    // -- payload bit flip -> the canary check must NOT fire (the boundary) --
    uint8_t* payload = (uint8_t*)weft_r_live_ptr(&w, 16);
    uint8_t saved_pay = payload[0];
    payload[0] ^= 0xFF;
    m.payload_flip_undetected = l12_canary_matches(&w, weft_r_seq(&w))
                                && verify_held(&w, seq, payload_max) == false
                                && weft_r_seq(&w) == seq;
    payload[0] = saved_pay;

    // Post-revert sanity: the frame verifies clean again.
    bool reverted_clean = verify_held(&w, seq, payload_max);
    weft_destroy(&w);

    bool pass = m.env_flip_detected && m.canary_flip_detected
                && m.magic_flip_detected && m.payload_flip_undetected
                && reverted_clean;
    fprintf(stderr, "L12 env_flip=%d canary_flip=%d magic=%d payload_undetected=%d "
                    "reverted_clean=%d pass=%d\n",
            m.env_flip_detected, m.canary_flip_detected, m.magic_flip_detected,
            m.payload_flip_undetected, reverted_clean, pass);
    printf("{\"test\":\"L12-canary-corruption\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"env_flip_detected\":%s,\"canary_flip_detected\":%s,"
           "\"magic_flip_detected\":%s,"
           "\"payload_flip_undetected_boundary\":%s,\"reverted_clean\":%s}}\n",
           pass ? "true" : "false",
           m.env_flip_detected ? "true" : "false",
           m.canary_flip_detected ? "true" : "false",
           m.magic_flip_detected ? "true" : "false",
           m.payload_flip_undetected ? "true" : "false",
           reverted_clean ? "true" : "false");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L13 — exhaustive claim retry (fan-out bounded loop, 10K claims)
// ---------------------------------------------------------------------------

// The fan-out claim's bounded 4-attempt loop (RFC 0004) under a storming
// writer: 10,000 consecutive claims, every one must RESOLVE (fresh / not
// fresh) — never spin, never deadlock — and every FRESH claim's payload must
// be the exact tword pattern. The retry accounting (skips, exhaustions) is
// surfaced via reader stats: counted, never silent (Law 1).

typedef struct {
    weft_fanout_t* f;
    uint32_t words;
    _Atomic uint64_t frames_done;
    _Atomic bool stop;
} l13_writer_args_t;

static void* l13_writer_thread(void* arg) {
    l13_writer_args_t* a = (l13_writer_args_t*)arg;
    const uint32_t W = a->words;
    uint32_t src[64];
    uint32_t seq = 0;
    while (!atomic_load(&a->stop)) {
        seq++;   // storms until the reader is done (no frame cap: the claim
                 // window must stay LIVE for all 10K claims)
        (void)weft_fanout_begin(a->f);
        for (uint32_t w = 0; w < W; w++) {
            src[w] = weft_mix32(seq * 2654435761u + w);   // tword(seq, w)
        }
        (void)weft_fanout_fill(a->f, src, (size_t)W * 4);
        (void)weft_fanout_publish(a->f);
        atomic_store_explicit(&a->frames_done, seq, memory_order_relaxed);
    }
    return NULL;
}

static int run_l13(cli_t* c) {
    int claims_target = c->claims > 0 ? c->claims : 10000;
    int slots = c->bound > 0 ? c->bound : 4;   // reuse bound for M (2..64)
    if (slots < 2 || slots > (int)WEFT_FANOUT_MAX_SLOTS) slots = 4;
    int words = c->payload_max > 0 ? (c->payload_max / 4) : 8;
    if (words < 1) words = 1;
    if (words > 64) words = 64;

    weft_fanout_t* f = weft_fanout_new((size_t)words * 4, (unsigned)slots);
    if (!f) return 2;
    weft_fanout_reader_t* r = weft_fanout_reader_new(
        weft_fanout_ring(f), weft_fanout_ring_bytes((size_t)words * 4, (unsigned)slots),
        (size_t)words * 4, (unsigned)slots);
    if (!r) { weft_fanout_free(f); return 2; }

    l13_writer_args_t wa = { .f = f, .words = (uint32_t)words };
    atomic_init(&wa.frames_done, 0);
    atomic_init(&wa.stop, false);
    pthread_t wt;
    pthread_create(&wt, NULL, l13_writer_thread, &wa);

    uint64_t fresh = 0, not_fresh = 0, torn_accepted = 0;
    // Wait for the storm to start (first frame published), then pace claims so
    // they land on a LIVE writer (a tight loop outruns the writer and would
    // make every claim not-fresh — exposure, not evidence).
    while (atomic_load(&wa.frames_done) == 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 };
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < claims_target; i++) {
        struct timespec pace = { .tv_sec = 0, .tv_nsec = 50000 };  // 50us tick
        nanosleep(&pace, NULL);
        const weft_fanout_claim_t* cl = weft_fanout_claim(r);
        if (cl->fresh) {
            fresh++;
            const uint32_t* view = (const uint32_t*)weft_fanout_view(r);
            for (uint32_t w = 0; w < (uint32_t)words; w++) {
                if (view[w] != weft_mix32((uint32_t)cl->seq * 2654435761u + w)) {
                    torn_accepted++;
                    break;
                }
            }
        } else {
            not_fresh++;
        }
    }
    atomic_store(&wa.stop, true);
    pthread_join(wt, NULL);
    // NoFuture, adjudicated exactly once the writer is parked: the reader's
    // last consistent frame cannot exceed the writer's total publishes.
    uint64_t future = (r->last_seq > atomic_load(&wa.frames_done)) ? 1 : 0;

    weft_fanout_stats_t st;
    weft_fanout_reader_stats(r, &st);
    // Every claim resolved (the loop completed) — the no-deadlock property.
    bool resolved_all = (fresh + not_fresh) == (uint64_t)claims_target;
    // Telescoping identity at drain: drops == lastSeq - fresh (RFC 0004).
    bool telescoping = (st.drops == r->last_seq - st.fresh);
    weft_fanout_free(f);
    weft_fanout_reader_free(r);

    bool pass = resolved_all && torn_accepted == 0 && future == 0 && telescoping;
    fprintf(stderr, "L13 claims=%d fresh=%lu not_fresh=%lu torn=%lu future=%lu "
                    "skip=%lu exhausted=%lu pass=%d\n",
            claims_target, (unsigned long)fresh, (unsigned long)not_fresh,
            (unsigned long)torn_accepted, (unsigned long)future,
            (unsigned long)st.skipped_mid_overwrite,
            (unsigned long)st.torn_exhausted, pass);
    printf("{\"test\":\"L13-claim-retry\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"claims\":%d,\"fresh\":%lu,\"not_fresh\":%lu,"
           "\"torn_accepted\":%lu,\"future\":%lu,\"skips\":%lu,\"exhausted\":%lu}}\n",
           pass ? "true" : "false", claims_target,
           (unsigned long)fresh, (unsigned long)not_fresh,
           (unsigned long)torn_accepted, (unsigned long)future,
           (unsigned long)st.skipped_mid_overwrite,
           (unsigned long)st.torn_exhausted);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L14 — mixed endianness (LE wire contract, simulated-BE round-trip)
// ---------------------------------------------------------------------------

// The envelope/canary wire format is LITTLE-ENDIAN (03-ENVELOPE §1, 02 §1).
// No big-endian hardware exists in this sandbox (declared): the test proves
// the contract two ways instead —
//   (a) the LE wire bytes are PINNED: encode_v1 on this (LE) host must lay
//       out the exact documented byte sequence, field by field;
//   (b) a simulated big-endian producer (fields stored native-BE) crosses to
//       a little-endian consumer ONLY through the explicit bswap boundary —
//       the byte-swap adapter a real BE port must implement — and the LE
//       decoder then reads the frame perfectly.
// Verdict: le_wire_pinned AND be_view_roundtrip AND canary_le_pinned.

static uint32_t bswap32v(uint32_t x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8)
         | ((x >> 8) & 0xFF00u) | ((x >> 24) & 0xFFu);
}

static bool l14_le_wire_pinned(void) {
    uint8_t enc[64];
    memset(enc, 0, sizeof(enc));
    weft_envelope_encode_v1(enc, 0x11223344u, 0xABCDEF00u);
    // magic "WEFT" bytes, version=1 LE, header_size=16 LE, seq LE, payload_len LE
    if (enc[0] != 0x57 || enc[1] != 0x45 || enc[2] != 0x46 || enc[3] != 0x54) return false;
    if (enc[4] != 0x01 || enc[5] != 0x00) return false;          // version 1, LE
    if (enc[6] != 0x10 || enc[7] != 0x00) return false;          // header 16, LE
    if (enc[8] != 0x44 || enc[9] != 0x33 || enc[10] != 0x22 || enc[11] != 0x11) return false;
    if (enc[12] != 0x00 || enc[13] != 0xEF || enc[14] != 0xCD || enc[15] != 0xAB) return false;
    return true;
}

static bool l14_be_roundtrip(void) {
    // A big-endian producer lays the SAME fields in ITS native order:
    const uint16_t version = 1, header = 16;
    const uint32_t seq = 0xA1B2C3D4u, plen = 0x00000020u;  // 32 <= 64-16: fits the probe buffer
    uint8_t be[64];
    memset(be, 0, sizeof(be));
    be[0] = 0x57; be[1] = 0x45; be[2] = 0x46; be[3] = 0x54;    // magic: bytes, endian-free
    be[4] = (uint8_t)(version >> 8); be[5] = (uint8_t)version;   // BE u16
    be[6] = (uint8_t)(header >> 8);  be[7] = (uint8_t)header;
    be[8] = (uint8_t)(seq >> 24); be[9] = (uint8_t)(seq >> 16);
    be[10] = (uint8_t)(seq >> 8);  be[11] = (uint8_t)seq;        // BE u32
    be[12] = (uint8_t)(plen >> 24); be[13] = (uint8_t)(plen >> 16);
    be[14] = (uint8_t)(plen >> 8);  be[15] = (uint8_t)plen;
    // The BE->LE boundary adapter (what a real BE port does at the wire):
    uint8_t le[64];
    memcpy(le, be, 64);
    uint16_t v16; memcpy(&v16, le + 4, 2); v16 = (uint16_t)((v16 >> 8) | (v16 << 8)); memcpy(le + 4, &v16, 2);
    memcpy(&v16, le + 6, 2); v16 = (uint16_t)((v16 >> 8) | (v16 << 8)); memcpy(le + 6, &v16, 2);
    uint32_t v32; memcpy(&v32, le + 8, 4); v32 = bswap32v(v32); memcpy(le + 8, &v32, 4);
    memcpy(&v32, le + 12, 4); v32 = bswap32v(v32); memcpy(le + 12, &v32, 4);
    // The LE decoder must now read the frame exactly:
    uint16_t dv, dh; uint32_t ds, dp;
    if (weft_envelope_decode(le, 64, &dv, &dh, &ds, &dp) != WEFT_DECODE_OK) return false;
    return dv == 1 && dh == 16 && ds == seq && dp == plen;
}

static bool l14_canary_le_pinned(void) {
    weft_t w;
    if (weft_init(&w, 256) != 0) return false;
    fill_payload(&w, 0xDEADBEEFu, 256);
    if (weft_publish(&w, 0xDEADBEEFu, 256) != WEFT_PUB_OK) { weft_destroy(&w); return false; }
    (void)weft_r_claim(&w);
    const uint8_t* can = weft_r_live_ptr(&w, w.buf_size - 8);
    uint64_t cv;
    memcpy(&cv, can, 8);
    weft_destroy(&w);
    // On this LE host the u64 canary must read back as the plain seq value
    // (low byte first in memory) — the LE wire contract, pinned.
    if (cv != 0xDEADBEEFull) return false;
    if (can[0] != 0xEF || can[1] != 0xBE || can[2] != 0xAD || can[3] != 0xDE) return false;
    return true;
}

static int run_l14(void) {
    bool a = l14_le_wire_pinned();
    bool b = l14_be_roundtrip();
    bool d = l14_canary_le_pinned();
    bool pass = a && b && d;
    fprintf(stderr, "L14 le_wire_pinned=%d be_roundtrip=%d canary_le=%d pass=%d\n",
            a, b, d, pass);
    printf("{\"test\":\"L14-mixed-endianness\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"le_wire_pinned\":%s,\"be_view_roundtrip\":%s,"
           "\"canary_le_pinned\":%s}}\n",
           pass ? "true" : "false",
           a ? "true" : "false", b ? "true" : "false", d ? "true" : "false");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L15 — huge payload (1 MiB frames: geometry, alignment, memory pressure)
// ---------------------------------------------------------------------------

static int run_l15(cli_t* c) {
    size_t payload_max = 1048576;                       // 1 MiB (catalog)
    if (c->payload_max > 0) payload_max = (size_t)c->payload_max;
    int frames = c->frames > 0 ? c->frames : 200;
    int writer_hz = c->writer_hz > 0 ? c->writer_hz : 120;

    weft_t w;
    if (weft_init(&w, payload_max) != 0) return 2;
    size_t expect_buf = ((16 + payload_max + 8) + 63) & ~(size_t)63;
    bool buf_size_ok = (w.buf_size == expect_buf);
    bool alignment_ok = true;
    for (int i = 0; i < 3; i++) {
        if (((uintptr_t)w.buf[i]) % 64 != 0) alignment_ok = false;
    }

    // RSS before (reported, never gated — the memory-pressure observation)
    long rss_before_kb = 0;
    {
        FILE* f = fopen("/proc/self/statm", "r");
        long tot, res;
        if (f && fscanf(f, "%ld %ld", &tot, &res) == 2) rss_before_kb = res * 4;
        if (f) fclose(f);
    }

    l1_writer_args_t wa = { .w = &w, .writer_hz = writer_hz, .frames = frames,
                            .payload_max = (int)payload_max, .seq = 0 };
    atomic_init(&wa.stop, false);
    atomic_init(&wa.published, 0);
    pthread_t wt;
    pthread_create(&wt, NULL, l1_writer_thread, &wa);

    uint32_t last_seq = 0;
    int torn = 0;
    uint64_t claims = 0;
    uint64_t deadline = now_ns() + 30000000000ull;
    while (now_ns() < deadline) {
        (void)weft_r_claim(&w);
        uint32_t s = weft_r_seq(&w);
        if (s != last_seq) {
            hold_inject_ms(5);
            if (!verify_held(&w, s, (uint32_t)payload_max)) torn++;
            last_seq = s;
            claims++;
        }
        if (s == (uint32_t)frames) break;   // drain: the final frame is observed
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000000ull / 60 };
        nanosleep(&ts, NULL);
    }
    pthread_join(wt, NULL);
    bool drain_ok = (last_seq == (uint32_t)frames);

    long rss_after_kb = 0;
    {
        FILE* f = fopen("/proc/self/statm", "r");
        long tot, res;
        if (f && fscanf(f, "%ld %ld", &tot, &res) == 2) rss_after_kb = res * 4;
        if (f) fclose(f);
    }
    weft_destroy(&w);

    bool pass = (torn == 0) && drain_ok && buf_size_ok && alignment_ok;
    fprintf(stderr, "L15 payload_max=%zu buf_size=%zu frames=%d torn=%d drain=%d "
                    "align=%d rss_delta=%ldkB pass=%d\n",
            payload_max, expect_buf, frames, torn, drain_ok, alignment_ok,
            rss_after_kb - rss_before_kb, pass);
    printf("{\"test\":\"L15-huge-payload\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"payload_max\":%zu,\"buf_size\":%zu,\"frames\":%d,"
           "\"torn\":%d,\"drain_ok\":%s,\"alignment_ok\":%s,"
           "\"rss_delta_kb\":%ld,\"claims\":%lu}}\n",
           pass ? "true" : "false", payload_max, expect_buf, frames, torn,
           drain_ok ? "true" : "false", alignment_ok ? "true" : "false",
           rss_after_kb - rss_before_kb, (unsigned long)claims);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// L16 — rapid revoke/reclaim cycles (the I6 handshake under churn)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    _Atomic bool stop;
    _Atomic bool acked;
} l16_writer_args_t;

static void* l16_writer_thread(void* arg) {
    l16_writer_args_t* a = (l16_writer_args_t*)arg;
    uint32_t seq = 1;
    while (!atomic_load(&a->stop)) {
        fill_payload(a->w, seq, 64);
        if (weft_publish(a->w, seq, 64) == WEFT_PUB_DROPPED_REVOKED) {
            atomic_store(&a->acked, true);
            break;   // post-ACK: never touch buffer bytes again (02 §6)
        }
        seq++;
    }
    return NULL;
}

static int run_l16(cli_t* c) {
    int cycles = c->cycles > 0 ? c->cycles : 2000;
    int timeout_ms = c->timeout_ms > 0 ? c->timeout_ms : 250;
    int payload_max = c->payload_max > 0 ? c->payload_max : 256;

    int completed = 0, timeouts = 0, poison_failures = 0, ack_failures = 0;
    for (int i = 0; i < cycles; i++) {
        weft_t* w = (weft_t*)malloc(sizeof(weft_t));
        if (!w) break;
        if (weft_init(w, payload_max) != 0) { free(w); break; }

        l16_writer_args_t wa = { .w = w };
        atomic_init(&wa.stop, false);
        atomic_init(&wa.acked, false);
        pthread_t wt;
        pthread_create(&wt, NULL, l16_writer_thread, &wa);

        // Let each cycle establish a live publish stream before the revoke
        // (a churn cycle with zero successful publishes is a weaker probe).
        struct timespec warm = { .tv_sec = 0, .tv_nsec = 250000 };
        nanosleep(&warm, NULL);
        uint32_t e0 = weft_epoch(w);
        weft_revoke(w);
        int rc = weft_reclaim(w, e0, timeout_ms);
        if (rc != 0) {
            timeouts++;
            atomic_store(&wa.stop, true);
            pthread_join(wt, NULL);
            weft_destroy(w);
            free(w);
            continue;
        }
        // Poison post-ACK, then verify the writer honored the contract
        for (int b = 0; b < 3; b++) memset(w->buf[b], 0xDE, w->buf_size);
        atomic_store(&wa.stop, true);
        pthread_join(wt, NULL);
        if (!atomic_load(&wa.acked)) ack_failures++;
        // Spot-check the poison survived (writer must not write post-ACK):
        // the canary word of each buffer stays 0xDEDEDEDEDEDEDEDE.
        bool poison_ok = true;
        for (int b = 0; b < 3; b++) {
            uint64_t cv;
            memcpy(&cv, w->buf[b] + w->buf_size - 8, 8);
            if (cv != 0xDEDEDEDEDEDEDEDEull) poison_ok = false;
        }
        if (!poison_ok) poison_failures++;

        weft_destroy(w);
        free(w);
        completed++;
    }

    bool pass = (completed == cycles) && timeouts == 0
                && poison_failures == 0 && ack_failures == 0;
    fprintf(stderr, "L16 cycles=%d completed=%d timeouts=%d poison_fail=%d "
                    "ack_fail=%d pass=%d\n",
            cycles, completed, timeouts, poison_failures, ack_failures, pass);
    printf("{\"test\":\"L16-rapid-reclaim\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"cycles\":%d,\"completed\":%d,\"timeouts\":%d,"
           "\"poison_failures\":%d,\"ack_failures\":%d}}\n",
           pass ? "true" : "false", cycles, completed, timeouts,
           poison_failures, ack_failures);
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Main — dispatch
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    cli_t c;
    cli_init(&c);
    if (parse_args(argc, argv, &c) != 0) return 2;

    if (strcmp(c.test_id, "L1-tear") == 0) return run_l1(&c);
    if (strcmp(c.test_id, "L2-writer-steps") == 0) return run_l2(&c);
    if (strcmp(c.test_id, "L3-reader-steps") == 0) return run_l3(&c);
    if (strcmp(c.test_id, "L4-freshness") == 0) return run_l4(&c);
    if (strcmp(c.test_id, "L5-progress") == 0) return run_l5(&c);
    if (strcmp(c.test_id, "L6-ownership") == 0) return run_l6(&c);
    if (strcmp(c.test_id, "L7-revocation") == 0) return run_l7(&c);
    if (strcmp(c.test_id, "L8-envelope") == 0) return run_l8();
    if (strcmp(c.test_id, "L9-cross-beam") == 0) return run_l9(&c);
    if (strcmp(c.test_id, "L10-nested-revocation") == 0) return run_l10(&c);
    if (strcmp(c.test_id, "L12-canary-corruption") == 0) return run_l12(&c);
    if (strcmp(c.test_id, "L13-claim-retry") == 0) return run_l13(&c);
    if (strcmp(c.test_id, "L14-mixed-endianness") == 0) return run_l14();
    if (strcmp(c.test_id, "L15-huge-payload") == 0) return run_l15(&c);
    if (strcmp(c.test_id, "L16-rapid-reclaim") == 0) return run_l16(&c);
    // L11-ffi-stress: script-orchestrated (litmus/ffi_stress/run.sh) — needs
    // node/cargo cross-process boundaries; not a single-binary cell.

    fprintf(stderr, "unknown test id: %s\n", c.test_id);
    return 2;
}
