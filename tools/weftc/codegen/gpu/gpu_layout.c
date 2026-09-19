// gpu/gpu_layout.c — the alignment engine (see gpu_layout.h).
//
// Alignment rules implemented (normative for v1):
//
//   WGSL storage / GLSL std430:
//     scalar align = size (u32/f32: 4, u64/i64: 8, f16: 2)
//     vec2<T> align = 2*align(T); vec3/vec4 align = 4*align(T)
//     mat4x4<f32>   align 16, size 64
//     array<T,N>    align = align(T); stride = roundUp(align(T), size(T))
//     struct        align = max(member aligns); size = roundUp(align, end)
//
//   WGSL uniform / GLSL std140 (additionally):
//     u64/i64 and f16/f64 members refused (not portable in uniform buffers)
//     array stride rounds up to 16
//     struct-member alignment rounds up to 16
//
//   Vocabulary gaps (why 8/16-bit hosts are refused): WGSL and GLSL provide
//   no 8/16-bit integer buffer types, and synthesized pads are u32-only (a
//   2-byte pad would itself require f16 in WGSL and float16_t extensions in
//   GLSL — refused rather than risked).

#include "gpu_layout.h"

static uint32_t gpu_round_up(uint32_t v, uint32_t a)
{
    return ((v + a - 1) / a) * a;
}

const char* gpu_space_name(gpu_space s)
{
    return s == GPU_STORAGE ? "storage (std430-equivalent)" : "uniform (std140-equivalent)";
}

const char* gpu_lang_name(gpu_lang l)
{
    return l == GPU_WGSL ? "wgsl" : "glsl";
}

typedef struct {
    bool ok;                // expressible in this (lang, space)?
    uint32_t align, size;
    const char* wgsl;       // type text
    const char* glsl;
    char wgsl_buf[160];     // when the text must be composed (arrays)
    char glsl_buf[160];
    char reason[480];       // refusal reason when !ok
} gpu_type_info;

static gpu_type_info gpu_scalar_info(weft_type_kind k, gpu_lang lang, gpu_space space)
{
    gpu_type_info t;
    memset(&t, 0, sizeof(t));
    t.ok = true;
    switch (k) {
    case WT_U8: case WT_I8:
        t.ok = false;
        snprintf(t.reason, sizeof(t.reason),
                 "WGSL and GLSL expose no 8-bit integer buffer types; widen to u32");
        return t;
    case WT_U16: case WT_I16:
        t.ok = false;
        snprintf(t.reason, sizeof(t.reason),
                 "WGSL and GLSL expose no 16-bit integer buffer types; widen to u32 (or keep this struct CPU-side)");
        return t;
    case WT_BOOL:
        t.ok = false;
        snprintf(t.reason, sizeof(t.reason),
                 "host bool (1 byte) is not portably host-shareable; use u32 flags + bitfields");
        return t;
    case WT_U32: t.align = 4; t.size = 4; t.wgsl = "u32";  t.glsl = "uint";  return t;
    case WT_I32: t.align = 4; t.size = 4; t.wgsl = "i32";  t.glsl = "int";   return t;
    case WT_F32: t.align = 4; t.size = 4; t.wgsl = "f32";  t.glsl = "float"; return t;
    case WT_U64:
    case WT_I64:
        if (space == GPU_UNIFORM) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason),
                     "u64/i64 are not permitted in the uniform address space (WGSL; std140 has no 64-bit integers) — use the storage variant");
            return t;
        }
        t.align = 8; t.size = 8;
        t.wgsl = (k == WT_U64) ? "u64" : "i64";
        t.glsl = (k == WT_U64) ? "uint64_t" : "int64_t";
        return t;
    case WT_F16:
        if (lang == GPU_GLSL) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason),
                     "GLSL struct members of float16_t need rarely-enabled extensions; use f32 or the wgsl target");
            return t;
        }
        if (space == GPU_UNIFORM) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason),
                     "f16 in the uniform address space is not portable across WGSL implementations — use the storage variant");
            return t;
        }
        t.align = 2; t.size = 2; t.wgsl = "f16"; t.glsl = "float16_t";
        return t;
    case WT_F64:
        if (lang == GPU_WGSL) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason), "WGSL has no f64");
            return t;
        }
        if (space == GPU_UNIFORM) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason),
                     "f64 in uniform buffers needs GL_ARB_gpu_shader_fp64 and is refused for portability");
            return t;
        }
        t.align = 8; t.size = 8; t.wgsl = "f64"; t.glsl = "double";
        return t;
    case WT_VEC2F32: t.align = 8;  t.size = 8;  t.wgsl = "vec2<f32>"; t.glsl = "vec2";  return t;
    case WT_VEC3F32: t.align = 16; t.size = 12; t.wgsl = "vec3<f32>"; t.glsl = "vec3";  return t;
    case WT_VEC4F32: t.align = 16; t.size = 16; t.wgsl = "vec4<f32>"; t.glsl = "vec4";  return t;
    case WT_VEC2F16:
        if (lang == GPU_GLSL || space == GPU_UNIFORM) goto f16_refuse;
        t.align = 4; t.size = 4; t.wgsl = "vec2<f16>"; t.glsl = "f16vec2";
        return t;
    case WT_VEC3F16:
        if (lang == GPU_GLSL || space == GPU_UNIFORM) goto f16_refuse;
        t.align = 8; t.size = 6; t.wgsl = "vec3<f16>"; t.glsl = "f16vec3";
        return t;
    case WT_VEC4F16:
        if (lang == GPU_GLSL || space == GPU_UNIFORM) goto f16_refuse;
        t.align = 8; t.size = 8; t.wgsl = "vec4<f16>"; t.glsl = "f16vec4";
        return t;
    default:
        t.ok = false;
        snprintf(t.reason, sizeof(t.reason), "type not supported in GPU targets");
        return t;
    }
