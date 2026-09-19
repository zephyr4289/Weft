// gpu/gpu_layout.h — WGSL/GLSL buffer alignment engine (Law 4 machinery).
//
// One engine, two languages (WGSL, GLSL), two address spaces (storage ==
// std430-equivalent, uniform == std140-equivalent). It walks the HOST layout
// and decides, member by member:
//   * can this host field sit at its host offset under GPU alignment rules?
//   * which explicit pad members must be inserted to skip host gaps?
//   * does the resulting GPU struct size equal the host size?
// Any "no" -> the struct is marked not-exact with a precise reason and the
// emitters REFUSE it (total refusal, never partial).
//
// The rules implemented here are deliberately re-implemented INDEPENDENTLY in
// tests/gpu_layout_check.c, which parses the emitted WGSL/GLSL text and
// recomputes offsets from scratch — double-entry verification: the emitter
// and the checker agree or the gate fails.

#ifndef WEFTC_GPU_LAYOUT_H
#define WEFTC_GPU_LAYOUT_H

#include "../weftc_codegen.h"

typedef enum { GPU_WGSL, GPU_GLSL } gpu_lang;
typedef enum { GPU_STORAGE, GPU_UNIFORM } gpu_space;

#define GPU_MAX_MEMBERS 160

typedef struct {
    bool is_pad;
    char name[64];          // host field name, or weft_padN for synthesized pads
    char wgsl_type[128];
    char glsl_type[128];
    uint32_t host_offset;   // real fields only
    uint32_t offset;        // computed GPU offset
    uint32_t size;          // computed GPU member size
    uint32_t align;         // computed GPU member alignment
} gpu_member;

typedef struct {
    gpu_member members[GPU_MAX_MEMBERS];
    uint32_t nmembers;
    uint32_t size;          // computed GPU struct size (roundUp(align, end))
    uint32_t align;         // computed GPU struct alignment
    bool exact;             // byte-exact with the host layout?
    char reason[512];       // first violation, human-readable (Law 4 warning)
    bool uses_f16;          // WGSL: needs `enable f16;`
    bool uses_u64;          // GLSL: needs GL_ARB_gpu_shader_int64
    bool uses_f64;          // GLSL: needs GL_ARB_gpu_shader_fp64
} gpu_layout;

// Computes the layout of `s` under (lang, space). Returns true when exact.
// When not exact, `out` still carries the reason (and whatever members were
// placed before the refusal point — the emitters ignore them).
bool gpu_layout_compute(const weft_ir* ir, const weft_struct* s,
                        gpu_lang lang, gpu_space space, gpu_layout* out);

const char* gpu_space_name(gpu_space s);
const char* gpu_lang_name(gpu_lang l);

#endif // WEFTC_GPU_LAYOUT_H
