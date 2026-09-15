// weft.h — Triad Protocol public API
//
// This is the spike implementation of the Weft Triad Protocol, written in C11
// with pthreads and stdatomic.h. It is a faithful model of the production
// Rust + Kotlin/JNI implementation: same atomics, same memory model, same
// buffer rotation, same invariants.
//
// The spike exists to de-risk the only unknown that can kill the Weft
// specification: does the Triad Protocol actually deliver
//   1. Zero torn reads under a 120 Hz writer / 60 Hz reader?
//   2. Zero per-frame heap allocation after warmup?
//   3. Frame pacing locked to the reader's VSYNC rate?
//
// If the spike shows all three, the spec is sound and the Android v0.1
// library can be built with confidence. If any fails, the spec needs revision
// before any platform implementation begins.

#ifndef WEFT_H
#define WEFT_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

// Frame format for the spike: 1024 floats per buffer.
//   [0]      : sequence number (writer-incremented)
//   [1..N-2] : payload (deterministic pattern, so the reader can verify integrity)
//   [N-1]    : checksum (sum of payload floats, as uint32_t reinterpretation)
#define WEFT_FRAME_FLOATS 1024

// A Weft is three off-heap buffers + two atomics + a writer-private index.
typedef struct {
    // Three off-heap buffers, 16-byte aligned, never move.
    float* buffers[3];

    // Index of the most-recently-published buffer. -1 = no data yet.
    // Writer: Release store. Reader: Acquire load.
    _Atomic int32_t latest;

    // Index currently held by the reader. -1 = no reader.
    // Reader: CAS -1 → idx (Acquire), then store -1 (Release).
    // Writer: Relaxed load (used only to exclude from candidates).
    _Atomic int32_t claimed;

    // Writer-private working index. Not shared; no synchronization needed.
    int writer_idx;

    // Capacity in floats (== WEFT_FRAME_FLOATS, but stored per-instance for generality).
    size_t capacity;

    // Telemetry counters (Relaxed atomic — telemetry only, not synchronization).
    _Atomic uint64_t publish_count;
    _Atomic uint64_t read_count;
    _Atomic uint64_t torn_read_count;       // should remain 0
    _Atomic uint64_t checksum_mismatch_count; // should remain 0
    _Atomic uint64_t stale_read_count;      // reader saw same frame twice in a row (allowed, latest-wins)
} weft_t;

// Allocate a Weft with the given capacity (in floats). Buffers are 16-byte
// aligned off-heap. Returns 0 on success, -1 on allocation failure.
int weft_init(weft_t* w, size_t capacity);

// Free a Weft. Safe to call after the writer and reader have stopped.
void weft_destroy(weft_t* w);

// Writer: publish a frame of `capacity` floats.
// The writer writes to its private buffer, then atomically publishes it.
// Wait-free: O(1), no spin, no retry.
void weft_publish(weft_t* w, const float* frame_data);

// Reader: attempt to read the latest frame.
// Returns:
//   0  — no new data since the last successful read (latest unchanged or empty)
//   1  — successfully claimed and read a frame; out_buf has the snapshot
//  -1  — internal error (should never happen with the Triad Protocol)
//
// The reader is wait-free: single Acquire load + single CAS + single Release store.
int weft_read(weft_t* w, float* out_buf, size_t capacity);

// Compute the checksum of a frame for torn-read / corruption detection.
// The first float is the sequence number; the last is the expected checksum.
// This function recomputes the payload checksum and compares.
// Returns 0 if valid, -1 if mismatch (torn read or corruption).
int weft_frame_verify(const float* frame, size_t capacity);

#endif // WEFT_H
