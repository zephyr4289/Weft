// weft.c — Triad Protocol implementation
//
// Faithful C11 + pthreads + stdatomic model of the production Rust + Kotlin/JNI
// implementation. Same atomics, same memory model, same invariants.

#include "weft.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

int weft_init(weft_t* w, size_t capacity) {
    memset(w, 0, sizeof(*w));
    w->capacity = capacity;
    w->writer_idx = 0;

    // Allocate three off-heap buffers, 16-byte aligned.
    // posix_memalign returns zeroed memory; we'll fill it with the writer.
    for (int i = 0; i < 3; i++) {
        // posix_memalign requires size to be a multiple of alignment.
        // capacity (1024 floats) * 4 bytes = 4096 bytes; 16-byte alignment is fine.
        int rc = posix_memalign((void**)&w->buffers[i], 16, capacity * sizeof(float));
        if (rc != 0 || w->buffers[i] == NULL) {
            // Clean up what we have and bail.
            for (int j = 0; j < i; j++) { free(w->buffers[j]); w->buffers[j] = NULL; }
            return -1;
        }
        memset(w->buffers[i], 0, capacity * sizeof(float));
    }

    atomic_init(&w->latest, (int32_t)-1);
    atomic_init(&w->claimed, (int32_t)-1);
    atomic_init(&w->publish_count, (uint64_t)0);
    atomic_init(&w->read_count, (uint64_t)0);
    atomic_init(&w->torn_read_count, (uint64_t)0);
    atomic_init(&w->checksum_mismatch_count, (uint64_t)0);
    atomic_init(&w->stale_read_count, (uint64_t)0);

    return 0;
}

void weft_destroy(weft_t* w) {
    for (int i = 0; i < 3; i++) {
        free(w->buffers[i]);
        w->buffers[i] = NULL;
    }
}

void weft_publish(weft_t* w, const float* frame_data) {
    // 1. Pick a target buffer: NOT the latest published, NOT the reader's claim.
    //    |candidates| >= 1 always (3 buffers, 2 excluded).
    int32_t latest_now  = atomic_load_explicit(&w->latest,  memory_order_relaxed);
    int32_t claimed_now = atomic_load_explicit(&w->claimed, memory_order_relaxed);

    int next = -1;
    for (int i = 0; i < 3; i++) {
        if (i != latest_now && i != claimed_now) {
            next = i;
            break;
        }
    }
    // With 3 buffers and 2 excluded, next is always found.
    // (If latest == -1 and claimed == -1, all three are candidates; we pick 0.)
    // (If latest == claimed (shouldn't happen, but defensive), we still pick any other.)
    if (next == -1) {
        // Edge case: latest == claimed. Pick the third buffer.
        for (int i = 0; i < 3; i++) {
            if (i != latest_now) { next = i; break; }
        }
    }
    if (next == -1) next = 0; // last resort — should never happen

    // 2. Write to buffers[next]. Writer-private; no synchronization needed.
    //    The reader cannot be reading this buffer (it's neither latest nor claimed).
    memcpy(w->buffers[next], frame_data, w->capacity * sizeof(float));

    // 3. Publish with Release semantics.
    //    Reader's Acquire load on `latest` will see the write.
    atomic_store_explicit(&w->latest, next, memory_order_release);

    // 4. Update writer_idx for the next publish.
    //    Old latest (the buffer the reader might have just released) becomes the
    //    next writer target. If old latest was -1, keep our current writer_idx
    //    (the buffer we just wrote to is now latest; we need a new writer buffer).
    if (latest_now == -1) {
        // First publish. Our just-written buffer is now latest. We need to pick
        // a new writer buffer that is NOT latest and NOT claimed.
        // Since claimed is -1, any of the other two will do.
        // Pick the smallest index that isn't `next`.
        for (int i = 0; i < 3; i++) {
            if (i != next) { w->writer_idx = i; break; }
        }
    } else {
        w->writer_idx = latest_now;  // the old latest is now free
    }

    atomic_fetch_add_explicit(&w->publish_count, 1, memory_order_relaxed);
}

int weft_read(weft_t* w, float* out_buf, size_t capacity) {
    // 1. Acquire-load the latest index.
    int32_t idx = atomic_load_explicit(&w->latest, memory_order_acquire);
    if (idx == -1) {
        // No data yet.
        return 0;
    }

    // 2. Claim via CAS. If another reader already claimed, return 0.
    int32_t expected = -1;
    if (!atomic_compare_exchange_strong_explicit(
            &w->claimed,
            &expected,
            idx,
            memory_order_acquire,   // success: we now hold the buffer
            memory_order_relaxed)) { // failure: someone else has it
        // Another reader got it. For single-reader Wefts, this never happens.
        return 0;
    }

    // 3. Snapshot the buffer. The writer cannot touch buffers[idx] because:
    //    - it is currently `latest`, so writer excludes it
    //    - it is currently `claimed`, so writer excludes it
    //    Both conditions hold until we release.
    memcpy(out_buf, w->buffers[idx], capacity * sizeof(float));

    // 4. Release the claim.
    atomic_store_explicit(&w->claimed, (int32_t)-1, memory_order_release);

    // 5. Verify integrity (torn-read detection).
    if (weft_frame_verify(out_buf, capacity) != 0) {
        atomic_fetch_add_explicit(&w->checksum_mismatch_count, 1, memory_order_relaxed);
        // Still counts as a "read" but flagged
        atomic_fetch_add_explicit(&w->read_count, 1, memory_order_relaxed);
        return -1;
    }

    atomic_fetch_add_explicit(&w->read_count, 1, memory_order_relaxed);
    return 1;
}

int weft_frame_verify(const float* frame, size_t capacity) {
    // Layout: [0] = seq, [1..N-2] = payload, [N-1] = checksum
    if (capacity < 3) return -1;

    uint32_t seq = (uint32_t)frame[0];

    // Recompute checksum: sum of payload floats reinterpreted as uint32.
    // Using uint32 reinterpretation avoids float precision issues.
    uint32_t computed = 0;
    for (size_t i = 1; i < capacity - 1; i++) {
        uint32_t bits;
        memcpy(&bits, &frame[i], sizeof(uint32_t));
        computed += bits;
    }
    // Mix in the seq for stronger detection.
    computed ^= seq * 0x9E3779B1u;

    uint32_t stored;
    memcpy(&stored, &frame[capacity - 1], sizeof(uint32_t));

    return (computed == stored) ? 0 : -1;
}
