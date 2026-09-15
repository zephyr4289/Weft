// litmus_runner.c — L1–L8 litmus runner (C)
//
// Per 04-LITMUS.md (procedures + verdicts) and 05-CONTRACTS.md (CLI + JSON output).
//
// CLI: ./litmus_runner <TEST_ID> [key=value ...]
//   TEST_ID ∈ L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress
//            L6-ownership L7-revocation L8-envelope
//
// Output: exactly ONE JSON line on stdout (the LAST line); all diagnostics to stderr.
// Exit: 0 pass · 1 fail · 2 usage/contract error.

#define _GNU_SOURCE  // for nanosleep, posix_memalign
#include "weft.h"

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

    fprintf(stderr, "unknown test id: %s\n", c.test_id);
    return 2;
}
