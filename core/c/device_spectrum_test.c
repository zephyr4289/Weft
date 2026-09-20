// device_spectrum_test.c — Conformance test suite for Device-Spectrum Optimizations (doc-007)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "weft_device_profile.h"
#include "weft_f16_codec.h"
#include "weft_render_bridge.h"
#include "fanout.h"

static void test_device_profile(void) {
    printf("[TEST] Device profile probe & recommendations...\n");

    weft_device_profile_t prof = weft_device_probe();
    assert(prof.tier >= WEFT_DEVICE_TIER_LOW && prof.tier <= WEFT_DEVICE_TIER_HIGH);
    assert(prof.cache_line >= 64u);
    assert(prof.perf_cores + prof.eff_cores >= 1u);
    assert(prof.thermal_budget > 0u);

    // Test tier geometry tables
    weft_device_profile_t low_p = { .tier = WEFT_DEVICE_TIER_LOW, .cache_line = 64 };
    assert(weft_device_recommended_slot_count(&low_p) == 8u);
    assert(weft_device_recommended_alignment(&low_p) == 128u);
    assert(weft_device_recommended_claim_cadence_divisor(&low_p) == 2u);
    assert(weft_device_recommended_governor_cooldown(&low_p) == 500u);
    assert(weft_device_recommended_prefetch_dist(&low_p) == 0u);

    weft_device_profile_t mid_p = { .tier = WEFT_DEVICE_TIER_MID, .cache_line = 64 };
    assert(weft_device_recommended_slot_count(&mid_p) == 4u);
    assert(weft_device_recommended_alignment(&mid_p) == 64u);
    assert(weft_device_recommended_claim_cadence_divisor(&mid_p) == 1u);
    assert(weft_device_recommended_governor_cooldown(&mid_p) == 250u);
    assert(weft_device_recommended_prefetch_dist(&mid_p) == 128u);

    weft_device_profile_t high_p = { .tier = WEFT_DEVICE_TIER_HIGH, .cache_line = 128 };
    assert(weft_device_recommended_slot_count(&high_p) == 4u);
    assert(weft_device_recommended_alignment(&high_p) == 128u);
    assert(weft_device_recommended_claim_cadence_divisor(&high_p) == 1u);
    assert(weft_device_recommended_governor_cooldown(&high_p) == 150u);
    assert(weft_device_recommended_prefetch_dist(&high_p) == 256u);

    printf("  [PASS] Device profile heuristics validated (tier=%d, cores=%u/%u, cache=%uB)\n",
           prof.tier, prof.perf_cores, prof.eff_cores, prof.cache_line);
}

static void test_f16_codec_accuracy(void) {
    printf("[TEST] Float16 codec accuracy and special values...\n");

    float test_vals[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 100.25f, 65504.0f, 0.00006103515625f };
    size_t num_vals = sizeof(test_vals) / sizeof(test_vals[0]);

    for (size_t i = 0; i < num_vals; i++) {
        float original = test_vals[i];
        uint16_t h = weft_f32_to_f16(original);
        float recovered = weft_f16_to_f32(h);

        if (original == 0.0f) {
            assert(recovered == 0.0f);
        } else {
            float rel_err = fabsf((recovered - original) / original);
            assert(rel_err < 0.002f); // Half precision is 11 bits of mantissa (~0.1% precision)
        }
    }

    // Special value: Infinity
    float inf = 1.0f / 0.0f;
    uint16_t h_inf = weft_f32_to_f16(inf);
    float rec_inf = weft_f16_to_f32(h_inf);
    assert(isinf(rec_inf));

    // Special value: NaN
    float nan_val = 0.0f / 0.0f;
    uint16_t h_nan = weft_f32_to_f16(nan_val);
    float rec_nan = weft_f16_to_f32(h_nan);
    assert(isnan(rec_nan));

    printf("  [PASS] Float16 codec precision bounds verified across full range.\n");
}

