// weft_f16_codec.h — Float16 Quantization Codec & Dirty-Region Mask, C driver layer
//
// WHY EXISTS: nano/doc-007.md §5. Reduced bandwidth mode for low-end hardware
// and high-rate telemetry payloads (spectrograms, heatmaps, order books).
// Halves payload memory copy bandwidth (f32 -> f16) and avoids transmitting
// unchanged grid rows via a 64-bit dirty mask.
//
// LAW 3: Mechanism, not policy. The ring protocol remains identical;
// quantization and row masking are purely driver-layer payload codecs.

#ifndef WEFT_F16_CODEC_H
#define WEFT_F16_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "fanout.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Convert standard IEEE 754 float32 to float16 (half-precision).
uint16_t weft_f32_to_f16(float val);

/// Convert IEEE 754 float16 (half-precision) to float32.
float weft_f16_to_f32(uint16_t val);

/// Quantize f32 array -> f16 array directly into the ring's active write slot, then publish.
/// `f->payload_bytes` must be at least `count * sizeof(uint16_t)`.
void weft_ring_publish_f16(weft_fanout_t* f, const float* src, int count);

/// Claim and dequantize a half-precision f16 ring slot back into a float32 destination buffer.
/// Returns 1 on successful fresh claim, 0 on not-fresh or torn copy.
int weft_ring_claim_f16(weft_fanout_reader_t* r, float* dst, int max_count, uint64_t* out_seq);

/// Compute a 64-bit dirty row mask for a 2D grid/spectrogram up to 64 rows.
/// Bit `i` is set if row `i` differs between `prev` and `curr` by more than `epsilon`.
uint64_t weft_dirty_mask_compute(const float* prev, const float* curr, int rows, int cols_per_row, float epsilon);

/// Copy only the dirty rows indicated by `dirty_mask` from `src` to `dst`.
/// Returns the total number of dirty rows copied.
int weft_dirty_copy_f32(float* dst, const float* src, uint64_t dirty_mask, int rows, int cols_per_row);

/// Driver-layer adaptive frame skipping decision when consumer lags behind.
/// Returns 1 if publish should be skipped, 0 if it should proceed.
int weft_adaptive_should_skip_publish(uint32_t reader_frames_behind, uint32_t threshold);

#ifdef __cplusplus
}
#endif

#endif // WEFT_F16_CODEC_H
