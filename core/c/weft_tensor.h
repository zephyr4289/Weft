// weft_tensor.h — RFC-0016 §5: the zero-alloc AI/tensor stream bridge
// (C driver layer).
//
// WHY EXISTS: the lead's mandate — "AI model inference outputs (audio
// tokens, embedding vectors, video embeddings) published directly into
// Weft rings for real-time visualization and downstream pipelines." AI
// runtimes (llama.cpp/GGML, ONNX Runtime, Whisper) emit dense typed
// tensors; today every consumer invents its own payload framing, so
// nothing downstream can be generic. This module defines WTS1 — ONE
// in-band tensor frame format carried as an ordinary fan-out frame —
// plus the zero-alloc publisher/parse paths around it:
//
//   ┌──────────────── Weft frame payload (slot, under the FI1 bracket) ─┐
//   │ WTS1 header (32 B) ── dtype, rank, dims, elem_count ── payload    │
//   └───────────────────────────────────────────────────────────────────┘
//
// THE ADAPTER SEAMS (one memcpy into the slot, maximum):
//   llama.cpp / GGML  : token streams   = publish_tokens (U32 ids — the
//                       decode step's output, zero transforms)
//                       embeddings      = publish_f16 (F32 -> F16 quantized
//                       through the SAME weft_f16_codec the device-tier
//                       layer ships — one quantization dialect tree-wide)
//   ONNX Runtime      : float tensors   = publish_audio_f32 / fill_bytes
//                       (the runtime's output buffer -> slot, one copy)
//   Whisper           : PCM chunks      = publish_audio_f32; token logits
//                       -> publish_f16
//   Zero-copy upper   : an engine whose arena IS a ring slot (GGML custom
//   rung               allocators) writes through the raw cursor from
//                       weft_tensor_frame_begin — no copy at all (the
//                       RFC's documented integration point)
//
// FORMAT: WTS1, little-endian, 32-byte header + payload:
//   offset  0  magic "WTS1" (u32 LE = 0x31535457)
//   offset  4  version (u8, =1) | dtype (u8) | rank (u8, 1..4) | flags (u8, =0)
//   offset  8  elem_count (u32)
//   offset 12  payload_words (u32) — u32 words AFTER the header
//   offset 16  dim[0..3] (u32 each; dims >= rank must be 0)
//   offset 32  payload — elem_count elements of dtype, tail zero-padded
//              to a u32 multiple (the fan-out ring's word granularity)
// Invariants: prod(dims[0..rank)) == elem_count; payload_words ==
// ceil(elem_count * elem_size / 4); flags==0 (unknown bits reject on
// parse — the version discipline every Weft wire format follows).
//
// LAW 2: every publish path allocates nothing and performs at most ONE
//        copy (the engine buffer -> slot); the parse path is read-only.
// LAW 3: driver layer; weft.c/weft.h untouched; composes with every
//        existing surface (frames are ordinary fan-out frames — the
//        flight recorder, shm sessions, GPU wraps and xdp ingestion all
//        carry them unchanged).
// LAW 4: geometry that does not fit the slot is refused BEFORE begin()
//        (no burned seq — unlike uring_rx's kernel path, we can see the
//        size first); every parse anomaly is a refusal, never a guess.

#ifndef WEFT_TENSOR_H
#define WEFT_TENSOR_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// "WTS1" little-endian.
#define WEFT_TENSOR_MAGIC 0x31535457u

/// WTS1 header size in bytes (payload follows).
#define WEFT_TENSOR_HEADER_BYTES 32u

typedef enum {
    WEFT_TENSOR_U8  = 0,
    WEFT_TENSOR_U16 = 1,
    WEFT_TENSOR_U32 = 2,
    WEFT_TENSOR_U64 = 3,
    WEFT_TENSOR_I8  = 4,
    WEFT_TENSOR_I16 = 5,
    WEFT_TENSOR_I32 = 6,
    WEFT_TENSOR_I64 = 7,
    WEFT_TENSOR_F16 = 8,   ///< IEEE 754 binary16, the weft_f16_codec dialect
    WEFT_TENSOR_F32 = 9,
    WEFT_TENSOR_F64 = 10,
} weft_tensor_dtype_t;

