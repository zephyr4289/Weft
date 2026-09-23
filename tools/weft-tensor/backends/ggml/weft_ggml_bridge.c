// weft_ggml_bridge.c — RFC-0017 §6 runtime bridge (dlopen + probe +
// the formal buffer-type road).
//
// PINNED ABI: the b4312-era backend ABI (GGML_BACKEND_API_VERSION 1 —
// the last generation whose buffer-type structs are PUBLIC; references
// vendored at tests/abi-verify/ggml-2024-{backend,impl}.h). The 2025
// split made the structs opaque: when the probe detects that era the
// formal road REFUSES with OPAQUE_ERA and the data-pointer road (which
// works across every ggml generation) is the documented path — never a
// silent layout mismatch (Law 4).

#include "weft_ggml_bridge.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The mirrored ABI structs (b4312 — layout-stable, from ggml-backend-impl.h)
// ---------------------------------------------------------------------------

typedef struct ggml_backend_buffer_type* ggml_backend_buffer_type_t;
typedef struct ggml_backend_buffer* ggml_backend_buffer_t;
typedef bool  ggml_bool;

struct ggml_backend_buffer_type_i {
    const char*          (*get_name)(ggml_backend_buffer_type_t buft);
    ggml_backend_buffer_t (*alloc_buffer)(ggml_backend_buffer_type_t buft,
                                          size_t size);
    size_t               (*get_alignment)(ggml_backend_buffer_type_t buft);
    size_t               (*get_max_size)(ggml_backend_buffer_type_t buft);
    size_t               (*get_alloc_size)(ggml_backend_buffer_type_t buft,
                                           const void* tensor);
    ggml_bool            (*is_host)(ggml_backend_buffer_type_t buft);
};

struct ggml_backend_buffer_type {
    struct ggml_backend_buffer_type_i iface;
    void* device;    // ggml_backend_dev_t (NULL = no device — legal pre-dev era)
    void* context;
};

struct ggml_backend_buffer_i {
    void   (*free_buffer)(ggml_backend_buffer_t buffer);
    void*  (*get_base)(ggml_backend_buffer_t buffer);
    void   (*init_tensor)(ggml_backend_buffer_t buffer, void* tensor);
    void   (*memset_tensor)(ggml_backend_buffer_t buffer, void* tensor,
                            uint8_t value, size_t offset, size_t size);
    void   (*set_tensor)(ggml_backend_buffer_t buffer, void* tensor,
                         const void* data, size_t offset, size_t size);
    void   (*get_tensor)(ggml_backend_buffer_t buffer, const void* tensor,
                         void* data, size_t offset, size_t size);
    ggml_bool (*cpy_tensor)(ggml_backend_buffer_t buffer,
                            const void* src, void* dst);
    void   (*clear)(ggml_backend_buffer_t buffer, uint8_t value);
    void   (*reset)(ggml_backend_buffer_t buffer);
};

struct ggml_backend_buffer {
    struct ggml_backend_buffer_i iface;
    ggml_backend_buffer_type_t   buft;
    void*  context;
    size_t size;
    int    usage;   // enum ggml_backend_buffer_usage (ANY=0/WEIGHTS/COMPUTE)
};

// The public ggml_tensor layout (stable across generations — from the
// vendored ggml.h; used ONLY by the nbytes ABI probe).
#define WEFT_GGML_MAX_DIMS 4
#define WEFT_GGML_MAX_OP_PARAMS 64
#define WEFT_GGML_MAX_SRC 10
#define WEFT_GGML_MAX_NAME 64

struct weft_ggml_tensor_probe {
    int      type;                       // enum ggml_type (F32 = 0)
    void*    buffer;
    int64_t  ne[WEFT_GGML_MAX_DIMS];
    size_t   nb[WEFT_GGML_MAX_DIMS];
    int      op;
    int32_t  op_params[WEFT_GGML_MAX_OP_PARAMS / sizeof(int32_t)];
    int32_t  flags;
    void*    src[WEFT_GGML_MAX_SRC];
    void*    view_src;
    size_t   view_offs;
    void*    data;
    char     name[WEFT_GGML_MAX_NAME];
    void*    extra;
    char     padding[8];
};

// resolved symbol types
typedef uint64_t (*ggml_nbytes_fn)(const void* t);
typedef size_t   (*ggml_type_size_fn)(int type);
typedef int64_t  (*ggml_blck_size_fn)(int type);
typedef const char* (*ggml_type_name_fn)(int type);
typedef void*    (*ggml_backend_buffer_init_fn)(
    ggml_backend_buffer_type_t buft, struct ggml_backend_buffer_i iface,
    void* context, size_t size);

