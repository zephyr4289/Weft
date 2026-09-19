// turbo_runner.c — RFC 0012 tail-latency EVIDENCE runner (T-bench family)
//
// The MEASUREMENT story (turbo_test.c owns the conformance story). Modes:
//
//   caps        capability report (turbo + uring ladders) — the honesty
//               preamble every evidence log starts with
//   TL-writer   publish-path latency: plain vs turbo-prefetch vs turbo-full
//               (mmap/THP ring + prefetch + NT fill + pin), 256 B and 64 KiB
//   TL-reader   claim-path latency at cadence with render-thrash between
//               frames (the honest display-thread model): plain vs
//               late-hint vs early-hint(+pin)
//   TL-ring     posix ring vs turbo mmap ring — claim latency INSIDE an
//               8 MiB ring (TLB-pressure regime)
//   ING-uring   ingestion throughput/latency (the negotiated uring rung;
//               build with -DWEFT_URING_FORCE_SYSCALL=1 for the A/B leg)
//
// Methodology (identical to bench_runner.c, the B-suite C leg): block +
// stride-sampled latencies via CLOCK_MONOTONIC, sorted percentiles
// p50/p90/p99/p999/max, clock_overhead_ns reported, no trimming, one JSON
// line per variant. Environment-tagged output; sandbox deltas stated.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/socket.h>

#include "turbo.h"
#include "uring_rx.h"
#include "fanout.h"
#include "fanout_simd.h"
#include "weft.h"

