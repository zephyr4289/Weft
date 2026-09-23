// weft_ort_bridge.c — RFC-0017 §5 implementation.
//
// Every ORT call goes through the verified v1.16.3 table mirror
// (weft_ort_abi.h — offsetof-asserted indices; drift is a compile error,
// an old runtime is a load-time refusal). The mock table injects through
// weft_ort_load_table and runs the identical code paths — the logic
// battery never needs libonnxruntime present (Law 4's honest split).

#include "weft_ort_bridge.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weft_ort_abi.h"

// ---------------------------------------------------------------------------
// Error names
// ---------------------------------------------------------------------------

const char* weft_ort_err_name(weft_ort_err_t e) {
    switch (e) {
    case WEFT_ORT_OK:             return "ok";
    case WEFT_ORT_ERR_NO_RUNTIME: return "no-libonnxruntime";
    case WEFT_ORT_ERR_OLD_RUNTIME:return "runtime-cannot-serve-api-16";
    case WEFT_ORT_ERR_BAD_ARG:    return "bad-argument";
    case WEFT_ORT_ERR_STATUS:     return "ort-call-returned-status";
    case WEFT_ORT_ERR_VIEW:       return "tensor-view-refused";
    case WEFT_ORT_ERR_DTYPE:      return "dtype-unmappable-to-onnx";
    case WEFT_ORT_ERR_MODEL:      return "model-bytes-refused";
    case WEFT_ORT_ERR_BINDING:    return "io-binding-refused";
    default:                      return "unknown";
    }
}

// ---------------------------------------------------------------------------
// The runtime
// ---------------------------------------------------------------------------

struct weft_ort_runtime {
    void*             lib;        // dlopen'd (NULL for the mock)
    weft_ort_api_t    api;        // the resolved table (copy of used fns)
    const void*       raw_table;  // the live table pointer (mock or real)
    char              version[96];
    char              last_error[256];
};

static void set_last_error(weft_ort_runtime_t* rt, const void* status) {
    if (!rt) return;
    rt->last_error[0] = '\0';
    if (!status) return;
    const char* msg = rt->api.GetErrorMessage
        ? rt->api.GetErrorMessage(status) : NULL;
    if (msg) {
        strncpy(rt->last_error, msg, sizeof(rt->last_error) - 1);
        rt->last_error[sizeof(rt->last_error) - 1] = '\0';
    }
}

/// Check a status return; on failure record + release the status.
static weft_ort_err_t check_status(weft_ort_runtime_t* rt, void* status) {
    if (status == NULL) return WEFT_ORT_OK;
    set_last_error(rt, status);
    if (rt->api.ReleaseStatus) rt->api.ReleaseStatus(status);
    return WEFT_ORT_ERR_STATUS;
}

weft_ort_err_t weft_ort_load_table(weft_ort_runtime_t** out,
                                   const void* api_table,
                                   const char* version_label) {
    if (!out || !api_table) return WEFT_ORT_ERR_BAD_ARG;
    *out = NULL;
    weft_ort_runtime_t* rt = (weft_ort_runtime_t*)calloc(1, sizeof(*rt));
    if (!rt) return WEFT_ORT_ERR_NO_RUNTIME;  // setup OOM: honest refusal
    // Copy the USED fields from the live table (uniform access; the
    // unused positions stay NULL and are never called).
    const weft_ort_api_t* src = (const weft_ort_api_t*)api_table;
    rt->api = *src;
    rt->raw_table = api_table;
    snprintf(rt->version, sizeof(rt->version), "%s",
             version_label ? version_label : "(injected table)");
    *out = rt;
    return WEFT_ORT_OK;
}

