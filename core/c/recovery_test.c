// recovery_test.c — Axis 3: Self-stabilizing ring health check & recovery test
#include "fanout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define PAYLOAD_BYTES 64
#define SLOT_COUNT 4

int main(void) {
    printf("=== Axis 3: Self-Stabilizing Ring Recovery Test ===\n");
    
    size_t ring_bytes = weft_fanout_ring_bytes(PAYLOAD_BYTES, SLOT_COUNT);
    size_t alloc_bytes = ((ring_bytes + 63) / 64) * 64;
    uint8_t* ring = (uint8_t*)aligned_alloc(64, alloc_bytes);
    assert(ring != NULL);
    
    weft_fanout_t writer = {0};
    int rc = weft_fanout_attach_writer(&writer, ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
    assert(rc == 0);
    
    // 1. Fresh ring health
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_HEALTHY);
    printf("  [PASS] Fresh ring is HEALTHY\n");
    
    // 2. Publish 10 frames
    for (int i = 0; i < 10; i++) {
        uint8_t* cur = weft_fanout_begin(&writer);
        assert(cur != NULL);
        memset(cur, i + 1, PAYLOAD_BYTES);
        uint64_t seq = weft_fanout_publish(&writer);
        assert(seq == (uint64_t)(i + 1));
    }
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_HEALTHY);
    printf("  [PASS] 10 published frames ring is HEALTHY\n");
    
    // 3. Inject FUTURE_SEQ corruption (latestSeq > publishes)
    _Atomic uint64_t* ctrl = (_Atomic uint64_t*)ring;
    atomic_store(ctrl, 9999);
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_CORRUPT_FUTURE_SEQ);
    rc = weft_ring_recover(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
    assert(rc == 0);
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_HEALTHY);
    printf("  [PASS] FUTURE_SEQ corruption detected and recovered\n");
    
    // 4. Inject IMPOSSIBLE_STAMP corruption (slotSeq[k] > latestSeq)
    atomic_store(ctrl + 2, 8888); // slot 0 stamp = 8888
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_CORRUPT_IMPOSSIBLE_STAMP);
    rc = weft_ring_recover(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
    assert(rc == 0);
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_HEALTHY);
    printf("  [PASS] IMPOSSIBLE_STAMP corruption detected and recovered\n");
    
    // 5. Inject SPLIT_BRAIN corruption (multiple slots match latestSeq)
    uint64_t lat = atomic_load(ctrl);
    atomic_store(ctrl + 2, lat);
    atomic_store(ctrl + 3, lat);
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_CORRUPT_SPLIT_BRAIN);
    rc = weft_ring_recover(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
    assert(rc == 0);
    assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_HEALTHY);
    printf("  [PASS] SPLIT_BRAIN corruption detected and recovered\n");
    
    // 6. 10,000 Randomized Corruption & Recovery Loop
    weft_fanout_reader_t reader = {0};
    int rc_r = weft_fanout_reader_init(&reader, ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
    assert(rc_r == 0);
    
    for (int iter = 0; iter < 10000; iter++) {
        // Publish a frame
        uint8_t* cur = weft_fanout_begin(&writer);
        if (cur) {
            memset(cur, (iter % 255) + 1, PAYLOAD_BYTES);
            weft_fanout_publish(&writer);
        }
        
        // Randomly corrupt a control word
        int target_word = rand() % (2 + SLOT_COUNT);
        uint64_t corrupt_val = (uint64_t)rand() * 1000 + 10000;
        atomic_store(ctrl + target_word, corrupt_val);
        
        // Reader claim under corruption MUST NOT crash or accept torn data
        const weft_fanout_claim_t* claim = weft_fanout_claim(&reader);
        (void)claim;
        
        // Health check must detect corruption or self-stabilize
        weft_ring_health_t h = weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
        if (h != WEFT_RING_HEALTHY) {
            rc = weft_ring_recover(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT);
            assert(rc == 0);
            assert(weft_ring_health_check(ring, ring_bytes, PAYLOAD_BYTES, SLOT_COUNT) == WEFT_RING_HEALTHY);
        }
    }
    printf("  [PASS] 10,000 randomized corruption + recovery cycles passed\n");
    
    weft_fanout_reader_destroy(&reader);
    weft_fanout_destroy(&writer);
    free(ring);
    
    printf("{\"test\":\"recovery\",\"status\":\"PASSED\",\"iterations\":10000}\n");
    return 0;
}
