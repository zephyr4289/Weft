// mock_ort.h — the fake OrtApi table for the AC-O logic battery.
//
// WHY: the bridge's logic (pointer identity, option discipline, bind/run
// call shapes, the Law-1 malloc window) must be GATED without
// libonnxruntime present — the no-runtime CI leg is itself a gate (the
// gpu_ring no-ICD precedent). The mock records every call the bridge
// makes and answers with objects that hold the EXACT arguments they were
// constructed with, so assertions are structural (pointer equality,
// recorded flags), not behavioral re-implementations of ONNX semantics.

#ifndef MOCK_ORT_H
#define MOCK_ORT_H

#include "weft_ort_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The mock's recording state (one global — tests are single-threaded).
typedef struct {
    // call counts
    int create_env;
    int create_session_from_array;
    int create_session_options;
    int disable_mem_pattern;
    int disable_cpu_mem_arena;
    int set_intra_op_threads;
    int create_io_binding;
    int run_with_binding;
    int create_tensor_with_data;
    int bind_input;
    int bind_output;
    int release_value;
    // last CreateTensorWithDataAsOrtValue args (the identity gate)
    void*    wrap_data;
    size_t   wrap_len;
    int64_t  wrap_shape[4];
    size_t   wrap_shape_len;
    int      wrap_type;
    // bound names (borrowed pointers into the bridge's strings)
    const char* last_input_name;
    const char* last_output_name;
    void*       last_output_value;
    // model bytes the session was opened with
    const void* model_bytes;
    size_t      model_len;
    // failure injection (nonzero -> the call returns a fake status)
    int fail_wrap;
    int fail_run;
} mock_ort_log_t;

extern mock_ort_log_t g_mock_ort;

/// Build the fake table (typed used-fields + name/allocator slots filled
/// at their verified raw positions). The returned pointer IS a
/// weft_ort_api_t the bridge consumes through weft_ort_load_table.
const weft_ort_api_t* mock_ort_table(void);

/// Reset the recording state (between gates).
void mock_ort_reset(void);

/// The fake value object layout (tests cast OrtValue* to this).
typedef struct {
    void*    data;        ///< the wrapped pointer (identity gate)
    size_t   len;
    int64_t  shape[4];
    size_t   shape_len;
    int      onnx_type;
} mock_ort_value_t;

#ifdef __cplusplus
}
#endif

#endif // MOCK_ORT_H