// ---------------------------------------------------------------------------
// Timing utilities (bench_runner.c methodology)
// ---------------------------------------------------------------------------

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t clock_overhead_ns(void) {
    uint64_t o[64];
    for (int i = 0; i < 64; i++) {
        uint64_t a = now_ns();
        uint64_t b = now_ns();
        o[i] = b - a;
    }
    for (int i = 1; i < 64; i++) {  // insertion sort
        uint64_t v = o[i];
        int j = i - 1;
        while (j >= 0 && o[j] > v) { o[j + 1] = o[j]; j--; }
        o[j + 1] = v;
    }
    return o[32];
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void sort_u64(uint64_t* a, size_t n) {
    qsort(a, n, sizeof(uint64_t), cmp_u64);
}

static uint64_t percentile_u64(uint64_t* sorted, size_t n, double p) {
    if (n == 0) return 0;
    double idx = (p / 100.0) * (double)n;
    size_t i = (size_t)idx;
    if (i >= n) i = n - 1;
    return sorted[i];
}

typedef struct {
    uint64_t p50, p90, p99, p999, pmax, n;
    double ops_per_s;
} dist_t;

static dist_t dist_from_samples(uint64_t* s, size_t n, double seconds) {
    dist_t d;
    memset(&d, 0, sizeof(d));
    if (n == 0) return d;
    sort_u64(s, n);
    d.n = n;
    d.p50 = percentile_u64(s, n, 50.0);
    d.p90 = percentile_u64(s, n, 90.0);
    d.p99 = percentile_u64(s, n, 99.0);
    d.p999 = percentile_u64(s, n, 99.9);
    d.pmax = s[n - 1];
    d.ops_per_s = seconds > 0 ? (double)n / seconds : 0;
    return d;
}

static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

// ---------------------------------------------------------------------------
// TL-writer — publish-path latency, three variants x two payload sizes
// ---------------------------------------------------------------------------

typedef enum {
    WV_PLAIN = 0,    // frozen begin/fill/publish, posix ring
    WV_PREFETCH = 1, // + prefetch wrappers, posix ring
    WV_RING = 2,     // turbo mmap ring (THP/prefault) + frozen fill + prefetch
    WV_NT = 3,       // posix ring + NT streaming fill + prefetch
    WV_FULL = 4      // turbo ring + NT fill + prefetch + writer pinned
} writer_variant_t;

static const char* wv_name(writer_variant_t v) {
    switch (v) {
        case WV_PLAIN:    return "plain";
        case WV_PREFETCH: return "turbo-prefetch";
        case WV_RING:     return "turbo-ring";
        case WV_NT:       return "turbo-nt-fill";
        case WV_FULL:     return "turbo-full";
    }
    return "?";
}

static void wv_frame(weft_fanout_t* f, writer_variant_t variant,
                     const void* buf, size_t payload_bytes) {
    if (variant == WV_PLAIN || variant == WV_RING) {
        if (variant == WV_PLAIN) (void)weft_fanout_begin(f);
        else                      (void)weft_turbo_begin(f);
        (void)weft_fanout_fill(f, buf, payload_bytes);
        if (variant == WV_PLAIN) (void)weft_fanout_publish(f);
        else                      (void)weft_turbo_publish(f);
    } else {
        (void)weft_turbo_begin(f);
        (void)weft_turbo_fill(f, buf, payload_bytes);
        (void)weft_turbo_publish(f);
    }
}

static void run_tl_writer(size_t payload_bytes, unsigned m, uint32_t frames,
                          int stride, writer_variant_t variant) {
    uint32_t* buf = malloc(payload_bytes);
    if (!buf) { fprintf(stderr, "oom\n"); exit(2); }
    const size_t words = payload_bytes / 4;

    weft_fanout_t f;
    weft_turbo_ring_t tr;
    memset(&tr, 0, sizeof(tr));
    if (variant == WV_FULL || variant == WV_RING) {
        weft_turbo_ring_opts_t o;
        weft_turbo_ring_opts_default(&o);
        o.slot_count = m;
        o.payload_bytes = payload_bytes;
        if (weft_turbo_fanout_create(&f, &tr, &o) != 0) {
            fprintf(stderr, "turbo ring create failed\n"); exit(2);
        }
        if (variant == WV_FULL) (void)weft_turbo_pin_cpu(0);
    } else {
        if (weft_fanout_init(&f, payload_bytes, m) != 0) {
            fprintf(stderr, "fanout init failed\n"); exit(2);
        }
    }

    // Warmup (10k frames) — same code path as the measured loop.
    for (uint32_t seq = 1; seq <= 10000; seq++) {
        for (size_t w = 0; w < words; w++) buf[w] = tword(seq, (uint32_t)w);
        wv_frame(&f, variant, buf, payload_bytes);
    }

    const size_t max_samples = frames / (size_t)stride + 16;
    uint64_t* samples = malloc(max_samples * sizeof(uint64_t));
    size_t ns = 0;
    const uint64_t t_start = now_ns();

    for (uint32_t seq = 1; seq <= frames; seq++) {
        for (size_t w = 0; w < words; w++) buf[w] = tword(seq, (uint32_t)w);
        const uint64_t t0 = now_ns();
        wv_frame(&f, variant, buf, payload_bytes);
        const uint64_t t1 = now_ns();
        if ((seq % (uint32_t)stride) == 0 && ns < max_samples) {
            samples[ns++] = t1 - t0;
        }
    }
    const uint64_t t_end = now_ns();
    const double seconds = (double)(t_end - t_start) / 1e9;

    // NOTE: samples hold the stride-sampled latencies; ops_per_s uses the
    // FULL frame count over the full window.
    dist_t d = dist_from_samples(samples, ns, seconds);
    d.ops_per_s = (double)frames / seconds;
    printf("{\"bench\":\"TL-writer\",\"lang\":\"c\",\"variant\":\"%s\","
           "\"payload_max\":%zu,\"slots\":%u,\"frames\":%u,\"stride\":%d,"
           "\"ops_per_s\":%.0f,\"p50\":%lu,\"p90\":%lu,\"p99\":%lu,"
           "\"p999\":%lu,\"max\":%lu,\"samples\":%lu,"
           "\"clock_overhead_ns\":%lu}\n",
           wv_name(variant), payload_bytes, m, frames, stride,
           d.ops_per_s, d.p50, d.p90, d.p99, d.p999, d.pmax,
           (unsigned long)d.n, clock_overhead_ns());

    free(samples);
    free(buf);
    if (variant == WV_FULL || variant == WV_RING) {
        weft_turbo_fanout_destroy(&f, &tr);
    } else {
        weft_fanout_destroy(&f);
    }
}

// ---------------------------------------------------------------------------
// TL-reader — claim latency at cadence with render-thrash (display model)
// ---------------------------------------------------------------------------

typedef struct {
    weft_fanout_t* fan;
    _Atomic int stop;
    uint64_t published;
} tlw_arg_t;

static void* tl_writer_entry(void* arg) {
    tlw_arg_t* wa = (tlw_arg_t*)arg;
    (void)weft_turbo_pin_cpu(0);
    const size_t pb = wa->fan->payload_bytes;
    const size_t words = pb / 4;
    uint32_t* buf = malloc(pb);
    uint32_t seq = 0;
    while (!atomic_load_explicit(&wa->stop, memory_order_relaxed)) {
        seq++;
        for (size_t w = 0; w < words; w++) buf[w] = tword(seq, (uint32_t)w);
        (void)weft_turbo_begin(wa->fan);
        (void)weft_turbo_fill(wa->fan, buf, pb);
        (void)weft_turbo_publish(wa->fan);
    }
    wa->published = seq;
    free(buf);
    return NULL;
}

typedef enum {
    RV_PLAIN = 0,     // weft_fanout_claim
    RV_LATE = 1,      // weft_turbo_claim (hint inside the call)
    RV_EARLY = 2,     // weft_turbo_prefetch_next BEFORE the render work
    RV_EARLY_PIN = 3  // early hint + reader pinned
} reader_variant_t;

static const char* rv_name(reader_variant_t v) {
    switch (v) {
        case RV_PLAIN:      return "plain";
        case RV_LATE:       return "turbo-late-hint";
        case RV_EARLY:      return "turbo-early-hint";
        case RV_EARLY_PIN:  return "turbo-early-hint+pin";
    }
    return "?";
}

/// The render-work simulator: sweep a scratch buffer the caller declares.
static volatile uint64_t g_sink;

static void render_thrash(uint32_t* scratch, size_t words) {
    uint64_t acc = 0;
    for (size_t w = 0; w < words; w++) acc += scratch[w];
    g_sink = acc;
}

static void run_tl_reader(size_t payload_bytes, unsigned m,
                          double measure_s, double cadence_us,
                          size_t thrash_words, reader_variant_t variant) {
    weft_turbo_ring_opts_t o;
    weft_turbo_ring_opts_default(&o);
    o.slot_count = m;
    o.payload_bytes = payload_bytes;
    weft_fanout_t f;
    weft_turbo_ring_t tr;
    if (weft_turbo_fanout_create(&f, &tr, &o) != 0) {
        fprintf(stderr, "turbo ring create failed\n"); exit(2);
    }

    weft_fanout_reader_t r;
    if (weft_fanout_reader_init(&r, tr.ring, tr.ring_bytes, payload_bytes, m) != 0) {
        fprintf(stderr, "reader init failed\n"); exit(2);
    }

    uint32_t* scratch = malloc(thrash_words * 4);
    for (size_t w = 0; w < thrash_words; w++) scratch[w] = (uint32_t)w;

    tlw_arg_t wa;
    memset(&wa, 0, sizeof(wa));
    wa.fan = &f;
    atomic_store(&wa.stop, 0);
    pthread_t tw;
    if (pthread_create(&tw, NULL, tl_writer_entry, &wa) != 0) {
        fprintf(stderr, "writer thread failed\n"); exit(2);
    }

    if (variant == RV_EARLY_PIN) (void)weft_turbo_pin_cpu(1);

    // Warmup: 200 cadence ticks.
    for (int i = 0; i < 200; i++) {
        render_thrash(scratch, thrash_words);
        (void)weft_turbo_claim(&r);
    }

    const size_t max_samples = (size_t)(measure_s * 1e6 / cadence_us) + 64;
    uint64_t* samples = malloc(max_samples * sizeof(uint64_t));
    size_t ns = 0;
    const uint64_t t_start = now_ns();
    const uint64_t cadence_ns = (uint64_t)(cadence_us * 1000.0);
    uint64_t deadline = t_start + cadence_ns;

    while (ns < max_samples - 1) {
        while (now_ns() < deadline) { /* cadence spin */ }
        if (variant >= RV_EARLY) {
            weft_turbo_prefetch_next(&r);   // EARLY: overlaps the render work
        }
        render_thrash(scratch, thrash_words);  // the display thread's frame work
        const uint64_t t0 = now_ns();
        if (variant == RV_PLAIN) {
            (void)weft_fanout_claim(&r);
        } else {
            (void)weft_turbo_claim(&r);
        }
        const uint64_t t1 = now_ns();
        samples[ns++] = t1 - t0;
        deadline += cadence_ns;
    }
    const uint64_t t_end = now_ns();

    atomic_store(&wa.stop, 1);
    pthread_join(tw, NULL);

    const double seconds = (double)(t_end - t_start) / 1e9;
    const dist_t d = dist_from_samples(samples, ns, seconds);
    printf("{\"bench\":\"TL-reader\",\"lang\":\"c\",\"variant\":\"%s\","
           "\"copy_impl\":\"%s\",\"pf_bytes\":%d,"
           "\"payload_max\":%zu,\"slots\":%u,\"cadence_us\":%.0f,"
           "\"thrash_kib\":%zu,\"writer_hz\":%.0f,\"samples\":%lu,"
           "\"p50\":%lu,\"p90\":%lu,\"p99\":%lu,\"p999\":%lu,\"max\":%lu,"
           "\"clock_overhead_ns\":%lu}\n",
           rv_name(variant), weft_fanout_copy_active_impl(),
           weft_turbo_prefetch_get_distance(),
           payload_bytes, m, cadence_us,
           thrash_words * 4 / 1024, (double)wa.published / seconds,
           (unsigned long)d.n, d.p50, d.p90, d.p99, d.p999, d.pmax,
           clock_overhead_ns());

    free(samples);
    free(scratch);
    weft_fanout_reader_destroy(&r);
    weft_turbo_fanout_destroy(&f, &tr);
}

// ---------------------------------------------------------------------------
// TL-ring — posix ring vs turbo mmap ring (TLB-pressure regime, 8 MiB ring)
// ---------------------------------------------------------------------------

static double tl_ring_pass(int use_turbo_ring, size_t payload_bytes, unsigned m,
                           double measure_s, double cadence_us, size_t thrash_words) {
    weft_fanout_t f;
    weft_turbo_ring_t tr;
    memset(&tr, 0, sizeof(tr));
    if (use_turbo_ring) {
        weft_turbo_ring_opts_t o;
        weft_turbo_ring_opts_default(&o);
        o.slot_count = m;
        o.payload_bytes = payload_bytes;
        if (weft_turbo_fanout_create(&f, &tr, &o) != 0) exit(2);
    } else {
        if (weft_fanout_init(&f, payload_bytes, m) != 0) exit(2);
    }
    weft_fanout_reader_t r;
    if (weft_fanout_reader_init(&r, f.ring, weft_fanout_ring_bytes(payload_bytes, m),
                                payload_bytes, m) != 0) exit(2);
    uint32_t* scratch = malloc(thrash_words * 4);
    for (size_t w = 0; w < thrash_words; w++) scratch[w] = (uint32_t)w;

    tlw_arg_t wa;
    memset(&wa, 0, sizeof(wa));
    wa.fan = &f;
    atomic_store(&wa.stop, 0);
    pthread_t tw;
    pthread_create(&tw, NULL, tl_writer_entry, &wa);

    for (int i = 0; i < 100; i++) {  // warmup
        render_thrash(scratch, thrash_words);
        (void)weft_turbo_claim(&r);
    }

    const size_t max_samples = (size_t)(measure_s * 1e6 / cadence_us) + 64;
    uint64_t* samples = malloc(max_samples * sizeof(uint64_t));
    size_t ns = 0;
    const uint64_t t_start = now_ns();
    const uint64_t cadence_ns = (uint64_t)(cadence_us * 1000.0);
    uint64_t deadline = t_start + cadence_ns;

    while (ns < max_samples - 1) {
        while (now_ns() < deadline) { /* spin */ }
        weft_turbo_prefetch_next(&r);
        render_thrash(scratch, thrash_words);
        const uint64_t t0 = now_ns();
        (void)weft_turbo_claim(&r);
        const uint64_t t1 = now_ns();
        samples[ns++] = t1 - t0;
        deadline += cadence_ns;
    }
    const uint64_t t_end = now_ns();
    atomic_store(&wa.stop, 1);
    pthread_join(tw, NULL);

    const double seconds = (double)(t_end - t_start) / 1e9;
    const dist_t d = dist_from_samples(samples, ns, seconds);
    printf("{\"bench\":\"TL-ring\",\"lang\":\"c\",\"variant\":\"%s\","
           "\"payload_max\":%zu,\"slots\":%u,\"ring_mib\":%.1f,"
           "\"cadence_us\":%.0f,\"samples\":%lu,"
           "\"p50\":%lu,\"p90\":%lu,\"p99\":%lu,\"p999\":%lu,\"max\":%lu,"
           "\"clock_overhead_ns\":%lu}\n",
           use_turbo_ring ? "turbo-mmap" : "posix-alloc",
           payload_bytes, m,
           (double)weft_fanout_ring_bytes(payload_bytes, m) / (1024.0 * 1024.0),
           cadence_us, (unsigned long)d.n,
           d.p50, d.p90, d.p99, d.p999, d.pmax, clock_overhead_ns());

    free(samples);
    free(scratch);
    weft_fanout_reader_destroy(&r);
    if (use_turbo_ring) weft_turbo_fanout_destroy(&f, &tr);
    else weft_fanout_destroy(&f);
    return 0.0;
}

// ---------------------------------------------------------------------------
// ING-uring — ingestion throughput + per-frame latency
// ---------------------------------------------------------------------------

typedef struct {
    int fd;
    uint64_t total;
} ing_feeder_t;

static void* ing_feeder_entry(void* arg) {
    ing_feeder_t* fe = (ing_feeder_t*)arg;
    const size_t pb = 256;
    uint32_t* pkt = malloc(pb);
    for (uint64_t seq = 1; seq <= fe->total; seq++) {
        for (size_t w = 0; w < pb / 4; w++) pkt[w] = tword((uint32_t)seq, (uint32_t)w);
        ssize_t n;
        do {
            n = send(fe->fd, pkt, pb, 0);
        } while (n < 0 && errno == EINTR);
    }
    free(pkt);
    return NULL;
}

static void run_ing_uring(uint64_t total) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "socketpair failed\n"); exit(2);
    }
    weft_fanout_t f;
    if (weft_fanout_init(&f, 256, 8) != 0) exit(2);
    weft_uring_rx_t rx;
    if (weft_uring_attach(&rx, &f, sv[0]) != 0) {
        fprintf(stderr, "uring attach failed\n"); exit(2);
    }

    ing_feeder_t fe = { .fd = sv[1], .total = total };
    pthread_t tf;
    pthread_create(&tf, NULL, ing_feeder_entry, &fe);

    const size_t max_samples = 65536;
    uint64_t* samples = malloc(max_samples * sizeof(uint64_t));
    size_t ns = 0;
    const uint64_t t_start = now_ns();
    while (rx.stats.frames < total) {
        const uint64_t t0 = now_ns();
        const uint64_t seq = weft_uring_next(&rx);
        const uint64_t t1 = now_ns();
        if (seq != 0 && ns < max_samples && (ns % 1) == 0) {
            samples[ns] = t1 - t0;
            ns++;
        }
    }
    const uint64_t t_end = now_ns();
    pthread_join(tf, NULL);

    const double seconds = (double)(t_end - t_start) / 1e9;
    const dist_t d = dist_from_samples(samples, ns, seconds);
    printf("{\"bench\":\"ING-uring\",\"lang\":\"c\",\"mode\":\"%s\","
           "\"frames\":%lu,\"seconds\":%.3f,\"frames_per_s\":%.0f,"
           "\"mb_per_s\":%.1f,\"p50\":%lu,\"p90\":%lu,\"p99\":%lu,"
           "\"p999\":%lu,\"max\":%lu,\"enters\":%lu,\"syscalls\":%lu,"
           "\"aborted\":%lu,\"clock_overhead_ns\":%lu}\n",
           weft_uring_mode_str(rx.mode), (unsigned long)rx.stats.frames,
           seconds, (double)rx.stats.frames / seconds,
           (double)rx.stats.bytes / seconds / (1024.0 * 1024.0),
           d.p50, d.p90, d.p99, d.p999, d.pmax,
           (unsigned long)rx.stats.enters, (unsigned long)rx.stats.syscalls,
           (unsigned long)rx.stats.aborted, clock_overhead_ns());

    free(samples);
    weft_uring_detach(&rx);
    weft_fanout_destroy(&f);
    close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char* argv0) {
    fprintf(stderr, "usage: %s <caps|TL-writer|TL-reader|TL-ring|ING-uring> [key=value ...]\n"
            "  keys: payload= slots= measure_s= cadence_us= thrash_kib= frames= variant=\n"
            "  TL-reader only: copy=<scalar|sse2|avx2|avx512|neon> (pin the claim copy;\n"
            "                  issue #17-1 A/B — auto = widest silicon path)\n"
            "  TL-reader only: pf=<bytes|0|auto> (prefetch distance; issue #17-4 sweep)\n", argv0);
    exit(2);
}

