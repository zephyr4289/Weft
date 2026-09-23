// weft_ort_bridge.h — RFC-0017 §5: the zero-copy ONNX Runtime bridge.
//
// WHY EXISTS: the lead's Pillar-2 mandate — a custom OrtMemoryInfo
// provider that "wraps Weft DMA memory into an OrtValue tensor using
// CreateTensorWithDataAsOrtValue()" with "guaranteed 0 heap allocations
// during OrtRun() inference calls." Today applications copy raw sensor
// buffers into intermediate OrtValue or GPU staging buffers, wasting
// 3-10 ms per frame; this bridge makes the DMA ring's OWN bytes the
// tensor:
//
//   weft_tensor_view_t ──wrap──> OrtValue (CreateTensorWithDataAsOrtValue)
//        │                          │
//        │ data pointer ───────────┘ identity (NEVER copied — the API
//        │                             wraps; tests assert pointer
//        │                             identity through GetTensorMutableData)
//        └── output span ──BindOutput──> model writes logits DIRECTLY
//                                        into Weft memory (write-back)
//
// THE POOLED SESSION (Law 1): session options carry DisableMemPattern +
// DisableCpuMemArena + pinned intra-op threads (no growth surprises);
// the IoBinding is created ONCE; input/output OrtValues are wrapped once
// per geometry and RE-USED across RunWithBinding calls. The AC-O gates
// assert the malloc-audit window does not move across a pooled run.
//
// NO LINK-TIME DEPENDENCY (the gpu_ring dlopen rule): libonnxruntime is
// dlopen'd at runtime; the API table is the VERIFIED v1.16.3 mirror
// (weft_ort_abi.h — generated from the vendored header, every used
// field offsetof-asserted at its table index; a runtime older than
// ORT_API_VERSION 16 refuses at load, never guesses).
//
// HONESTY MAP (Law 4): with libonnxruntime absent the bridge refuses
// (WEFT_ORT_ERR_NO_RUNTIME) and callers route to [FALLBACK-COPY] or the
// declared SIM road — the no-runtime leg is itself a CI gate. The mock
// table (tests/mock_ort.h) runs the FULL logic battery (pointer
// identity, option discipline, bind/run call shapes, zero-alloc window)
// without the runtime; the real-runtime leg runs where the library
// exists (fixture: tests/fixtures/weft_fixture_identity.onnx, generated
// by tests/fixtures/gen_model.py — a minimal float32[1,64] Identity
// graph, no external downloads).
//
// GPU/NPU EXECUTION PROVIDERS: the wrapped value carries CPU memory
// info; CPU/CoreML/QNN-NNAPI-CPU EPs consume it in place. A CUDA EP may
// still stage to VRAM (the provider's contract, not the bridge's) — the
// write-back span (BindOutput into Weft memory) stays zero-copy on every
// EP that honors host outputs. Documented, not hidden.

#ifndef WEFT_ORT_BRIDGE_H
#define WEFT_ORT_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "weft/weft_tensor_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_ORT_OK = 0,
    WEFT_ORT_ERR_NO_RUNTIME = -1,  ///< no libonnxruntime.so at dlopen time
    WEFT_ORT_ERR_OLD_RUNTIME = -2, ///< runtime cannot serve ORT_API_VERSION 16
    WEFT_ORT_ERR_BAD_ARG = -3,
    WEFT_ORT_ERR_STATUS = -4,      ///< an ORT call returned a status (the
                                   ///< message is fetchable via
                                   ///< weft_ort_last_error)
    WEFT_ORT_ERR_VIEW = -5,        ///< the tensor view refused the ladder
    WEFT_ORT_ERR_DTYPE = -6,       ///< dtype outside the ONNX map
    WEFT_ORT_ERR_MODEL = -7,       ///< model bytes refused by the runtime
    WEFT_ORT_ERR_BINDING = -8,     ///< IoBinding/name mismatch
} weft_ort_err_t;

const char* weft_ort_err_name(weft_ort_err_t e);

// ---------------------------------------------------------------------------
// The runtime (dlopen'd; or a mock table injected by tests)
// ---------------------------------------------------------------------------

typedef struct weft_ort_runtime weft_ort_runtime_t;

/// dlopen("libonnxruntime.so.1" | ".so"), OrtGetApiBase()->GetApi(16).
/// NULL out on every refusal (named, never silent).
weft_ort_err_t weft_ort_load(weft_ort_runtime_t** out);

/// The runtime's own version string (OrtApiBase::GetVersionString) —
/// "(not loaded)" / "(mock)" otherwise. Evidence lines carry it.
const char* weft_ort_version(const weft_ort_runtime_t* rt);