f16_refuse:
    t.ok = false;
    snprintf(t.reason, sizeof(t.reason),
             "f16 vectors are WGSL-storage-only (enable f16); GLSL/uniform refused for portability");
    return t;
}

// info for one FIELD (resolves arrays, gpu_type overrides, nested structs)
static gpu_type_info gpu_field_info(const weft_ir* ir, const weft_field* f,
                                    gpu_lang lang, gpu_space space,
                                    bool* uses_f16, bool* uses_u64, bool* uses_f64)
{
    gpu_type_info t;
    memset(&t, 0, sizeof(t));
    t.ok = true;

    // gpu_type override: mat4x4<f32> over [f32; 16]
    if (f->gpu_type && strcmp(f->gpu_type, "mat4x4<f32>") == 0) {
        t.align = 16; t.size = 64;
        t.wgsl = "mat4x4<f32>";
        t.glsl = "mat4";
        return t;
    }

    if (f->kind == WT_ARRAY) {
        // element info via a scalar/vector/struct proxy of the element kind
        weft_field proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.kind = f->elem_kind;
        proxy.count = f->count;
        proxy.struct_ref = f->struct_ref;
        proxy.struct_type = f->struct_type;
        proxy.gpu_type = NULL;
        gpu_type_info e = gpu_field_info(ir, &proxy, lang, space, uses_f16, uses_u64, uses_f64);
        if (!e.ok) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason), "array element: %.400s", e.reason);
            return t;
        }
        uint32_t stride = gpu_round_up(e.align, e.size);
        if (space == GPU_UNIFORM) {
            stride = gpu_round_up(stride, 16);
        }
        t.align = e.align;
        if (space == GPU_UNIFORM) {
            t.align = gpu_round_up(e.align, 16);
        }
        t.size = stride * f->count;
        if (t.size != f->size) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason),
                     "array<%.60s, %u>: %s stride %u x %u = %u bytes != host %u bytes",
                     lang == GPU_WGSL ? e.wgsl_buf[0] ? e.wgsl_buf : e.wgsl : (e.glsl_buf[0] ? e.glsl_buf : e.glsl),
                     f->count, gpu_space_name(space), stride, f->count, t.size, f->size);
            return t;
        }
        if (lang == GPU_WGSL) {
            snprintf(t.wgsl_buf, sizeof(t.wgsl_buf), "array<%.100s, %u>",
                     e.wgsl_buf[0] ? e.wgsl_buf : e.wgsl, f->count);
            t.wgsl = t.wgsl_buf;
            snprintf(t.glsl_buf, sizeof(t.glsl_buf), "%.100s[%u]",
                     e.glsl_buf[0] ? e.glsl_buf : e.glsl, f->count);
            t.glsl = t.glsl_buf;
        } else {
            snprintf(t.glsl_buf, sizeof(t.glsl_buf), "%.100s[%u]",
                     e.glsl_buf[0] ? e.glsl_buf : e.glsl, f->count);
            t.glsl = t.glsl_buf;
            snprintf(t.wgsl_buf, sizeof(t.wgsl_buf), "array<%.100s, %u>",
                     e.wgsl_buf[0] ? e.wgsl_buf : e.wgsl, f->count);
            t.wgsl = t.wgsl_buf;
        }
        return t;
    }

    if (f->kind == WT_STRUCT) {
        gpu_layout dep;
        memset(&dep, 0, sizeof(dep));
        bool dep_exact = gpu_layout_compute(ir, f->struct_type, lang, space, &dep);
        if (!dep_exact) {
            t.ok = false;
            snprintf(t.reason, sizeof(t.reason),
                     "embedded struct %s is not GPU-exact: %.380s", f->struct_ref, dep.reason);
            return t;
        }
        t.align = dep.align;
        t.size = dep.size;
        if (space == GPU_UNIFORM) {
            t.align = gpu_round_up(dep.align, 16);
            if (gpu_round_up(t.align, dep.size) != dep.size) {
                t.ok = false;
                snprintf(t.reason, sizeof(t.reason),
                         "embedded struct %s: uniform alignment %u rounds member size past host %u",
                         f->struct_ref, t.align, dep.size);
                return t;
            }
        }
        snprintf(t.wgsl_buf, sizeof(t.wgsl_buf), "%s", f->struct_type->rust_name);
        t.wgsl = t.wgsl_buf;
        snprintf(t.glsl_buf, sizeof(t.glsl_buf), "Weft%s", f->struct_type->rust_name);
        t.glsl = t.glsl_buf;
        return t;
    }

    t = gpu_scalar_info(f->kind, lang, space);
    if (t.ok) {
        if (f->kind == WT_F16 || (f->kind >= WT_VEC2F16 && f->kind <= WT_VEC4F16)) *uses_f16 = true;
        if (f->kind == WT_U64 || f->kind == WT_I64) *uses_u64 = true;
        if (f->kind == WT_F64) *uses_f64 = true;
    }
    return t;
}

