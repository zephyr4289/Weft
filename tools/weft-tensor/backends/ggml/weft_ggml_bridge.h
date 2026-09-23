// weft_ggml_bridge.h — RFC-0017 §6: the llama.cpp / ggml zero-copy
// audio & embedding feeder.
//
// WHY EXISTS: the lead's Pillar-2 mandate — "Implement a custom
// ggml_backend_buffer_type that points ggml tensors directly to Weft's
// lock-free ring buffer memory" so "audio streaming chunks (e.g.
// Whisper / Voice LLM) stream straight from microphone DMA into
// transformer attention heads with zero formatting hops." Today every
// audio chunk is copied and reformatted into a fresh ggml_tensor
// allocation per chunk — malloc, format, free, per 20 ms of audio.
//
// THE TWO ROADS:
//   FORMAL (buffer type): a custom ggml_backend_buffer_type — "WEFT_DMA"
//   — whose buffers ARE pre-registered ring spans. Tensor data placement
//   is served by a deterministic first-fit planner (64-byte aligned,
//   zero allocations — Law 1); init_tensor lands tensor->data inside the
//   span; set/get_tensor are bounds-checked in-place aliases. Pinned to
//   the ggml b4312-era backend ABI (GGML_BACKEND_API_VERSION 1 — the
//   era whose public impl header carries the buffer-type structs; the
//   2025 split made them opaque, and the bridge detects + reports that
//   honestly). Runtime-gated by an ABI probe: the loaded libggml's own
//   ggml_nbytes() must agree with the mirror's tensor geometry or the
//   formal road REFUSES (never guesses — Law 4).
//
//   DATA-POINTER (version-robust): the plan layer computes the exact
//   ggml shape (ne[4]/nb[4]/nbytes) + span placement for a view; the
//   caller's ggml tensors get data pointers inside the ring span (the
//   documented view-tensor pattern — works across ggml generations,
//   including the 2025 opaque-handle era).
//
// BOTH roads share the plan layer (pure logic, gated everywhere):
// dtype map (WTS1 -> ggml), ggml geometry order (ne[0] is ggml's
// INNERMOST axis — the reverse of the view's row-major dims), the
// placement planner, and the audio windowing (Whisper-style
// window/hop/overlap with an explicit partial-tail policy).
//
// HONESTY MAP: libggml absent -> the bridge refuses and the battery
// runs the plan layer only (the AC-G gates); the formal road is the
// real-library leg (DECLARED in the sandbox, probed at runtime
// everywhere it ships). Unsigned WTS1 dtypes have NO ggml type —
// refused with a named error (the [FALLBACK-COPY] conversion road is
// the caller's, documented, never silent).

#ifndef WEFT_GGML_BRIDGE_H
#define WEFT_GGML_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "weft/weft_tensor_view.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Dtype map (WTS1 dialect -> ggml type codes, b4312/2025-stable values)
// ---------------------------------------------------------------------------

#define WEFT_GGML_TYPE_F32 0
#define WEFT_GGML_TYPE_F16 1
#define WEFT_GGML_TYPE_I8  24
#define WEFT_GGML_TYPE_I16 25
#define WEFT_GGML_TYPE_I32 26
#define WEFT_GGML_TYPE_I64 27
#define WEFT_GGML_TYPE_F64 28

/// ggml type code for a WTS1 dtype; -1 when ggml has no such type
/// (unsigned ints; STRING-class) — the named refusal, not a guess.
int weft_ggml_type_code(weft_tensor_dtype_t t);

// ---------------------------------------------------------------------------
// Geometry: view -> ggml shape (the plan layer, pure)
// ---------------------------------------------------------------------------

typedef struct {
    int64_t  ne[4];       ///< ggml order: ne[0] = innermost (view dims reversed)
    size_t   nb[4];       ///< byte strides per the ggml recurrence
    int      ggml_type;   ///< WEFT_GGML_TYPE_*
    uint64_t nbytes;      ///< nb[3] * ne[3] (the ggml_nbytes result)
    int      ok;          ///< 1 when the view maps
    weft_tv_err_t err;    ///< the refusal reason when !ok
} weft_ggml_shape_t;

/// Map a view to ggml geometry. PACKED row-major views only (a strided
/// view over foreign memory would need ggml views — the caller's road,
/// documented); big-endian refuses (conversion is the fallback road).
int weft_ggml_shape_of_view(const weft_tensor_view_t* v,
                            weft_ggml_shape_t* out);

// ---------------------------------------------------------------------------
// Placement planner (deterministic, zero-alloc — Law 1)
// ---------------------------------------------------------------------------

#define WEFT_GGML_MAX_SPANS 8

typedef struct {
    uint8_t* base;      ///< the registered span (ring slot / DMA window)
    uint64_t bytes;
    uint64_t used;      ///< the first-fit cursor (epoch-rewindable)
} weft_ggml_span_t;

