// mock_ort.c — the fake OrtApi table implementation (see mock_ort.h).

#include "mock_ort.h"

#include <stdlib.h>
#include <string.h>

mock_ort_log_t g_mock_ort;

// --- fake objects ---------------------------------------------------------

typedef struct { int magic; } mock_env_t;
typedef struct { const void* bytes; size_t len; } mock_session_t;
typedef struct { int magic; } mock_options_t;
typedef struct { int n_inputs, n_outputs; } mock_binding_t;

static void* status_ok = NULL;  // ORT: NULL status == success

static void* mock_create_env(int level, const char* logid, void** out) {
    (void)level; (void)logid;
    g_mock_ort.create_env++;
    *out = calloc(1, sizeof(mock_env_t));
    return status_ok;
}

static void* mock_create_session_from_array(void* env, const void* data,
                                            size_t len, const void* options,
                                            void** out) {
    (void)env; (void)options;
    g_mock_ort.create_session_from_array++;
    mock_session_t* s = (mock_session_t*)calloc(1, sizeof(*s));
    s->bytes = data;
    s->len = len;
    g_mock_ort.model_bytes = data;
    g_mock_ort.model_len = len;
    *out = s;
    return status_ok;
}

static void* mock_create_session_options(void** out) {
    g_mock_ort.create_session_options++;
    *out = calloc(1, sizeof(mock_options_t));
    return status_ok;
}

static void* mock_disable_mem_pattern(void* options) {
    (void)options;
    g_mock_ort.disable_mem_pattern++;
    return status_ok;
}

static void* mock_disable_cpu_mem_arena(void* options) {
    (void)options;
    g_mock_ort.disable_cpu_mem_arena++;
    return status_ok;
}

static void* mock_set_intra_op_threads(void* options, int n) {
    (void)options;
    g_mock_ort.set_intra_op_threads++;
    g_mock_ort.wrap_shape[0] = 0;  // (unused; keeps -Wall quiet)
    (void)n;
    return status_ok;
}

static void* mock_get_input_count(const void* session, size_t* out) {
    (void)session;
    *out = 1;
    return status_ok;
}

static void* mock_get_output_count(const void* session, size_t* out) {
    (void)session;
    *out = 1;
    return status_ok;
}

static void* mock_get_input_name(void* session, size_t i, void* alloc,
                                 char** out) {
    (void)session; (void)alloc;
    // STATIC storage: the bridge's contract treats names as owned by
    // the session (valid until ReleaseSession) — static strings keep
    // the mock allocation-free and ASAN-clean.
    *out = (i == 0) ? (char*)"X" : NULL;
    return status_ok;
}

static void* mock_get_output_name(void* session, size_t i, void* alloc,
                                  char** out) {
    (void)session; (void)alloc;
    *out = (i == 0) ? (char*)"Y" : NULL;
    return status_ok;
}

static void* mock_default_allocator(void) {
    static int alloc_guard = 1;
    return &alloc_guard;
}

static void* mock_create_tensor_with_data(const void* info, void* data,
                                          size_t len, const int64_t* shape,
                                          size_t shape_len, int type,
                                          void** out) {
    (void)info;
    g_mock_ort.create_tensor_with_data++;
    if (g_mock_ort.fail_wrap) {
        static int fake_status = 1;
        return &fake_status;
    }
    g_mock_ort.wrap_data = data;
    g_mock_ort.wrap_len = len;
    g_mock_ort.wrap_shape_len = shape_len < 4 ? shape_len : 4;
    for (size_t i = 0; i < g_mock_ort.wrap_shape_len; i++) {
        g_mock_ort.wrap_shape[i] = shape[i];
    }
    g_mock_ort.wrap_type = type;
    mock_ort_value_t* v = (mock_ort_value_t*)calloc(1, sizeof(*v));
    v->data = data;
    v->len = len;
    v->shape_len = g_mock_ort.wrap_shape_len;
    for (size_t i = 0; i < v->shape_len; i++) v->shape[i] = shape[i];
    v->onnx_type = type;
    *out = v;
    return status_ok;
}

static void* mock_is_tensor(const void* value, int* out) {
    (void)value;
    *out = 1;
    return status_ok;
}