/// The parsed WTS1 header (validated by weft_tensor_frame_parse).
typedef struct {
    uint8_t version;        ///< 1
    uint8_t dtype;          ///< weft_tensor_dtype_t
    uint8_t rank;           ///< 1..4
    uint8_t flags;          ///< 0 (reserved — unknown bits reject)
    uint32_t elem_count;    ///< prod(dims[0..rank))
    uint32_t payload_words; ///< u32 words after the header
    uint32_t dims[4];       ///< dims[k]=0 for k >= rank
} weft_tensor_hdr_t;

/// Element size in bytes for a dtype (0 for invalid dtypes).
size_t weft_tensor_elem_size(weft_tensor_dtype_t t);

/// Dtype name ("u8".."f64") for logs and evidence.
const char* weft_tensor_dtype_name(weft_tensor_dtype_t t);

// ---------------------------------------------------------------------------
// Publisher (zero-alloc; at most one copy per frame)
// ---------------------------------------------------------------------------

/// Begin a tensor frame and write its header into the slot: dims/rank/
/// dtype/elem_count fully determine the header (the payload cursor is
/// returned for the fill calls). Returns the payload cursor, or NULL
/// when the geometry cannot fit the slot (refused BEFORE any bracket —
/// no seq is burned; Law 4). The frame closes with the ordinary
/// weft_fanout_publish().
uint8_t* weft_tensor_frame_begin(weft_fanout_t* f, weft_tensor_dtype_t dt,
                                 const uint32_t* dims, uint8_t rank);

/// Fill the payload with raw bytes (one copy — the ONNX/general seam).
/// n must equal the header's payload bytes (elem_count * elem_size,
/// rounded UP to a u32 multiple — the tail is zeroed to keep the ring's
/// word granularity clean). Returns 0/-1.
int weft_tensor_fill_bytes(uint8_t* payload_cursor, const void* src, size_t n);

/// Fill the payload with an F32 vector quantized to F16 through the
/// weft_f16_codec dialect (the embeddings seam — llama.cpp/ONNX output,
/// one transform+copy, bit-identical to weft_f32_to_f16 per element).
/// n is the element count. Returns 0/-1.
int weft_tensor_fill_f32_as_f16(uint8_t* payload_cursor, const float* src,
                                uint32_t n);

/// Publish a complete F16 embedding vector as one frame (begin + fill +
/// publish in one call — the common seam). Returns the frame seq, 0 on
/// refusal (geometry / NULL).
uint64_t weft_tensor_publish_f16(weft_fanout_t* f, const float* vec, uint32_t n);

/// Publish a U32 token stream as one frame (the llama.cpp decode seam —
/// a plain word copy, zero transforms). Returns the frame seq, 0 on refusal.
uint64_t weft_tensor_publish_tokens(weft_fanout_t* f, const uint32_t* tokens,
                                    uint32_t n);

/// Publish an F32 PCM chunk as one frame (the Whisper/audio seam).
/// Returns the frame seq, 0 on refusal.
uint64_t weft_tensor_publish_audio_f32(weft_fanout_t* f, const float* pcm,
                                       uint32_t n);

// ---------------------------------------------------------------------------
// Consumer (read-only; validates everything, guesses nothing)
// ---------------------------------------------------------------------------

/// Parse and validate a WTS1 frame. `len` is the claimed frame's payload
/// extent (payload_bytes of the ring). On success fills *out_hdr and
/// *out_payload (pointing INSIDE the frame buffer — valid until the next
/// claim). Returns 0; -1 on any anomaly (magic, version, flags, dtype,
/// rank, dims product, payload extent). Never reads past len.
int weft_tensor_frame_parse(const void* frame, size_t len,
                            weft_tensor_hdr_t* out_hdr,
                            const void** out_payload);

/// Read element i of a parsed F16 payload as F32 (the codec dialect).
float weft_tensor_f16_at(const void* payload, uint32_t i);

/// Read element i of a parsed U32 payload.
uint32_t weft_tensor_u32_at(const void* payload, uint32_t i);

/// Read element i of a parsed F32 payload.
float weft_tensor_f32_at(const void* payload, uint32_t i);

#ifdef __cplusplus
}
#endif

#endif // WEFT_TENSOR_H