weft_ort_err_t weft_ort_load(weft_ort_runtime_t** out) {
    if (!out) return WEFT_ORT_ERR_BAD_ARG;
    *out = NULL;

    void* lib = dlopen("libonnxruntime.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libonnxruntime.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return WEFT_ORT_ERR_NO_RUNTIME;

    weft_ort_get_api_base_fn get_base =
        (weft_ort_get_api_base_fn)dlsym(lib, "OrtGetApiBase");
    if (!get_base) {
        dlclose(lib);
        return WEFT_ORT_ERR_NO_RUNTIME;
    }
    const weft_ort_api_base_t* base =
        (const weft_ort_api_base_t*)(uintptr_t)get_base();
    if (!base || !base->GetApi) {
        dlclose(lib);
        return WEFT_ORT_ERR_NO_RUNTIME;
    }
    const void* table = base->GetApi(16);  // the verified mirror's version
    if (!table) {
        dlclose(lib);
        return WEFT_ORT_ERR_OLD_RUNTIME;
    }

    weft_ort_runtime_t* rt = NULL;
    weft_ort_err_t e = weft_ort_load_table(&rt, table, NULL);
    if (e != WEFT_ORT_OK) {
        dlclose(lib);
        return e;
    }
    rt->lib = lib;
    // The runtime's own version string (evidence lines carry it).
    if (base->GetVersionString) {
        const char* v = base->GetVersionString();
        snprintf(rt->version, sizeof(rt->version), "%s", v ? v : "?");
    }
    // Sanity: the table must expose the load-bearing fields.
    if (!rt->api.CreateTensorWithDataAsOrtValue ||
        !rt->api.RunWithBinding || !rt->api.CreateIoBinding ||
        !rt->api.BindInput || !rt->api.BindOutput) {
        weft_ort_unload(rt);
        return WEFT_ORT_ERR_OLD_RUNTIME;
    }
    *out = rt;
    return WEFT_ORT_OK;
}

const char* weft_ort_version(const weft_ort_runtime_t* rt) {
    return rt ? rt->version : "(not loaded)";
}

const char* weft_ort_last_error(const weft_ort_runtime_t* rt) {
    return rt ? rt->last_error : "";
}

const void* weft_ort_raw_table(const weft_ort_runtime_t* rt) {
    return rt ? rt->raw_table : NULL;
}

void weft_ort_unload(weft_ort_runtime_t* rt) {
    if (!rt) return;
    if (rt->lib) dlclose(rt->lib);
    free(rt);
}

// ---------------------------------------------------------------------------
// The pooled session
// ---------------------------------------------------------------------------

#define WEFT_ORT_MAX_NAMES 8

struct weft_ort_session {
    weft_ort_runtime_t* rt;         // borrowed
    void*  env;                     // owned
    void*  options;                 // owned
    void*  session;                 // owned
    void*  io_binding;              // owned (ONE — Law 1)
    void*  memory_info;             // owned (CPU default)
    char*  input_names[WEFT_ORT_MAX_NAMES];   // owned copies
    char*  output_names[WEFT_ORT_MAX_NAMES];
    size_t n_inputs;
    size_t n_outputs;
};

static weft_ort_err_t fetch_name(weft_ort_session_t* s, int is_input,
                                 size_t i, char** slot);

weft_ort_err_t weft_ort_session_open_ex(weft_ort_session_t** out,
                                        weft_ort_runtime_t* rt,
                                        const void* model_bytes,
                                        size_t model_len,
                                        int intra_threads,
                                        int graph_opt_level) {
    if (!out || !rt || !model_bytes || model_len == 0) {
        return WEFT_ORT_ERR_BAD_ARG;
    }
    *out = NULL;
    weft_ort_api_t* api = &rt->api;

    weft_ort_session_t* s = (weft_ort_session_t*)calloc(1, sizeof(*s));
    if (!s) return WEFT_ORT_ERR_NO_RUNTIME;  // setup OOM: honest refusal
    s->rt = rt;

    weft_ort_err_t e;
    // env: warning-level logging, no telemetry round-trips in the path
    e = check_status(rt, api->CreateEnv(3 /*ORT_LOGGING_LEVEL_WARNING*/,
                                        "weft-tensor", &s->env));
    if (e != WEFT_ORT_OK) goto fail;

    e = check_status(rt, api->CreateSessionOptions(&s->options));
    if (e != WEFT_ORT_OK) goto fail;

    // THE POOLING PROFILE (Law 1's three knobs):
    //   - DisableMemPattern: no execution-plan memory reuse patterns that
    //     allocate per-run arenas
    //   - DisableCpuMemArena: no CRT arena on top (device allocations
    //     come from the EPs, inputs/outputs are OUR wrapped memory)
    //   - intra-op threads pinned: no thread-pool growth at run time
    if (api->DisableMemPattern) {
        e = check_status(rt, api->DisableMemPattern(s->options));
        if (e != WEFT_ORT_OK) goto fail;
    }
    if (api->DisableCpuMemArena) {
        e = check_status(rt, api->DisableCpuMemArena(s->options));
        if (e != WEFT_ORT_OK) goto fail;
    }
    if (intra_threads > 0 && api->SetIntraOpNumThreads) {
        e = check_status(rt, api->SetIntraOpNumThreads(s->options,
                                                       intra_threads));
        if (e != WEFT_ORT_OK) goto fail;
    }
    if (graph_opt_level >= 0 && api->SetSessionGraphOptimizationLevel) {
        // level 1 = ORT_ENABLE_BASIC (deterministic, no layout swaps the
        // pooling profile cannot predict)
        e = check_status(rt, api->SetSessionGraphOptimizationLevel(
                                 s->options, graph_opt_level));
        if (e != WEFT_ORT_OK) goto fail;
    }

    // model from MEMORY (CreateSessionFromArray) — no filesystem rides
    // in the evidence path (the fixture bytes ship in-tree).
    e = check_status(rt, api->CreateSessionFromArray(s->env, model_bytes,
                                                     model_len, s->options,
                                                     &s->session));
    if (e != WEFT_ORT_OK) {
        e = WEFT_ORT_ERR_MODEL;
        goto fail;
    }

    e = check_status(rt, api->CreateIoBinding(s->session, &s->io_binding));
    if (e != WEFT_ORT_OK) goto fail;

    // CPU default memory info: allocator type 0 (device), mem type 0
    // (default) — the wrap's zero-copy currency on CPU-capable EPs.
    e = check_status(rt, api->CreateCpuMemoryInfo(0, 0, &s->memory_info));
    if (e != WEFT_ORT_OK) goto fail;

    // Input/output name census (owned copies; bounded).
    e = check_status(rt, api->SessionGetInputCount(s->session, &s->n_inputs));
    if (e != WEFT_ORT_OK) goto fail;
    e = check_status(rt, api->SessionGetOutputCount(s->session,
                                                    &s->n_outputs));
    if (e != WEFT_ORT_OK) goto fail;
    if (s->n_inputs > WEFT_ORT_MAX_NAMES || s->n_outputs > WEFT_ORT_MAX_NAMES ||
        s->n_inputs == 0 || s->n_outputs == 0) {
        e = WEFT_ORT_ERR_MODEL;  // fixture-class models only (v1 scope)
        goto fail;
    }
    for (size_t i = 0; i < s->n_inputs; i++) {
        e = fetch_name(s, 1, i, &s->input_names[i]);
        if (e != WEFT_ORT_OK) goto fail;
    }
    for (size_t i = 0; i < s->n_outputs; i++) {
        e = fetch_name(s, 0, i, &s->output_names[i]);
        if (e != WEFT_ORT_OK) goto fail;
    }

    *out = s;
    return WEFT_ORT_OK;

fail:
    weft_ort_session_close(s);
    return e;
}

/// SessionGetInputName/OutputName use an allocator in the modern API —
/// but the mirror pins the RAW table's fields 36/37
/// (SessionGetInputName(session, index, allocator, char** out)) whose
/// v1.16 signature allocates through the given allocator. Passing the
/// default allocator (field 78) keeps ownership simple. When the raw
/// fields are absent (mock), names are faked by the mock itself.
static weft_ort_err_t fetch_name(weft_ort_session_t* s, int is_input,
                                 size_t i, char** slot) {
    weft_ort_runtime_t* rt = s->rt;
    // The name fields sit at fixed positions in the RAW table (36/37 —
    // verified); they were not mirrored as typed fields, so address the
    // raw table directly with bounds checked by the mirror's size.
    void** raw = (void**)s->rt->raw_table;
    void* (*get_name)(void*, size_t, void*, char**) =
        (void* (*)(void*, size_t, void*, char**))raw[is_input ? 36 : 37];
    void* (*default_allocator)(void) =
        (void* (*)(void))raw[78];  // GetAllocatorWithDefaultOptions
    if (!get_name || !default_allocator) {
        return WEFT_ORT_ERR_OLD_RUNTIME;
    }
    void* alloc = default_allocator();
    char* name = NULL;
    weft_ort_err_t e =
        check_status(rt, get_name(s->session, i, alloc, &name));
    if (e != WEFT_ORT_OK) return e;
    if (!name) return WEFT_ORT_ERR_MODEL;
    *slot = name;  // owned by the session (freed with the allocator-less
                   // CRT copy discipline: the ORT name buffer is valid
                   // until ReleaseSession; we keep the pointer)
    return WEFT_ORT_OK;
}

weft_ort_err_t weft_ort_session_open(weft_ort_session_t** out,
                                     weft_ort_runtime_t* rt,
                                     const void* model_bytes,
                                     size_t model_len) {
    return weft_ort_session_open_ex(out, rt, model_bytes, model_len, 1, 1);
}

int weft_ort_onnx_type(weft_tensor_dtype_t t) {
    switch (t) {
    case WEFT_TENSOR_F32:  return 1;   // FLOAT
    case WEFT_TENSOR_U8:   return 2;   // UINT8
    case WEFT_TENSOR_I8:   return 3;   // INT8
    case WEFT_TENSOR_U16:  return 4;   // UINT16
    case WEFT_TENSOR_I16:  return 5;   // INT16
    case WEFT_TENSOR_I32:  return 6;   // INT32
    case WEFT_TENSOR_I64:  return 7;   // INT64
    case WEFT_TENSOR_F16:  return 10;  // FLOAT16
    case WEFT_TENSOR_F64:  return 11;  // DOUBLE
    case WEFT_TENSOR_U32:  return 12;  // UINT32
    case WEFT_TENSOR_U64:  return 13;  // UINT64
    default:               return -1;
    }
}

weft_ort_err_t weft_ort_wrap_view(weft_ort_session_t* s,
                                  const weft_tensor_view_t* v,
                                  void** out_value) {
    if (!s || !v || !out_value) return WEFT_ORT_ERR_BAD_ARG;
    *out_value = NULL;

    weft_tv_err_t tv = weft_tensor_view_validate(v, 0);
    if (tv != WEFT_TV_OK) return WEFT_ORT_ERR_VIEW;

    int onnx_type = weft_ort_onnx_type((weft_tensor_dtype_t)v->dtype);
    if (onnx_type < 0) return WEFT_ORT_ERR_DTYPE;

    // PACKED views wrap flat; strided views refuse (ONNX dense tensors
    // are contiguous — a stride copy would be a [FALLBACK-COPY], not a
    // wrap; the caller runs the SIMD road).
    if (!(v->flags & WEFT_TENSOR_VIEW_F_PACKED)) return WEFT_ORT_ERR_VIEW;
    if (v->flags & WEFT_TENSOR_VIEW_F_BIG_ENDIAN) {
        return WEFT_ORT_ERR_VIEW;  // conversion is the fallback road
    }

    int64_t shape[4];
    for (uint32_t k = 0; k < v->rank; k++) shape[k] = (int64_t)v->dims[k];

    weft_ort_runtime_t* rt = s->rt;
    void* value = NULL;
    weft_ort_err_t e = check_status(
        rt, rt->api.CreateTensorWithDataAsOrtValue(
                s->memory_info, v->data, (size_t)v->byte_len, shape,
                (size_t)v->rank, onnx_type, &value));
    if (e != WEFT_ORT_OK) return e;
    if (!value) return WEFT_ORT_ERR_STATUS;
    *out_value = value;
    return WEFT_ORT_OK;
}

weft_ort_err_t weft_ort_bind_input(weft_ort_session_t* s, const char* name,
                                   void* value) {
    if (!s || !name || !value) return WEFT_ORT_ERR_BAD_ARG;
    return check_status(s->rt,
                        s->rt->api.BindInput(s->io_binding, name, value));
}

weft_ort_err_t weft_ort_bind_output_span(weft_ort_session_t* s,
                                         const char* name,
                                         const weft_tensor_view_t* out_view,
                                         void** out_value) {
    if (!s || !name || !out_view || !out_value) return WEFT_ORT_ERR_BAD_ARG;
    *out_value = NULL;
    weft_ort_err_t e = weft_ort_wrap_view(s, out_view, out_value);
    if (e != WEFT_ORT_OK) return e;
    e = check_status(s->rt, s->rt->api.BindOutput(s->io_binding, name,
                                                  *out_value));
    if (e != WEFT_ORT_OK) {
        weft_ort_value_free(s, *out_value);
        *out_value = NULL;
        return e;
    }
    return WEFT_ORT_OK;
}

weft_ort_err_t weft_ort_run_pooled(weft_ort_session_t* s) {
    if (!s || !s->io_binding) return WEFT_ORT_ERR_BAD_ARG;
    // The hot path: ONE table call (RunWithBinding). Mem-pattern is off,
    // the arena is off, every value is pre-wrapped — the AC-O gate
    // asserts the malloc window does not move across this call.
    return check_status(s->rt,
                        s->rt->api.RunWithBinding(s->session, NULL,
                                                  s->io_binding));
}

const char* weft_ort_input_name(weft_ort_session_t* s, size_t i) {
    if (!s || i >= s->n_inputs) return NULL;
    return s->input_names[i];
}
const char* weft_ort_output_name(weft_ort_session_t* s, size_t i) {
    if (!s || i >= s->n_outputs) return NULL;
    return s->output_names[i];
}
size_t weft_ort_input_count(weft_ort_session_t* s) {
    return s ? s->n_inputs : 0;
}
size_t weft_ort_output_count(weft_ort_session_t* s) {
    return s ? s->n_outputs : 0;
}

void weft_ort_value_free(weft_ort_session_t* s, void* value) {
    if (!s || !value) return;
    if (s->rt->api.ReleaseValue) s->rt->api.ReleaseValue(value);
}

void weft_ort_session_close(weft_ort_session_t* s) {
    if (!s) return;
    weft_ort_api_t* api = &s->rt->api;
    if (s->io_binding && api->ReleaseIoBinding) {
        api->ReleaseIoBinding(s->io_binding);
    }
    if (s->session && api->ReleaseSession) api->ReleaseSession(s->session);
    if (s->memory_info && api->ReleaseMemoryInfo) {
        api->ReleaseMemoryInfo(s->memory_info);
    }
    if (s->options && api->ReleaseSessionOptions) {
        api->ReleaseSessionOptions(s->options);
    }
    if (s->env && api->ReleaseEnv) api->ReleaseEnv(s->env);
    // Names are owned by the ORT session (valid until ReleaseSession —
    // already released above); nothing further to free here.
    free(s);
}