bool gpu_layout_compute(const weft_ir* ir, const weft_struct* s,
                        gpu_lang lang, gpu_space space, gpu_layout* out)
{
    memset(out, 0, sizeof(*out));
    out->exact = true;

    uint32_t cur = 0;
    int pad_idx = 0;

    for (uint32_t fi = 0; fi < s->nfields; fi++) {
        const weft_field* f = &s->fields[fi];
        gpu_type_info ti = gpu_field_info(ir, f, lang, space,
                                          &out->uses_f16, &out->uses_u64, &out->uses_f64);
        if (!ti.ok) {
            out->exact = false;
            snprintf(out->reason, sizeof(out->reason), "field %s (%s): %s",
                     f->name, f->kind == WT_ARRAY || f->kind == WT_STRUCT ? "aggregate" : "scalar", ti.reason);
            return false;
        }
        if (f->offset % ti.align != 0) {
            out->exact = false;
            snprintf(out->reason, sizeof(out->reason),
                     "field %s: host offset %u violates %s alignment %u",
                     f->name, f->offset, gpu_space_name(space), ti.align);
            return false;
        }
        // pad the gap between cur and the field's host offset
        while (cur < f->offset) {
            uint32_t gap = f->offset - cur;
            if ((gap % 4u) != 0u) {
                out->exact = false;
                snprintf(out->reason, sizeof(out->reason),
                         "gap of %u byte(s) at offset %u before %s is not expressible: WGSL/GLSL pads are u32-only",
                         gap, cur, f->name);
                return false;
            }
            uint32_t n = gap / 4u;
            if (space == GPU_UNIFORM) {
                // arrays round strides to 16 in uniform — pads must be scalar u32s
                for (uint32_t p = 0; p < n; p++) {
                    if (out->nmembers >= GPU_MAX_MEMBERS) goto too_many;
                    gpu_member* m = &out->members[out->nmembers++];
                    memset(m, 0, sizeof(*m));
                    m->is_pad = true;
                    snprintf(m->name, sizeof(m->name), "weft_pad%d", pad_idx++);
                    snprintf(m->wgsl_type, sizeof(m->wgsl_type), "u32");
                    snprintf(m->glsl_type, sizeof(m->glsl_type), "uint");
                    m->offset = cur;
                    m->size = 4;
                    m->align = 4;
                    cur += 4;
                }
            } else {
                if (out->nmembers >= GPU_MAX_MEMBERS) goto too_many;
                gpu_member* m = &out->members[out->nmembers++];
                memset(m, 0, sizeof(*m));
                m->is_pad = true;
                snprintf(m->name, sizeof(m->name), "weft_pad%d", pad_idx++);
                if (n == 1u) {
                    snprintf(m->wgsl_type, sizeof(m->wgsl_type), "u32");
                    snprintf(m->glsl_type, sizeof(m->glsl_type), "uint");
                } else {
                    snprintf(m->wgsl_type, sizeof(m->wgsl_type), "array<u32, %u>", n);
                    snprintf(m->glsl_type, sizeof(m->glsl_type), "uint[%u]", n);
                }
                m->offset = cur;
                m->size = gap;
                m->align = 4;
                cur += gap;
            }
        }
        if (out->nmembers >= GPU_MAX_MEMBERS) goto too_many;
        gpu_member* m = &out->members[out->nmembers++];
        memset(m, 0, sizeof(*m));
        m->is_pad = false;
        snprintf(m->name, sizeof(m->name), "%s", f->name);
        snprintf(m->wgsl_type, sizeof(m->wgsl_type), "%s", ti.wgsl);
        snprintf(m->glsl_type, sizeof(m->glsl_type), "%s", ti.glsl);
        m->host_offset = f->offset;
        m->offset = f->offset;
        m->size = ti.size;
        m->align = ti.align;
        if (ti.size != f->size) {
            out->exact = false;
            snprintf(out->reason, sizeof(out->reason),
                     "field %s: GPU member size %u != host size %u", f->name, ti.size, f->size);
            return false;
        }
        cur = f->offset + f->size;
    }

    // tail padding to the host struct size
    while (cur < s->size) {
        uint32_t gap = s->size - cur;
        if ((gap % 4u) != 0u) {
            out->exact = false;
            snprintf(out->reason, sizeof(out->reason),
                     "tail gap of %u byte(s) at offset %u is not expressible: WGSL/GLSL pads are u32-only",
                     gap, cur);
            return false;
        }
        uint32_t n = gap / 4u;
        if (space == GPU_UNIFORM) {
            for (uint32_t p = 0; p < n; p++) {
                if (out->nmembers >= GPU_MAX_MEMBERS) goto too_many;
                gpu_member* m = &out->members[out->nmembers++];
                memset(m, 0, sizeof(*m));
                m->is_pad = true;
                snprintf(m->name, sizeof(m->name), "weft_pad%d", pad_idx++);
                snprintf(m->wgsl_type, sizeof(m->wgsl_type), "u32");
                snprintf(m->glsl_type, sizeof(m->glsl_type), "uint");
                m->offset = cur;
                m->size = 4;
                m->align = 4;
                cur += 4;
            }
        } else {
            if (out->nmembers >= GPU_MAX_MEMBERS) goto too_many;
            gpu_member* m = &out->members[out->nmembers++];
            memset(m, 0, sizeof(*m));
            m->is_pad = true;
            snprintf(m->name, sizeof(m->name), "weft_pad%d", pad_idx++);
            if (n == 1u) {
                snprintf(m->wgsl_type, sizeof(m->wgsl_type), "u32");
                snprintf(m->glsl_type, sizeof(m->glsl_type), "uint");
            } else {
                snprintf(m->wgsl_type, sizeof(m->wgsl_type), "array<u32, %u>", n);
                snprintf(m->glsl_type, sizeof(m->glsl_type), "uint[%u]", n);
            }
            m->offset = cur;
            m->size = gap;
            m->align = 4;
            cur += gap;
        }
    }

    // struct alignment + size
    uint32_t galign = 1;
    for (uint32_t m = 0; m < out->nmembers; m++) {
        if (out->members[m].align > galign) galign = out->members[m].align;
    }
    out->align = galign;
    out->size = gpu_round_up(galign, cur);
    if (out->size != s->size) {
        out->exact = false;
        snprintf(out->reason, sizeof(out->reason),
                 "GPU struct size %u (alignment %u rounds the end) != host size %u",
                 out->size, galign, s->size);
        return false;
    }
    return true;

too_many:
    out->exact = false;
    snprintf(out->reason, sizeof(out->reason), "member budget (%d) exceeded", GPU_MAX_MEMBERS);
    return false;
}
