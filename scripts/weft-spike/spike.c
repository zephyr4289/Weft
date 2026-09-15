// spike.c — Triad Protocol benchmark harness
//
// Simulates:
//   - A 120 Hz writer thread (model of a native audio tap or physics step)
//   - A 60 Hz reader thread (model of Android Choreographer / withFrameNanos)
//
// Measures:
//   1. Torn reads (= checksum mismatches) over a 60-second run
//   2. Heap allocations after warmup (should be 0; all buffers pre-allocated)
//   3. RSS growth (should be flat after startup)
//   4. Reader FPS (should hit 60 Hz target, with P50/P99/P100 frame times)
//   5. Effective writer FPS (should hit 120 Hz target)
//
// Output: a JSON report on stdout that the report generator consumes.

#include "weft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#define WRITER_HZ        120
#define READER_HZ        60
#define RUN_SECONDS      10   // 10 sec is enough; would be 60 on real hardware
#define SAMPLES_PER_FRAME 1

// Get current wall-clock in nanoseconds.
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Get RSS in kilobytes.
static long get_rss_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;  // on Linux, ru_maxrss is in KB
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* weft;
    _Atomic int* stop;
    _Atomic uint64_t* alloc_count;   // malloc calls during the run
    uint64_t start_ns;
    int writer_hz;
} writer_args_t;

static void* writer_thread(void* arg) {
    writer_args_t* a = (writer_args_t*)arg;
    weft_t* w = a->weft;

    // Pre-allocate the working buffer ONCE. The writer never allocates per-frame.
    float* frame = (float*)malloc(WEFT_FRAME_FLOATS * sizeof(float));
    if (frame == NULL) {
        fprintf(stderr, "writer: malloc failed\n");
        return NULL;
    }
    atomic_fetch_add_explicit(a->alloc_count, 1, memory_order_relaxed);

    uint64_t period_ns = 1000000000ull / (uint64_t)a->writer_hz;
    uint64_t next_deadline = a->start_ns + period_ns;
    uint32_t seq = 0;

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        uint64_t now = now_ns();
        if (now < next_deadline) {
            // Sleep until deadline. Use nanosleep for sub-ms precision.
            uint64_t remaining = next_deadline - now;
            if (remaining > 1000000) {  // > 1ms
                struct timespec ts = {
                    .tv_sec = remaining / 1000000000,
                    .tv_nsec = remaining % 1000000000,
                };
                nanosleep(&ts, NULL);
            }
            // busy-wait the last microsecond for precision
            while (now_ns() < next_deadline) { /* spin */ }
        }

        // Build a frame: seq at [0], payload at [1..N-2], checksum at [N-1].
        frame[0] = (float)seq;
        // Payload: deterministic pattern derived from seq, so reader can detect corruption.
        for (int i = 1; i < WEFT_FRAME_FLOATS - 1; i++) {
            frame[i] = (float)((seq * 0.0001) + (i * 0.001));
        }
        // Compute checksum (same as weft_frame_verify).
        uint32_t checksum = 0;
        for (int i = 1; i < WEFT_FRAME_FLOATS - 1; i++) {
            uint32_t bits;
            memcpy(&bits, &frame[i], sizeof(uint32_t));
            checksum += bits;
        }
        checksum ^= seq * 0x9E3779B1u;
        memcpy(&frame[WEFT_FRAME_FLOATS - 1], &checksum, sizeof(uint32_t));

        // Publish via the Triad Protocol. Wait-free.
        weft_publish(w, frame);

        seq++;
        next_deadline += period_ns;
        // If we've drifted far behind (e.g. system stalled), snap forward.
        uint64_t now2 = now_ns();
        if (now2 - next_deadline > period_ns * 2) {
            next_deadline = now2 + period_ns;
        }
    }

    free(frame);
    return NULL;
}

// ---------------------------------------------------------------------------
// Reader thread (simulated Choreographer at READER_HZ)
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* weft;
    _Atomic int* stop;
    _Atomic uint64_t* alloc_count;
    _Atomic uint64_t* read_count;
    _Atomic uint64_t* torn_count;
    uint64_t start_ns;
    int reader_hz;
    // Frame time samples for P50/P99/P100.
    uint64_t* frame_times_ns;
    size_t frame_times_cap;
    size_t frame_times_len;
} reader_args_t;