static void* mock_get_tensor_mutable_data(void* value, void** out) {
    // THE IDENTITY GATE: the data pointer the value was wrapped with is
    // exactly what comes back — never a copy.
    *out = ((mock_ort_value_t*)value)->data;
    return status_ok;
}

static void* mock_create_cpu_memory_info(int at, int mt, void** out) {
    (void)at; (void)mt;
    static int mi = 0;
    *out = &mi;
    return status_ok;
}

static void* mock_add_config_entry(void* options, const char* k,
                                   const char* v) {
    (void)options; (void)k; (void)v;
    return status_ok;
}

static void* mock_create_io_binding(void* session, void** out) {
    (void)session;
    g_mock_ort.create_io_binding++;
    mock_binding_t* b = (mock_binding_t*)calloc(1, sizeof(*b));
    *out = b;
    return status_ok;
}

static void* mock_bind_input(void* binding, const char* name,
                             const void* value) {
    (void)binding; (void)value;
    g_mock_ort.bind_input++;
    g_mock_ort.last_input_name = name;
    return status_ok;
}

static void* mock_bind_output(void* binding, const char* name,
                              const void* value) {
    (void)binding;
    g_mock_ort.bind_output++;
    g_mock_ort.last_output_name = name;
    g_mock_ort.last_output_value = (void*)value;
    return status_ok;
}

static void* mock_run_with_binding(void* session, const void* run_options,
                                   const void* binding) {
    (void)session; (void)run_options; (void)binding;
    g_mock_ort.run_with_binding++;
    if (g_mock_ort.fail_run) {
        static int fake_status = 1;
        return &fake_status;
    }
    return status_ok;
}

// generic releasers
static void mock_release_env(void* p)      { free(p); }
static void mock_release_status(void* p)   { (void)p; }
static void mock_release_memory_info(void* p) { (void)p; }
static void mock_release_session(void* p)  { free(p); }
static void mock_release_value(void* p)    { g_mock_ort.release_value++; free(p); }
static void mock_release_session_options(void* p) { free(p); }
static void mock_release_io_binding(void* p) { free(p); }

const weft_ort_api_t* mock_ort_table(void) {
    static weft_ort_api_t t;
    static int built = 0;
    if (built) return &t;
    memset(&t, 0, sizeof(t));

    t.CreateEnv = mock_create_env;
    t.CreateSessionFromArray = mock_create_session_from_array;
    t.CreateSessionOptions = mock_create_session_options;
    t.DisableMemPattern = mock_disable_mem_pattern;
    t.DisableCpuMemArena = mock_disable_cpu_mem_arena;
    t.SetIntraOpNumThreads = mock_set_intra_op_threads;
    t.SessionGetInputCount = mock_get_input_count;
    t.SessionGetOutputCount = mock_get_output_count;
    t.CreateTensorWithDataAsOrtValue = mock_create_tensor_with_data;
    t.IsTensor = mock_is_tensor;
    t.GetTensorMutableData = mock_get_tensor_mutable_data;
    t.CreateCpuMemoryInfo = mock_create_cpu_memory_info;
    t.AddSessionConfigEntry = mock_add_config_entry;
    t.RunWithBinding = mock_run_with_binding;
    t.CreateIoBinding = mock_create_io_binding;
    t.BindInput = mock_bind_input;
    t.BindOutput = mock_bind_output;
    t.ReleaseEnv = mock_release_env;
    t.ReleaseStatus = mock_release_status;
    t.ReleaseMemoryInfo = mock_release_memory_info;
    t.ReleaseSession = mock_release_session;
    t.ReleaseValue = mock_release_value;
    t.ReleaseSessionOptions = mock_release_session_options;
    t.ReleaseIoBinding = mock_release_io_binding;

    // raw-position slots the bridge addresses directly (verified indices:
    // 36 SessionGetInputName, 37 SessionGetOutputName, 78 default allocator)
    t._reserved_36 = (void*)mock_get_input_name;
    t._reserved_37 = (void*)mock_get_output_name;
    t._reserved_78 = (void*)mock_default_allocator;

    built = 1;
    return &t;
}

void mock_ort_reset(void) {
    memset(&g_mock_ort, 0, sizeof(g_mock_ort));
}