// ---------------------------------------------------------------------------
// Error names
// ---------------------------------------------------------------------------

const char* weft_ggml_err_name(weft_ggml_err_t e) {
    switch (e) {
    case WEFT_GGML_ERR_OK:          return "ok";
    case WEFT_GGML_ERR_NO_LIB:      return "no-libggml";
    case WEFT_GGML_ERR_NO_SYMBOLS:  return "library-present-symbols-absent";
    case WEFT_GGML_ERR_ABI_PROBE:   return "ggml_nbytes-disagrees-mirror-refused";
    case WEFT_GGML_ERR_OPAQUE_ERA:  return "2025-opaque-buffer-abi";
    case WEFT_GGML_ERR_BAD_ARG:     return "bad-argument";
    case WEFT_GGML_ERR_PLAN:        return "placement-refused-full-spans";
    case WEFT_GGML_ERR_VIEW:        return "tensor-view-refused";
    default:                        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// The runtime
// ---------------------------------------------------------------------------

struct weft_ggml_rt {
    void* lib_core;      // libggml-base / libggml (types + nbytes)
    void* lib_backend;   // libggml-backend / libggml (buffer_init)
    ggml_nbytes_fn            ggml_nbytes;
    ggml_type_size_fn         ggml_type_size;
    ggml_blck_size_fn         ggml_blck_size;
    ggml_type_name_fn         ggml_type_name;
    ggml_backend_buffer_init_fn ggml_backend_buffer_init;
    int  probe_ok;           // nbytes probe agreed with the mirror
    char report[192];
    // the formal road's static buffer type (one per runtime — ggml's
    // own CPU buffer type is also a singleton)
    struct ggml_backend_buffer_type buft;
    weft_ggml_planner_t* buft_planner;  // bound at weft_ggml_buffer_type()
    int buft_bound;
};

weft_ggml_err_t weft_ggml_rt_load(weft_ggml_rt_t** out) {
    if (!out) return WEFT_GGML_ERR_BAD_ARG;
    *out = NULL;

    weft_ggml_rt_t* rt = (weft_ggml_rt_t*)calloc(1, sizeof(*rt));
    if (!rt) return WEFT_GGML_ERR_NO_LIB;  // setup OOM: honest refusal

    // core: try the combined lib first, then the split base
    const char* core_names[] = { "libggml.so", "libggml-base.so",
                                 "libggml.so.1", "libggml-base.so.1" };
    for (size_t i = 0; i < sizeof(core_names) / sizeof(core_names[0]); i++) {
        rt->lib_core = dlopen(core_names[i], RTLD_NOW | RTLD_LOCAL);
        if (rt->lib_core) break;
    }
    if (!rt->lib_core) {
        free(rt);
        return WEFT_GGML_ERR_NO_LIB;
    }
    rt->ggml_nbytes = (ggml_nbytes_fn)dlsym(rt->lib_core, "ggml_nbytes");
    rt->ggml_type_size = (ggml_type_size_fn)dlsym(rt->lib_core,
                                                  "ggml_type_size");
    rt->ggml_blck_size = (ggml_blck_size_fn)dlsym(rt->lib_core,
                                                  "ggml_blck_size");
    rt->ggml_type_name = (ggml_type_name_fn)dlsym(rt->lib_core,
                                                  "ggml_type_name");
    if (!rt->ggml_nbytes || !rt->ggml_type_size || !rt->ggml_blck_size) {
        // Library present but the plan-critical symbols are absent —
        // an incompatible object, refused by name.
        dlclose(rt->lib_core);
        free(rt);
        return WEFT_GGML_ERR_NO_SYMBOLS;
    }

    // backend: buffer_init (the formal road's constructor)
    const char* backend_names[] = { "libggml-backend.so", "libggml.so",
                                    "libggml-backend.so.1" };
    for (size_t i = 0; i < sizeof(backend_names) / sizeof(backend_names[0]); i++) {
        rt->lib_backend = dlopen(backend_names[i], RTLD_NOW | RTLD_LOCAL);
        if (rt->lib_backend) {
            rt->ggml_backend_buffer_init =
                (ggml_backend_buffer_init_fn)dlsym(
                    rt->lib_backend, "ggml_backend_buffer_init");
            if (rt->ggml_backend_buffer_init) break;
            dlclose(rt->lib_backend);
            rt->lib_backend = NULL;
        }
    }
    // (buffer_init absent is NOT fatal — the data-pointer road works;
    // the formal road reports OPAQUE_ERA/NO_SYMBOLS when requested.)

    // THE ABI PROBE (Law 4's runtime mirror of the compile-time asserts):
    // a mirror-laid F32[64] tensor whose nbytes the real library must
    // agree with, plus the type-table cross-checks. Any disagreement
    // refuses the formal road outright.
    struct weft_ggml_tensor_probe probe;
    memset(&probe, 0, sizeof(probe));
    probe.type = 0;  // GGML_TYPE_F32
    probe.ne[0] = 64; probe.ne[1] = 1; probe.ne[2] = 1; probe.ne[3] = 1;
    probe.nb[0] = 4; probe.nb[1] = 256; probe.nb[2] = 256; probe.nb[3] = 256;
    uint64_t nbytes = rt->ggml_nbytes(&probe);
    int f32_size_ok = (rt->ggml_type_size(0) == 4);
    int f32_blck_ok = (rt->ggml_blck_size(0) == 1);
    rt->probe_ok = (nbytes == 256) && f32_size_ok && f32_blck_ok;

    const char* core_name = "?";
    Dl_info info;
    if (dladdr(rt->ggml_nbytes, &info) && info.dli_fname) {
        core_name = info.dli_fname;
    }
    snprintf(rt->report, sizeof(rt->report),
             "lib=%s nbytes_probe=%s (got %llu, want 256; f32 size %d blck %d)"
             " formal_road=%s",
             core_name, rt->probe_ok ? "OK" : "FAIL",
             (unsigned long long)nbytes, f32_size_ok, f32_blck_ok,
             (rt->ggml_backend_buffer_init && rt->probe_ok)
                 ? "capable (b4312 ABI)" : "REFUSED (opaque/symbols)");

    *out = rt;
    return WEFT_GGML_ERR_OK;
}

const char* weft_ggml_rt_report(const weft_ggml_rt_t* rt) {
    return rt ? rt->report : "(not loaded)";
}

void weft_ggml_rt_unload(weft_ggml_rt_t* rt) {
    if (!rt) return;
    if (rt->lib_backend) dlclose(rt->lib_backend);
    if (rt->lib_core) dlclose(rt->lib_core);
    free(rt);
}

// ---------------------------------------------------------------------------
// The formal road: the WEFT_DMA buffer type
// ---------------------------------------------------------------------------

static const char* buft_get_name(ggml_backend_buffer_type_t buft) {
    (void)buft;
    return "WEFT_DMA";
}

static size_t buft_get_alignment(ggml_backend_buffer_type_t buft) {
    (void)buft;
    return 64;  // Law 2 — the float-vector alignment floor
}

static size_t buft_get_max_size(ggml_backend_buffer_type_t buft) {
    struct ggml_backend_buffer_type* b =
        (struct ggml_backend_buffer_type*)buft;
    weft_ggml_rt_t* rt = (weft_ggml_rt_t*)b->context;
    if (!rt || !rt->buft_planner) return 0;
    uint64_t max_bytes = 0;
    for (int s = 0; s < rt->buft_planner->n; s++) {
        if (rt->buft_planner->spans[s].bytes > max_bytes) {
            max_bytes = rt->buft_planner->spans[s].bytes;
        }
    }
    return (size_t)max_bytes;
}

static ggml_bool buft_is_host(ggml_backend_buffer_type_t buft) {
    (void)buft;
    return true;  // the ring span is CPU-visible shared memory
}

static void* buf_get_base(ggml_backend_buffer_t buffer) {
    weft_ggml_span_t* span = (weft_ggml_span_t*)buffer->context;
    return span ? span->base : NULL;
}

static void buf_init_tensor(ggml_backend_buffer_t buffer, void* tensor) {
    // ggml's own allocator has already placed tensor->data inside the
    // span (base + its packed offset, aligned by buft_get_alignment);
    // init_tensor VALIDATES that placement — a tensor outside the span
    // is a hard failure surfaced by the caller's checks, not a guess.
    (void)buffer; (void)tensor;
}

static void buf_set_tensor(ggml_backend_buffer_t buffer, void* tensor,
                           const void* data, size_t offset, size_t size) {
    weft_ggml_span_t* span = (weft_ggml_span_t*)buffer->context;
    struct weft_ggml_tensor_probe* t =
        (struct weft_ggml_tensor_probe*)tensor;
    if (!span || !t || !t->data) return;
    uint64_t dst = (uint64_t)((uint8_t*)t->data - span->base) + offset;
    if (dst + size > span->bytes) return;  // bounds: refused, never torn
    memcpy((uint8_t*)t->data + offset, data, size);
}

static void buf_get_tensor(ggml_backend_buffer_t buffer, const void* tensor,
                           void* data, size_t offset, size_t size) {
    weft_ggml_span_t* span = (weft_ggml_span_t*)buffer->context;
    const struct weft_ggml_tensor_probe* t =
        (const struct weft_ggml_tensor_probe*)tensor;
    if (!span || !t || !t->data) return;
    uint64_t src = (uint64_t)((const uint8_t*)t->data - span->base) + offset;
    if (src + size > span->bytes) return;
    memcpy(data, (const uint8_t*)t->data + offset, size);
}

static void buf_clear(ggml_backend_buffer_t buffer, uint8_t value) {
    weft_ggml_span_t* span = (weft_ggml_span_t*)buffer->context;
    if (span) memset(span->base, value, (size_t)span->bytes);
}

static ggml_backend_buffer_t buft_alloc_buffer(
    ggml_backend_buffer_type_t buft, size_t size) {
    struct ggml_backend_buffer_type* b =
        (struct ggml_backend_buffer_type*)buft;
    weft_ggml_rt_t* rt = (weft_ggml_rt_t*)b->context;
    if (!rt || !rt->buft_planner || !rt->ggml_backend_buffer_init) {
        return NULL;
    }
    // First span with room (the honest ladder: full spans refuse NULL —
    // ggml's allocator treats that as allocation failure, which it is).
    for (int s = 0; s < rt->buft_planner->n; s++) {
        if (rt->buft_planner->spans[s].bytes >= size) {
            struct ggml_backend_buffer_i iface = {0};
            iface.get_base = buf_get_base;
            iface.init_tensor = buf_init_tensor;
            iface.set_tensor = buf_set_tensor;
            iface.get_tensor = buf_get_tensor;
            iface.clear = buf_clear;
            // free_buffer/memset_tensor/cpy_tensor/reset stay NULL — the
            // documented optionals (memory is the ring's; we never free).
            return rt->ggml_backend_buffer_init(buft, iface,
                                                &rt->buft_planner->spans[s],
                                                size);
        }
    }
    return NULL;
}

void* weft_ggml_buffer_type(weft_ggml_rt_t* rt,
                            weft_ggml_planner_t* planner) {
    if (!rt || !planner || planner->n == 0) return NULL;
    if (!rt->ggml_backend_buffer_init) return NULL;  // OPAQUE_ERA road
    if (!rt->probe_ok) return NULL;                  // ABI disagreement
    rt->buft.iface.get_name = buft_get_name;
    rt->buft.iface.alloc_buffer = buft_alloc_buffer;
    rt->buft.iface.get_alignment = buft_get_alignment;
    rt->buft.iface.get_max_size = buft_get_max_size;
    rt->buft.iface.get_alloc_size = NULL;   // default (ggml_nbytes)
    rt->buft.iface.is_host = buft_is_host;
    rt->buft.device = NULL;
    rt->buft.context = rt;
    rt->buft_planner = planner;
    rt->buft_bound = 1;
    return &rt->buft;
}

// ---------------------------------------------------------------------------
// The data-pointer road (every era)
// ---------------------------------------------------------------------------

weft_ggml_err_t weft_ggml_place_view(weft_ggml_rt_t* rt,
                                     weft_ggml_planner_t* p,
                                     const weft_tensor_view_t* v,
                                     void** out_data,
                                     weft_ggml_shape_t* out_shape) {
    if (!p || !v || !out_data || !out_shape) {
        return WEFT_GGML_ERR_BAD_ARG;
    }
    weft_ggml_err_t e = WEFT_GGML_ERR_OK;
    if (weft_ggml_shape_of_view(v, out_shape) != 0) {
        return WEFT_GGML_ERR_VIEW;
    }
    int span = -1;
    uint64_t off = 0;
    if (weft_ggml_planner_place(p, out_shape->nbytes, &span, &off) != 0) {
        return WEFT_GGML_ERR_PLAN;  // full spans — the [FALLBACK-COPY]
                                    // signal (counted in the planner)
    }
    *out_data = p->spans[span].base + off;
    (void)rt;  // the data-pointer road needs no library (the plan layer
               // IS the contract; rt rides along for the report line)
    return e;
}