/// TEST INJECTION POINT: build a runtime over a FAKE api table (the
/// mock's weft_ort_api_t) without dlopen. Real callers use weft_ort_load.
weft_ort_err_t weft_ort_load_table(weft_ort_runtime_t** out,
                                   const void* api_table,
                                   const char* version_label);

/// The last ORT error message seen on this runtime ("" when none).
const char* weft_ort_last_error(const weft_ort_runtime_t* rt);

/// The live API table pointer (mock or real) — the test battery's
/// direct access point for table-level verification (e.g. calling
/// GetTensorMutableData through the very table the bridge uses).
const void* weft_ort_raw_table(const weft_ort_runtime_t* rt);

void weft_ort_unload(weft_ort_runtime_t* rt);

// ---------------------------------------------------------------------------
// The pooled session (Law 1 — everything here is created once)
// ---------------------------------------------------------------------------

typedef struct weft_ort_session weft_ort_session_t;

/// Open a session over model bytes: options get DisableMemPattern +
/// DisableCpuMemArena + intra-op pinned to 1 (the deterministic pooling
/// profile — callers with their own threading policy can re-open with
/// weft_ort_session_open_ex), model loads from MEMORY (no filesystem
/// dependency), one IoBinding is created.
weft_ort_err_t weft_ort_session_open(weft_ort_session_t** out,
                                     weft_ort_runtime_t* rt,
                                     const void* model_bytes,
                                     size_t model_len);

/// Extended open (intra_threads: 0 = runtime default; graph_opt_level:
/// 0..3, default 1 — "basic"; the fixture runs at any level).
weft_ort_err_t weft_ort_session_open_ex(weft_ort_session_t** out,
                                        weft_ort_runtime_t* rt,
                                        const void* model_bytes,
                                        size_t model_len,
                                        int intra_threads,
                                        int graph_opt_level);

/// Zero-copy wrap: view → OrtValue (CreateTensorWithDataAsOrtValue with
/// CPU default memory info). The value NEVER copies — the gate asserts
/// GetTensorMutableData(value) == view->data. Caller releases with
/// weft_ort_value_free. dtype must map (f32/f16/u8/u16/i8/i32/i64/u32/
/// u64/f64; i16/u64 map too — the full WTS1 dialect minus none).
weft_ort_err_t weft_ort_wrap_view(weft_ort_session_t* s,
                                  const weft_tensor_view_t* v,
                                  void** out_value);

/// Bind a wrapped value as a model input (by name; rebinds replace).
weft_ort_err_t weft_ort_bind_input(weft_ort_session_t* s, const char* name,
                                   void* value);

/// Bind a model OUTPUT into Weft memory: wraps out_view (zero-copy) and
/// binds it — the EP writes results DIRECTLY into the span. Returns the
/// wrapped value (release with weft_ort_value_free AFTER session close,
/// or let weft_ort_session_close release the pooled set).
weft_ort_err_t weft_ort_bind_output_span(weft_ort_session_t* s,
                                         const char* name,
                                         const weft_tensor_view_t* out_view,
                                         void** out_value);

/// The hot path: RunWithBinding over the pooled IoBinding. ZERO ORT-side
/// allocations by construction (mem-pattern off, arena off, values
/// pre-wrapped); the AC-O gate ASSERTS the malloc window does not move.
weft_ort_err_t weft_ort_run_pooled(weft_ort_session_t* s);

/// Input/output name i of the session (for bind calls). Returns NULL on
/// bad index (callers do not free — owned by the session).
const char* weft_ort_input_name(weft_ort_session_t* s, size_t i);
const char* weft_ort_output_name(weft_ort_session_t* s, size_t i);
size_t       weft_ort_input_count(weft_ort_session_t* s);
size_t       weft_ort_output_count(weft_ort_session_t* s);

/// Release a wrapped value (NULL-safe).
void weft_ort_value_free(weft_ort_session_t* s, void* value);

/// Close + release everything the session created (the runtime stays).
void weft_ort_session_close(weft_ort_session_t* s);

// ---------------------------------------------------------------------------
// Dtype map (WTS1 dialect -> ONNXTensorElementDataType)
// ---------------------------------------------------------------------------

/// ONNX element type for a WTS1 dtype (-1 when unmappable). FLOAT=1,
/// UINT8=2, INT8=3, UINT16=4, INT16=5, INT32=6, INT64=7, FLOAT16=10,
/// DOUBLE=11, UINT32=12, UINT64=13 (the verified v1.16.3 enum).
int weft_ort_onnx_type(weft_tensor_dtype_t t);

#ifdef __cplusplus
}
#endif

#endif // WEFT_ORT_BRIDGE_H