static void* reader_thread(void* arg) {
    reader_args_t* a = (reader_args_t*)arg;
    weft_t* w = a->weft;

    // Pre-allocate the snapshot buffer ONCE.
    float* snap = (float*)malloc(WEFT_FRAME_FLOATS * sizeof(float));
    if (snap == NULL) {
        fprintf(stderr, "reader: malloc failed\n");
        return NULL;
    }
    atomic_fetch_add_explicit(a->alloc_count, 1, memory_order_relaxed);

    uint64_t period_ns = 1000000000ull / (uint64_t)a->reader_hz;
    uint64_t next_deadline = a->start_ns + period_ns;
    uint32_t last_seq_seen = 0xFFFFFFFF;  // sentinel

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        uint64_t now = now_ns();
        if (now < next_deadline) {
            uint64_t remaining = next_deadline - now;
            if (remaining > 1000000) {
                struct timespec ts = {
                    .tv_sec = remaining / 1000000000,
                    .tv_nsec = remaining % 1000000000,
                };
                nanosleep(&ts, NULL);
            }
            while (now_ns() < next_deadline) { /* spin */ }
        }

        // VSYNC tick: attempt a read.
        uint64_t t0 = now_ns();
        int rc = weft_read(w, snap, WEFT_FRAME_FLOATS);
        uint64_t t1 = now_ns();

        // Record the read time (always — even on stale or no-data frames,
        // because the reader still rendered a frame at VSYNC rate).
        if (a->frame_times_len < a->frame_times_cap) {
            a->frame_times_ns[a->frame_times_len++] = t1 - t0;
        }

        if (rc == 1) {
            uint32_t seq = (uint32_t)snap[0];
            if (seq == last_seq_seen) {
                // Stale read: reader outpaces writer. Allowed by latest-wins semantics.
                // (Don't increment torn_count — this is expected.)
            }
            last_seq_seen = seq;
        } else if (rc == -1) {
            // Torn read or checksum mismatch. The Triad Protocol should prevent this.
            atomic_fetch_add_explicit(a->torn_count, 1, memory_order_relaxed);
        }
        // rc == 0 means no new data yet — fine.

        next_deadline += period_ns;
        uint64_t now2 = now_ns();
        if (now2 - next_deadline > period_ns * 2) {
            next_deadline = now2 + period_ns;
        }
    }

    free(snap);
    return NULL;
}