int main(int argc, char** argv) {
    if (argc < 2) usage(argv[0]);
    const char* mode = argv[1];
    size_t payload = 256;
    unsigned slots = 8;
    double measure_s = 3.0;
    double cadence_us = 500.0;
    size_t thrash_kib = 1024;
    uint64_t frames = 200000;
    int variant = -1;  // -1 = all (diagnostic); evidence runs set variant=
    const char* copy_impl = NULL;  // TL-reader only: pin the claim-copy impl
    const char* pf_bytes = NULL;   // TL-reader only: prefetch distance ("auto" tunes)

    for (int i = 2; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (!eq) usage(argv[0]);
        *eq = 0;
        const char* k = argv[i];
        const char* v = eq + 1;
        if (strcmp(k, "payload") == 0) payload = (size_t)atoi(v);
        else if (strcmp(k, "slots") == 0) slots = (unsigned)atoi(v);
        else if (strcmp(k, "measure_s") == 0) measure_s = atof(v);
        else if (strcmp(k, "cadence_us") == 0) cadence_us = atof(v);
        else if (strcmp(k, "thrash_kib") == 0) thrash_kib = (size_t)atoi(v);
        else if (strcmp(k, "frames") == 0) frames = strtoull(v, NULL, 10);
        else if (strcmp(k, "variant") == 0) {
            // Writer: plain|turbo-prefetch|turbo-ring|turbo-nt-fill|turbo-full
            // Reader: plain|turbo-late-hint|turbo-early-hint|turbo-early-hint+pin
            // Ring:   posix|turbo-mmap
            if (strcmp(v, "plain") == 0) variant = 0;
            else if (strcmp(v, "turbo-prefetch") == 0) variant = 1;
            else if (strcmp(v, "turbo-ring") == 0) variant = 2;
            else if (strcmp(v, "turbo-nt-fill") == 0) variant = 3;
            else if (strcmp(v, "turbo-full") == 0) variant = 4;
            else if (strcmp(v, "turbo-late-hint") == 0) variant = 1;
            else if (strcmp(v, "turbo-early-hint") == 0) variant = 2;
            else if (strcmp(v, "turbo-early-hint+pin") == 0) variant = 3;
            else if (strcmp(v, "turbo-mmap") == 0) variant = 1;
            else if (strcmp(v, "posix-alloc") == 0) variant = 0;
            else usage(argv[0]);
        }
        else if (strcmp(k, "copy") == 0) copy_impl = v;
        else if (strcmp(k, "pf") == 0) pf_bytes = v;  // TL-reader only
        else usage(argv[0]);
    }

    if (strcmp(mode, "caps") == 0) {
        char tb[512], ub[256];
        weft_turbo_caps_report(tb, sizeof(tb));
        weft_uring_report(ub, sizeof(ub));
        fputs(tb, stdout);
        fputs(ub, stdout);
        return 0;
    }
    if (strcmp(mode, "TL-writer") == 0) {
        // House methodology (bench_runner.c): ONE variant per process
        // invocation — no cross-variant heap/layout state. The evidence
        // driver interleaves A-B-A-B across invocations.
        const int wv_max = 4;
        if (variant >= 0 && variant <= wv_max) {
            run_tl_writer(payload, slots, (uint32_t)frames, 16, (writer_variant_t)variant);
        } else {
            const size_t payloads[2] = { 256, 65536 };
            for (int p = 0; p < 2; p++) {
                for (int v = 0; v <= wv_max; v++) {
                    run_tl_writer(payloads[p], slots, (uint32_t)frames, 16, (writer_variant_t)v);
                }
            }
        }
        return 0;
    }
    if (strcmp(mode, "TL-reader") == 0) {
        // Issue #17-1 A/B: pin the claim-copy implementation (the seam's
        // dispatcher). Refused pins abort — a typo'd impl name must never
        // silently bench the wrong path (honest measurement, Law 4).
        if (copy_impl && weft_fanout_copy_force_impl(copy_impl) != 0) {
            fprintf(stderr, "copy=%s refused (not compiled in or silicon lacks it)\n",
                    copy_impl);
            return 2;
        }
        // Issue #17-4 A/B: pin the runtime prefetch distance (integer bytes,
        // 0 = off, or "auto" for the vendor tune). Refused values abort —
        // a typo'd distance must never silently bench the wrong hint batch.
        if (pf_bytes) {
            if (strcmp(pf_bytes, "auto") == 0) {
                weft_turbo_prefetch_tune();
            } else {
                const int d = atoi(pf_bytes);
                if (d < 0 || weft_turbo_prefetch_set_distance(d) != d) {
                    fprintf(stderr, "pf=%s refused (negative or over hard max)\n",
                            pf_bytes);
                    return 2;
                }
            }
        }
        if (variant >= 0 && variant <= 3) {
            run_tl_reader(payload, slots, measure_s, cadence_us,
                          thrash_kib * 256 /* words */, (reader_variant_t)variant);
        } else {
            for (int v = 0; v <= 3; v++) {
                run_tl_reader(payload, slots, measure_s, cadence_us,
                              thrash_kib * 256 /* words */, (reader_variant_t)v);
            }
        }
        weft_fanout_copy_force_auto();
        return 0;
    }
    if (strcmp(mode, "TL-ring") == 0) {
        const size_t pb = 1024 * 1024;  // 1 MiB payloads -> 8 MiB ring
        if (variant == 0 || variant == 1) {
            tl_ring_pass(variant, pb, 8, measure_s, cadence_us, thrash_kib * 256);
        } else {
            tl_ring_pass(0, pb, 8, measure_s, cadence_us, thrash_kib * 256);
            tl_ring_pass(1, pb, 8, measure_s, cadence_us, thrash_kib * 256);
        }
        return 0;
    }
    if (strcmp(mode, "ING-uring") == 0) {
        run_ing_uring(frames);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