static void test_f16_ring_integration(void) {
    printf("[TEST] Float16 ring publish & claim integration...\n");

    const unsigned slot_count = 4;
    const unsigned float_count = 64;
    const unsigned payload_bytes = float_count * sizeof(uint16_t); // Half-precision storage (128 bytes, mult of 4)
    size_t ring_bytes = weft_fanout_ring_bytes(payload_bytes, slot_count);

    uint8_t* ring_mem = (uint8_t*)calloc(1, ring_bytes);
    assert(ring_mem != NULL);

    weft_fanout_t writer;
    memset(&writer, 0, sizeof(writer));
    int rc = weft_fanout_attach_writer(&writer, ring_mem, ring_bytes, payload_bytes, slot_count);
    assert(rc == 0);

    weft_fanout_reader_t reader;
    rc = weft_fanout_reader_init(&reader, ring_mem, ring_bytes, payload_bytes, slot_count);
    assert(rc == 0);

    // Publish f32 frames quantized to f16
    float sample_data[64];
    for (int i = 0; i < 64; i++) {
        sample_data[i] = (float)i * 1.5f;
    }

    weft_ring_publish_f16(&writer, sample_data, 64);

    // Claim f16 and dequantize
    float received_data[64];
    uint64_t seq = 0;
    int claimed = weft_ring_claim_f16(&reader, received_data, 64, &seq);
    assert(claimed == 1);
    assert(seq == 1);

    for (int i = 0; i < 64; i++) {
        float rel_err = fabsf((received_data[i] - sample_data[i]) / (sample_data[i] == 0.0f ? 1.0f : sample_data[i]));
        assert(rel_err < 0.002f);
    }

    weft_fanout_reader_destroy(&reader);
    free(ring_mem);
    printf("  [PASS] Float16 ring publish/claim end-to-end verified.\n");
}

static void test_dirty_region_and_skipping(void) {
    printf("[TEST] Dirty-region row mask & adaptive skipping...\n");

    const int rows = 16;
    const int cols = 32;
    float prev[16 * 32];
    float curr[16 * 32];
    float dst[16 * 32];

    for (int i = 0; i < rows * cols; i++) {
        prev[i] = 1.0f;
        curr[i] = 1.0f;
        dst[i] = 0.0f;
    }

    // Mutate row 2 and row 9
    curr[2 * cols + 5] = 5.0f;
    curr[9 * cols + 10] = 9.0f;

    uint64_t mask = weft_dirty_mask_compute(prev, curr, rows, cols, 0.001f);
    uint64_t expected_mask = (1ULL << 2) | (1ULL << 9);
    assert(mask == expected_mask);

    int copied = weft_dirty_copy_f32(dst, curr, mask, rows, cols);
    assert(copied == 2);
    assert(dst[2 * cols + 5] == 5.0f);
    assert(dst[9 * cols + 10] == 9.0f);
    assert(dst[0] == 0.0f); // Unmodified rows not copied

    // Test adaptive frame skipping
    assert(weft_adaptive_should_skip_publish(1, 4) == 0);
    assert(weft_adaptive_should_skip_publish(4, 4) == 0);
    assert(weft_adaptive_should_skip_publish(5, 4) == 1);

    printf("  [PASS] Dirty-region 64-bit mask and adaptive skipping verified.\n");
}

static void test_render_bridge(void) {
    printf("[TEST] Render bridge lifecycle & present...\n");

    const unsigned slot_count = 4;
    const unsigned payload_bytes = 256;
    size_t ring_bytes = weft_fanout_ring_bytes(payload_bytes, slot_count);

    uint8_t* ring_mem = (uint8_t*)calloc(1, ring_bytes);
    assert(ring_mem != NULL);

    weft_fanout_t ring;
    memset(&ring, 0, sizeof(ring));
    int rc = weft_fanout_attach_writer(&ring, ring_mem, ring_bytes, payload_bytes, slot_count);
    assert(rc == 0);

    weft_render_bridge_t* bridge = NULL;
    rc = weft_render_bridge_create(&bridge, &ring, WEFT_RENDER_BACKEND_METAL);
    assert(rc == 0);
    assert(bridge != NULL);
    assert(weft_render_bridge_get_backend(bridge) == WEFT_RENDER_BACKEND_METAL);
    assert(bridge->is_direct_mapped == 1);

    // Publish frame 1
    void* wptr = weft_fanout_begin(&ring);
    memset(wptr, 0xAB, payload_bytes);
    weft_fanout_publish(&ring);

    // Bind and present
    weft_render_bridge_bind(bridge);
    assert(bridge->bind_count == 1);
    assert(bridge->last_seq_bound == 1);

    weft_render_bridge_present(bridge);
    assert(bridge->present_count == 1);

    // Publish frame 2
    wptr = weft_fanout_begin(&ring);
    memset(wptr, 0xCD, payload_bytes);
    weft_fanout_publish(&ring);

    weft_render_bridge_bind(bridge);
    assert(bridge->bind_count == 2);
    assert(bridge->last_seq_bound == 2);

    weft_render_bridge_present(bridge);
    assert(bridge->present_count == 2);

    weft_render_bridge_destroy(bridge);
    free(ring_mem);

    printf("  [PASS] Render bridge lifecycle, zero-copy bind, and present verified.\n");
}

int main(void) {
    printf("=== Weft Device-Spectrum Optimizations Test Suite ===\n");
    test_device_profile();
    test_f16_codec_accuracy();
    test_f16_ring_integration();
    test_dirty_region_and_skipping();
    test_render_bridge();
    printf("=== ALL DEVICE-SPECTRUM CONFORMANCE TESTS PASSED ===\n");
    return 0;
}