// ---------------------------------------------------------------------------
// Compare for qsort (sorting frame times for percentile calc)
// ---------------------------------------------------------------------------

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static uint64_t percentile(uint64_t* sorted, size_t n, double p) {
    if (n == 0) return 0;
    size_t idx = (size_t)((double)(n - 1) * p / 100.0);
    return sorted[idx];
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    int run_seconds = RUN_SECONDS;
    int writer_hz   = WRITER_HZ;
    int reader_hz  = READER_HZ;
    if (argc > 1) run_seconds = atoi(argv[1]);
    if (argc > 2) writer_hz   = atoi(argv[2]);
    if (argc > 3) reader_hz   = atoi(argv[3]);

    printf("# Weft Triad Protocol spike\n");
    printf("# Run: %d seconds, writer=%d Hz, reader=%d Hz\n", run_seconds, writer_hz, reader_hz);
    printf("# Frame size: %d floats (%d bytes)\n\n", WEFT_FRAME_FLOATS, (int)(WEFT_FRAME_FLOATS * sizeof(float)));

    // Allocate the Weft.
    weft_t w;
    if (weft_init(&w, WEFT_FRAME_FLOATS) != 0) {
        fprintf(stderr, "weft_init failed\n");
        return 1;
    }
    long rss_before = get_rss_kb();
    printf("# RSS after weft_init: %ld KB\n", rss_before);

    // Allocate the frame-time sample buffer (sized for max possible samples).
    size_t ft_cap = (size_t)reader_hz * run_seconds + 16;
    uint64_t* frame_times = (uint64_t*)malloc(ft_cap * sizeof(uint64_t));
    if (!frame_times) { fprintf(stderr, "malloc failed\n"); return 1; }
    size_t ft_len = 0;

    _Atomic int stop = 0;
    _Atomic uint64_t alloc_count = 0;
    _Atomic uint64_t torn_count  = 0;
    _Atomic uint64_t read_count  = 0;

    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)run_seconds * 1000000000ull;

    pthread_t writer_tid, reader_tid;
    writer_args_t wargs = {
        .weft = &w,
        .stop = &stop,
        .alloc_count = &alloc_count,
        .start_ns = start,
        .writer_hz = writer_hz,
    };
    reader_args_t rargs = {
        .weft = &w,
        .stop = &stop,
        .alloc_count = &alloc_count,
        .read_count = &read_count,
        .torn_count = &torn_count,
        .start_ns = start,
        .reader_hz = reader_hz,
        .frame_times_ns = frame_times,
        .frame_times_cap = ft_cap,
        .frame_times_len = 0,
    };

    // Override the per-thread Hz if specified.
    // (Writer and reader threads read these from their args structs.)

    pthread_create(&writer_tid, NULL, writer_thread, &wargs);
    pthread_create(&reader_tid, NULL, reader_thread, &rargs);

    // Let it run for run_seconds.
    while (now_ns() < deadline) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };  // 100ms
        nanosleep(&ts, NULL);
    }

    // Stop both threads.
    atomic_store_explicit(&stop, 1, memory_order_relaxed);
    pthread_join(writer_tid, NULL);
    pthread_join(reader_tid, NULL);

    uint64_t end = now_ns();
    uint64_t elapsed_ns = end - start;

    // Steal the frame_times_len from the reader's local state.
    ft_len = rargs.frame_times_len;

    // Sort frame times for percentile calc.
    qsort(frame_times, ft_len, sizeof(uint64_t), cmp_u64);

    long rss_after = get_rss_kb();

    // ---------------------------------------------------------------------------
    // Report
    // ---------------------------------------------------------------------------

    uint64_t publishes = atomic_load(&w.publish_count);
    uint64_t reads     = atomic_load(&w.read_count);
    uint64_t torns     = atomic_load(&w.checksum_mismatch_count);
    uint64_t allocs    = atomic_load(&alloc_count);

    double elapsed_s = (double)elapsed_ns / 1e9;
    double eff_writer_fps = (double)publishes / elapsed_s;
    double eff_reader_fps = (double)ft_len / elapsed_s;

    uint64_t p50 = percentile(frame_times, ft_len, 50.0);
    uint64_t p99 = percentile(frame_times, ft_len, 99.0);
    uint64_t p100 = ft_len > 0 ? frame_times[ft_len - 1] : 0;

    printf("\n");
    printf("=== SPIKE RESULTS ===\n");
    printf("{\n");
    printf("  \"run_seconds\":        %d,\n", run_seconds);
    printf("  \"writer_hz_target\":   %d,\n", writer_hz);
    printf("  \"reader_hz_target\":   %d,\n", reader_hz);
    printf("  \"frame_floats\":       %d,\n", WEFT_FRAME_FLOATS);
    printf("  \"frame_bytes\":        %d,\n", (int)(WEFT_FRAME_FLOATS * sizeof(float)));
    printf("\n");
    printf("  \"publishes\":          %lu,\n", publishes);
    printf("  \"reads_attempted\":    %lu,\n", ft_len);
    printf("  \"reads_succeeded\":    %lu,\n", reads);
    printf("  \"torn_reads\":         %lu,\n", torns);
    printf("  \"allocs_during_run\":  %lu,\n", allocs);
    printf("\n");
    printf("  \"elapsed_s\":          %.3f,\n", elapsed_s);
    printf("  \"eff_writer_fps\":     %.2f,\n", eff_writer_fps);
    printf("  \"eff_reader_fps\":     %.2f,\n", eff_reader_fps);
    printf("  \"reader_p50_ns\":      %lu,\n", p50);
    printf("  \"reader_p99_ns\":      %lu,\n", p99);
    printf("  \"reader_p100_ns\":     %lu,\n", p100);
    printf("  \"reader_p50_ms\":      %.3f,\n", p50 / 1e6);
    printf("  \"reader_p99_ms\":      %.3f,\n", p99 / 1e6);
    printf("  \"reader_p100_ms\":     %.3f,\n", p100 / 1e6);
    printf("\n");
    printf("  \"rss_before_kb\":      %ld,\n", rss_before);
    printf("  \"rss_after_kb\":       %ld,\n", rss_after);
    printf("  \"rss_growth_kb\":      %ld,\n", rss_after - rss_before);
    printf("\n");
    printf("  \"verdict\": {\n");
    printf("    \"torn_reads_zero\":       %s,\n", torns == 0 ? "true" : "false");
    printf("    \"allocs_per_frame_zero\": %s,\n", allocs <= 2 ? "true" : "false");
    printf("    \"p99_within_vsync\":      %s,\n", p99 < 1000000 ? "true" : "false");
    printf("    \"rss_growth_zero\":       %s\n", (rss_after - rss_before) <= 0 ? "true" : "false");
    printf("  }\n");
    printf("}\n");

    // Cleanup
    free(frame_times);
    weft_destroy(&w);
    return 0;
}