typedef struct {
    weft_ggml_span_t spans[WEFT_GGML_MAX_SPANS];
    int      n;
    uint64_t align;     ///< 64 (Law 2 — float-vector alignment floor)
    uint64_t placed;    ///< placement count (evidence)
    uint64_t refused;   ///< full-span refusals (counted, never silent)
} weft_ggml_planner_t;

void weft_ggml_planner_init(weft_ggml_planner_t* p);
int  weft_ggml_planner_register(weft_ggml_planner_t* p, void* base,
                                uint64_t bytes);       ///< setup path only
/// First-fit placement: returns the span index and byte offset, or -1
/// when nothing fits (the [FALLBACK-COPY] signal). NEVER allocates.
int weft_ggml_planner_place(weft_ggml_planner_t* p, uint64_t nbytes,
                            int* out_span, uint64_t* out_off);
/// Explicit placement at (span, offset) — the pre-planned road (the
/// ring's slot layout decides; refuses out-of-bounds, never guesses).
int weft_ggml_planner_place_at(weft_ggml_planner_t* p, int span,
                               uint64_t off, uint64_t nbytes);
/// Rewind cursors to the epoch start (the ring's frame boundary —
/// placements restart per publish epoch).
void weft_ggml_planner_reset(weft_ggml_planner_t* p);

// ---------------------------------------------------------------------------
// Audio windowing (Whisper-style chunk feeding, pure logic)
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t offset_elems;  ///< window start (elements, not bytes)
    uint32_t n_elems;       ///< window length (== window, except tail)
    int      partial;       ///< 1 = the tail window (policy-gated)
} weft_ggml_audio_win_t;

/// Plan window/hop feeding over `total_elems` samples. Partial tails
/// (total % hop leaves a short final window) are DROPPED unless
/// allow_partial is set — never padded in secret (Law 4; padding is a
/// caller policy with its own labeled copy). Returns 0 and fills
/// out_wins[0..*out_n); -1 when the window does not fit max_wins or the
/// geometry is empty.
int weft_ggml_audio_plan(uint32_t total_elems, uint32_t window,
                         uint32_t hop, int allow_partial,
                         weft_ggml_audio_win_t* out_wins,
                         uint32_t max_wins, uint32_t* out_n);

// ---------------------------------------------------------------------------
// The runtime bridge (dlopen; the formal road)
// ---------------------------------------------------------------------------

typedef struct weft_ggml_rt weft_ggml_rt_t;

typedef enum {
    WEFT_GGML_ERR_OK = 0,
    WEFT_GGML_ERR_NO_LIB = -1,      ///< no libggml at dlopen time
    WEFT_GGML_ERR_NO_SYMBOLS = -2,  ///< library present, symbols absent
    WEFT_GGML_ERR_ABI_PROBE = -3,   ///< ggml_nbytes disagrees with the
                                    ///< mirror — the formal road REFUSES
    WEFT_GGML_ERR_OPAQUE_ERA = -4,  ///< 2025+ opaque buffer structs (the
                                    ///< data-pointer road is documented)
    WEFT_GGML_ERR_BAD_ARG = -5,
    WEFT_GGML_ERR_PLAN = -6,        ///< placement refused (full spans)
    WEFT_GGML_ERR_VIEW = -7,        ///< view refused the ladder
} weft_ggml_err_t;

const char* weft_ggml_err_name(weft_ggml_err_t e);

/// dlopen libggml(-base/-backend/-cpu), resolve the plan-critical
/// symbols, run the ggml_nbytes ABI probe. The probe's verdict travels
/// with every claim (weft_ggml_rt_report).
weft_ggml_err_t weft_ggml_rt_load(weft_ggml_rt_t** out);

/// One-line evidence report: which library, which symbols, probe verdict,
/// era (formal-road-capable b4312 ABI vs opaque 2025+).
const char* weft_ggml_rt_report(const weft_ggml_rt_t* rt);

void weft_ggml_rt_unload(weft_ggml_rt_t* rt);

/// The formal road (b4312 ABI era only — OPAQUE_ERA refuses otherwise):
/// construct the "WEFT_DMA" ggml_backend_buffer_type bound to the
/// planner. The returned handle is owned by the runtime (valid until
/// unload); pass it to ggml_alloc-era consumers exactly like any other
/// buffer type (ggml_backend_cpu_buffer_type's peer).
void* weft_ggml_buffer_type(weft_ggml_rt_t* rt,
                            weft_ggml_planner_t* planner);

/// The data-pointer road (every era): validate the view, place it in
/// the planner, and return the exact pointer + ggml shape the caller's
/// tensor should carry (tensor->data = *out_data; ne/nb from *out_shape).
weft_ggml_err_t weft_ggml_place_view(weft_ggml_rt_t* rt,
                                     weft_ggml_planner_t* p,
                                     const weft_tensor_view_t* v,
                                     void** out_data,
                                     weft_ggml_shape_t* out_shape);

#ifdef __cplusplus
}
#endif

#endif // WEFT_GGML_BRIDGE_H
