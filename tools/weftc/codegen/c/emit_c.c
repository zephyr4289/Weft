// c/emit_c.c — weftc-codegen backend: C11 single-header projection (target c).
//
// What this emitter guarantees for every verified struct:
//   * a standalone, self-contained .h (the shared projection core is embedded
//     behind its own include guard, so any subset of headers compiles alone
//     and all of them compile together)
//   * compile-time ABI guards — static_assert on sizeof + offsetof for EVERY
//     field, exactly the pattern from the Pillar 1 briefing
//   * zero-copy cast helpers validating length, alignment and the 64-bit (or
//     32-bit) schema hash header before projecting — no allocation, ~4 ops
//   * alignment-free packed readers/writers (byte-assembly, Law 3) so packed
//     DMA/UMEM sources and destinations are safe on every architecture
//   * bitfield accessors and fixed-array LEN macros (innovation vectors)
//   * SIMD batch schema validation: AVX2 (x86), NEON (aarch64), scalar
//     everywhere — the baseline C11 target pulls in ONLY the five standard
//     headers; <immintrin.h>/<arm_neon.h> appear solely behind
//     compiler-defined capability macros

#include "../weftc_codegen.h"

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static const char* c_scalar_type(weft_type_kind k)
{
    switch (k) {
    case WT_U8: return "uint8_t";
    case WT_I8: return "int8_t";
    case WT_U16: return "uint16_t";
    case WT_I16: return "int16_t";
    case WT_U32: return "uint32_t";
    case WT_I32: return "int32_t";
    case WT_U64: return "uint64_t";
    case WT_I64: return "int64_t";
    case WT_F16: return "weft_f16_t";
    case WT_F32: return "float";
    case WT_F64: return "double";
    case WT_BOOL: return "bool";
    default: return "?";
    }
}

static bool is_vec_kind(weft_type_kind k)
{
    return (k >= WT_VEC2F32 && k <= WT_VEC4F32) || (k >= WT_VEC2F16 && k <= WT_VEC4F16);
}

static uint32_t vec_len(weft_type_kind k)
{
    switch (k) {
    case WT_VEC2F32: case WT_VEC2F16: return 2;
    case WT_VEC3F32: case WT_VEC3F16: return 3;
    default: return 4;
    }
}

static bool is_f16_vec(weft_type_kind k)
{
    return k >= WT_VEC2F16 && k <= WT_VEC4F16;
}

// declarator parts for a field: base type + array suffix. C syntax puts the
// brackets AFTER the field name: "float accel[3]" — base "float", suffix "[3]".
static void c_field_parts(const weft_field* f, char base[96], char suffix[80])
{
    suffix[0] = '\0';
    switch (f->kind) {
    case WT_VEC2F32: strcpy(base, "float"); strcpy(suffix, "[2]"); break;
    case WT_VEC3F32: strcpy(base, "float"); strcpy(suffix, "[3]"); break;
    case WT_VEC4F32: strcpy(base, "float"); strcpy(suffix, "[4]"); break;
    case WT_VEC2F16: strcpy(base, "weft_f16_t"); strcpy(suffix, "[2]"); break;
    case WT_VEC3F16: strcpy(base, "weft_f16_t"); strcpy(suffix, "[3]"); break;
    case WT_VEC4F16: strcpy(base, "weft_f16_t"); strcpy(suffix, "[4]"); break;
    case WT_ARRAY: {
        if (is_vec_kind(f->elem_kind)) {
            // array of vectors: float roi[COUNT][VECLEN] — contiguous, host-order
            snprintf(base, 96, "%s", is_f16_vec(f->elem_kind) ? "weft_f16_t" : "float");
            snprintf(suffix, 80, "[%u][%u]", f->count, vec_len(f->elem_kind));
            break;
        }
        weft_field tmp = *f;
        tmp.kind = f->elem_kind;
        char ebase[96], esuffix[80];
        c_field_parts(&tmp, ebase, esuffix);
        snprintf(base, 96, "%s", ebase);
        snprintf(suffix, 80, "[%u]%.40s", f->count, esuffix);
        break;
    }
    case WT_STRUCT:
        snprintf(base, 96, "%s", f->struct_type->c_name);
        break;
    default:
        snprintf(base, 96, "%s", c_scalar_type(f->kind));
        break;
    }
}

// "weft_<name>" — the symbol stem (no _t) for generated functions
static void c_stem(const weft_struct* st, char out[96])
{
    snprintf(out, 96, "weft_%s", st->name);
}

// element accessor kind for packed read/write codegen
typedef enum { EL_U8, EL_I8, EL_U16, EL_I16, EL_U32, EL_I32, EL_U64, EL_I64, EL_F16, EL_F32, EL_F64, EL_BOOL, EL_STRUCT, EL_BAD } elem_flavor;

static elem_flavor elem_flavor_of(weft_type_kind k)
{
    switch (k) {
    case WT_U8: return EL_U8;
    case WT_I8: return EL_I8;
    case WT_U16: return EL_U16;
    case WT_I16: return EL_I16;
    case WT_U32: return EL_U32;
    case WT_I32: return EL_I32;
    case WT_U64: return EL_U64;
    case WT_I64: return EL_I64;
    case WT_F16: return EL_F16;
    case WT_F32: return EL_F32;
    case WT_F64: return EL_F64;
    case WT_BOOL: return EL_BOOL;
    default: return EL_BAD;
    }
}

static const char* read_fn(elem_flavor e)
{
    switch (e) {
    case EL_U8: return "p[%s]"; // handled inline
    case EL_I8: return "(int8_t)p[%s]";
    case EL_U16: return "weft_read_u16_le(%s)";
    case EL_I16: return "(int16_t)weft_read_u16_le(%s)";
    case EL_U32: return "weft_read_u32_le(%s)";
    case EL_I32: return "(int32_t)weft_read_u32_le(%s)";
    case EL_U64: return "weft_read_u64_le(%s)";
    case EL_I64: return "(int64_t)weft_read_u64_le(%s)";
    case EL_F16: return "(weft_f16_t)weft_read_u16_le(%s)";
    case EL_F32: return "weft_read_f32_le(%s)";
    case EL_F64: return "weft_read_f64_le(%s)";
    case EL_BOOL: return "(bool)(p[%s] != 0u)";
    default: return NULL;
    }
}

// emit one field's packed-read statement(s) into body
static void emit_field_read(strbuf* body, const weft_field* f, const char* dst,
                            const char* src, uint32_t off)
{
    if (f->kind == WT_STRUCT) {
        char stem[96];
        c_stem(f->struct_type, stem);
        wsb_printf(body, "    {\n"
                         "        weft_error_t sub = %s_read_packed(%s + %uu, (len > %uu) ? (len - %uu) : 0u, &%s);\n"
                         "        if (sub != WEFT_OK) { return sub; }\n"
                         "    }\n",
                   stem, src, off, off, off, dst);
        return;
    }
    weft_type_kind ek = (f->kind == WT_ARRAY) ? f->elem_kind : f->kind;
    if (f->kind == WT_ARRAY && ek == WT_STRUCT) {
        char stem[96];
        c_stem(f->struct_type, stem);
        wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", f->count);
        wsb_printf(body, "        weft_error_t sub = %s_read_packed(%s + %uu + k * %uu, (len > %uu) ? (len - %uu) : 0u, &%s[k]);\n"
                         "        if (sub != WEFT_OK) { return sub; }\n"
                         "    }\n",
                   stem, src, off, f->struct_type->size,
                   off, off, dst);
        return;
    }
    if (f->kind == WT_ARRAY && is_vec_kind(ek)) {
        // array of vectors: float roi[COUNT][VECLEN] — nested-loop scalar reads
        const uint32_t vl = vec_len(ek);
        const bool f16 = is_f16_vec(ek);
        const uint32_t esz = f16 ? 2u : 4u;
        wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", f->count);
        wsb_printf(body, "        for (uint32_t j = 0u; j < %uu; j++) {\n", vl);
        if (f16) {
            wsb_printf(body, "            %s[k][j] = (weft_f16_t)weft_read_u16_le(%s + %uu + (k * %uu + j) * %uu);\n",
                       dst, src, off, vl, esz);
        } else {
            wsb_printf(body, "            %s[k][j] = weft_read_f32_le(%s + %uu + (k * %uu + j) * %uu);\n",
                       dst, src, off, vl, esz);
        }
        wsb_printf(body, "        }\n    }\n");
        return;
    }
    elem_flavor e = elem_flavor_of(ek);
    char addr[96];
    if (f->kind == WT_ARRAY) {
        // arrays/vectors: per-element loop
        uint32_t esz = 0;
        switch (e) {
        case EL_U8: case EL_I8: case EL_BOOL: esz = 1; break;
        case EL_U16: case EL_I16: case EL_F16: esz = 2; break;
        case EL_U32: case EL_I32: case EL_F32: esz = 4; break;
        default: esz = 8; break;
        }
        wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", f->count);
        snprintf(addr, sizeof(addr), "%s + %uu + k * %uu", src, off, esz);
        const char* rf = read_fn(e);
        char expr[160];
        if (e == EL_U8 || e == EL_I8 || e == EL_BOOL) {
            // rf contains "p[%s]" style with p fixed — special-case
            if (e == EL_BOOL) {
                snprintf(expr, sizeof(expr), "(bool)((%s)[0] != 0u)", addr);
            } else if (e == EL_U8) {
                snprintf(expr, sizeof(expr), "(%s)[0]", addr);
            } else {
                snprintf(expr, sizeof(expr), "(int8_t)(%s)[0]", addr);
            }
        } else {
            snprintf(expr, sizeof(expr), rf, addr);
        }
        wsb_printf(body, "        %s[k] = %s;\n    }\n", dst, expr);
        return;
    }
    // scalar / vector (vectors are plain C arrays)
    if (ek == WT_VEC2F32 || ek == WT_VEC3F32 || ek == WT_VEC4F32 ||
        ek == WT_VEC2F16 || ek == WT_VEC3F16 || ek == WT_VEC4F16) {
        uint32_t cnt = 0;
        uint32_t esz = 0;
        elem_flavor ve;
        if (ek == WT_VEC2F32) { cnt = 2; esz = 4; ve = EL_F32; }
        else if (ek == WT_VEC3F32) { cnt = 3; esz = 4; ve = EL_F32; }
        else if (ek == WT_VEC4F32) { cnt = 4; esz = 4; ve = EL_F32; }
        else if (ek == WT_VEC2F16) { cnt = 2; esz = 2; ve = EL_F16; }
        else if (ek == WT_VEC3F16) { cnt = 3; esz = 2; ve = EL_F16; }
        else { cnt = 4; esz = 2; ve = EL_F16; }
        wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", cnt);
        snprintf(addr, sizeof(addr), "%s + %uu + k * %uu", src, off, esz);
        char expr[160];
        snprintf(expr, sizeof(expr), read_fn(ve), addr);
        wsb_printf(body, "        %s[k] = %s;\n    }\n", dst, expr);
        return;
    }
    snprintf(addr, sizeof(addr), "%s + %uu", src, off);
    char expr[160];
    if (e == EL_U8) {
        snprintf(expr, sizeof(expr), "%s[0]", addr);
    } else if (e == EL_I8) {
        snprintf(expr, sizeof(expr), "(int8_t)%s[0]", addr);
    } else if (e == EL_BOOL) {
        snprintf(expr, sizeof(expr), "(bool)(%s[0] != 0u)", addr);
    } else {
        snprintf(expr, sizeof(expr), read_fn(e), addr);
    }
    wsb_printf(body, "    %s = %s;\n", dst, expr);
}

static void emit_field_write(strbuf* body, const weft_field* f, const char* src,
                             const char* dst, uint32_t off)
{
    if (f->kind == WT_STRUCT) {
        char stem[96];
        c_stem(f->struct_type, stem);
        wsb_printf(body, "    {\n"
                         "        weft_error_t sub = %s_write_packed(%s + %uu, (len > %uu) ? (len - %uu) : 0u, &%s);\n"
                         "        if (sub != WEFT_OK) { return sub; }\n"
                         "    }\n",
                   stem, dst, off, off, off, src);
        return;
    }
    weft_type_kind ek = (f->kind == WT_ARRAY) ? f->elem_kind : f->kind;
    if (f->kind == WT_ARRAY && ek == WT_STRUCT) {
        char stem[96];
        c_stem(f->struct_type, stem);
        wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", f->count);
        wsb_printf(body, "        weft_error_t sub = %s_write_packed(%s + %uu + k * %uu, (len > %uu) ? (len - %uu) : 0u, &%s[k]);\n"
                         "        if (sub != WEFT_OK) { return sub; }\n"
                         "    }\n",
                   stem, dst, off, f->struct_type->size,
                   off, off, src);
        return;
    }
    if (f->kind == WT_ARRAY && is_vec_kind(ek)) {
        // array of vectors: nested-loop scalar writes
        const uint32_t vl = vec_len(ek);
        const bool f16 = is_f16_vec(ek);
        const uint32_t esz = f16 ? 2u : 4u;
        wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", f->count);
        wsb_printf(body, "        for (uint32_t j = 0u; j < %uu; j++) {\n", vl);
        if (f16) {
            wsb_printf(body, "            weft_write_u16_le(%s + %uu + (k * %uu + j) * %uu, (uint16_t)%s[k][j]);\n",
                       dst, off, vl, esz, src);
        } else {
            wsb_printf(body, "            weft_write_f32_le(%s + %uu + (k * %uu + j) * %uu, %s[k][j]);\n",
                       dst, off, vl, esz, src);
        }
        wsb_printf(body, "        }\n    }\n");
        return;
    }
    elem_flavor e = elem_flavor_of(ek);
    if (ek == WT_VEC2F32 || ek == WT_VEC3F32 || ek == WT_VEC4F32) {
        e = EL_F32; // vectors decompose to their scalar flavor for byte IO
    } else if (ek == WT_VEC2F16 || ek == WT_VEC3F16 || ek == WT_VEC4F16) {
        e = EL_F16;
    }
    uint32_t esz = 1;
    const char* wfn = NULL;
    switch (e) {
    case EL_U8: esz = 1; wfn = NULL; break;
    case EL_I8: esz = 1; wfn = NULL; break;
    case EL_BOOL: esz = 1; wfn = NULL; break;
    case EL_U16: case EL_I16: case EL_F16: esz = 2; wfn = "weft_write_u16_le"; break;
    case EL_U32: case EL_I32: case EL_F32: esz = 4; wfn = "weft_write_u32_le"; break;
    case EL_U64: case EL_I64: case EL_F64: esz = 8; wfn = "weft_write_u64_le"; break;
    default: break;
    }
    uint32_t cnt = 1;
    if (f->kind == WT_ARRAY) cnt = f->count;
    else if (ek == WT_VEC2F32 || ek == WT_VEC2F16) cnt = 2;
    else if (ek == WT_VEC3F32 || ek == WT_VEC3F16) cnt = 3;
    else if (ek == WT_VEC4F32 || ek == WT_VEC4F16) cnt = 4;

    const char* castty = (esz == 2) ? "uint16_t" : (esz == 4) ? "uint32_t" : "uint64_t";

    if (cnt == 1) {
        if (!wfn) {
            if (e == EL_BOOL) {
                wsb_printf(body, "    %s[%uu] = %s ? 1u : 0u;\n", dst, off, src);
            } else {
                wsb_printf(body, "    %s[%uu] = (uint8_t)%s;\n", dst, off, src);
            }
        } else if (e == EL_F32) {
            wsb_printf(body, "    weft_write_f32_le(%s + %uu, %s);\n", dst, off, src);
        } else if (e == EL_F64) {
            wsb_printf(body, "    weft_write_f64_le(%s + %uu, %s);\n", dst, off, src);
        } else {
            wsb_printf(body, "    %s(%s + %uu, (%s)%s);\n", wfn, dst, off, castty, src);
        }
        return;
    }
    wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n", cnt);
    if (!wfn) {
        if (e == EL_BOOL) {
            wsb_printf(body, "        %s[%uu + k * %uu] = %s[k] ? 1u : 0u;\n", dst, off, esz, src);
        } else {
            wsb_printf(body, "        %s[%uu + k * %uu] = (uint8_t)%s[k];\n", dst, off, esz, src);
        }
    } else if (e == EL_F32) {
        wsb_printf(body, "        weft_write_f32_le(%s + %uu + k * %uu, %s[k]);\n", dst, off, esz, src);
    } else if (e == EL_F64) {
        wsb_printf(body, "        weft_write_f64_le(%s + %uu + k * %uu, %s[k]);\n", dst, off, esz, src);
    } else {
        wsb_printf(body, "        %s(%s + %uu + k * %uu, (%s)%s[k]);\n", wfn, dst, off, esz, castty, src);
    }
    wsb_printf(body, "    }\n");
}

// synthesized pad descriptors (same walk as emit_pad_members)
typedef struct {
    uint32_t offset;
    uint32_t width;   // 8 / 4 / 2 / 1
    uint32_t count;   // gap / width (>= 1)
    int idx;
} pad_desc;

static uint32_t collect_pads(const weft_struct* s, pad_desc* pads, uint32_t max)
{
    uint32_t cursor = 0;
    uint32_t n = 0;
    int idx = 0;
    for (uint32_t f = 0; f < s->nfields && n < max; f++) {
        const weft_field* fl = &s->fields[f];
        while (cursor < fl->offset && n < max) {
            uint32_t gap = fl->offset - cursor;
            uint32_t w = 1;
            if (gap % 8u == 0u && cursor % 8u == 0u) w = 8;
            else if (gap % 4u == 0u && cursor % 4u == 0u) w = 4;
            else if (gap % 2u == 0u && cursor % 2u == 0u) w = 2;
            pads[n].offset = cursor;
            pads[n].width = w;
            pads[n].count = gap / w;
            pads[n].idx = idx++;
            n++;
            cursor = fl->offset;
        }
        cursor = fl->offset + fl->size;
    }
    while (cursor < s->size && n < max) {
        uint32_t gap = s->size - cursor;
        uint32_t w = 1;
        if (gap % 8u == 0u && cursor % 8u == 0u) w = 8;
        else if (gap % 4u == 0u && cursor % 4u == 0u) w = 4;
        else if (gap % 2u == 0u && cursor % 2u == 0u) w = 2;
        pads[n].offset = cursor;
        pads[n].width = w;
        pads[n].count = gap / w;
        pads[n].idx = idx++;
        n++;
        cursor = s->size;
    }
    return n;
}

// emit IO lines for every synthesized pad. Pads are ALWAYS arrays, so the
// loops below stay uniform for every width and count.
static void emit_pad_reads(strbuf* body, const weft_struct* s)
{
    pad_desc pads[64];
    uint32_t n = collect_pads(s, pads, 64);
    for (uint32_t p = 0; p < n; p++) {
        const char* rfn = pads[p].width == 8 ? "weft_read_u64_le"
                        : pads[p].width == 4 ? "weft_read_u32_le"
                        : pads[p].width == 2 ? "weft_read_u16_le" : NULL;
        if (rfn) {
            wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n"
                             "        out->weft_pad%d[k] = %s(p + %uu + k * %uu);\n"
                             "    }\n",
                       pads[p].count, pads[p].idx, rfn, pads[p].offset, pads[p].width);
        } else {
            wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n"
                             "        out->weft_pad%d[k] = p[%uu + k];\n"
                             "    }\n",
                       pads[p].count, pads[p].idx, pads[p].offset);
        }
    }
}

static void emit_pad_writes(strbuf* body, const weft_struct* s)
{
    pad_desc pads[64];
    uint32_t n = collect_pads(s, pads, 64);
    for (uint32_t p = 0; p < n; p++) {
        const char* wfn = pads[p].width == 8 ? "weft_write_u64_le"
                        : pads[p].width == 4 ? "weft_write_u32_le"
                        : pads[p].width == 2 ? "weft_write_u16_le" : NULL;
        const char* cty = pads[p].width == 8 ? "uint64_t"
                        : pads[p].width == 4 ? "uint32_t"
                        : pads[p].width == 2 ? "uint16_t" : "uint8_t";
        if (wfn) {
            wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n"
                             "        %s(p + %uu + k * %uu, (%s)src->weft_pad%d[k]);\n"
                             "    }\n",
                       pads[p].count, wfn, pads[p].offset, pads[p].width, cty, pads[p].idx);
        } else {
            wsb_printf(body, "    for (uint32_t k = 0u; k < %uu; k++) {\n"
                             "        p[%uu + k] = (uint8_t)src->weft_pad%d[k];\n"
                             "    }\n",
                       pads[p].count, pads[p].offset, pads[p].idx);
        }
    }
}

// ---------------------------------------------------------------------------
// the shared projection core (embedded in every header; self-guarded)
// ---------------------------------------------------------------------------

static void emit_core_block(strbuf* b, const char* core_upper)
{
    wsb_printf(b, "// ---- weft projection core (shared; self-guarded — safe in every header) ----\n");
    wsb_printf(b, "#ifndef %s_H\n#define %s_H\n\n", core_upper, core_upper);
    wsb_puts(b,
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"
        "#include <stdalign.h>\n"
        "#include <assert.h>\n"
        "\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n"
        "\n"
        "/// Projection validation codes. Identical across every weftc-generated\n"
        "/// header; legacy `rc == 0` checks remain correct (OK is 0).\n"
        "typedef enum {\n"
        "    WEFT_OK                  = 0,\n"
        "    WEFT_ERR_SHORT_BUFFER    = 1,  // len < struct size (refused before any read)\n"
        "    WEFT_ERR_BAD_ALIGN       = 2,  // buffer address violates struct alignment\n"
        "    WEFT_ERR_SCHEMA_MISMATCH = 3,  // schema hash header does not match\n"
        "} weft_error_t;\n"
        "\n"
        "// The wire format is byte-exact little-endian. Refuse big-endian hosts at\n"
        "// compile time rather than silently misreading every frame.\n"
        "#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)\n"
        "#error \"weft projection: big-endian host not supported (wire format is byte-exact LE)\"\n"
        "#endif\n"
        "\n"
        "/// Little-endian readers built from byte assembly — alignment-safe on every\n"
        "/// architecture (Law 3) and dependency-free (Law 2). Optimizing compilers\n"
        "/// fold these into a single load where the ISA allows it.\n"
        "static inline uint16_t weft_read_u16_le(const uint8_t* p)\n"
        "{\n"
        "    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));\n"
        "}\n"
        "static inline uint32_t weft_read_u32_le(const uint8_t* p)\n"
        "{\n"
        "    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);\n"
        "}\n"
        "static inline uint64_t weft_read_u64_le(const uint8_t* p)\n"
        "{\n"
        "    return (uint64_t)weft_read_u32_le(p) | ((uint64_t)weft_read_u32_le(p + 4) << 32);\n"
        "}\n"
        "static inline void weft_write_u16_le(uint8_t* p, uint16_t v)\n"
        "{\n"
        "    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8);\n"
        "}\n"
        "static inline void weft_write_u32_le(uint8_t* p, uint32_t v)\n"
        "{\n"
        "    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);\n"
        "    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)(v >> 24);\n"
        "}\n"
        "static inline void weft_write_u64_le(uint8_t* p, uint64_t v)\n"
        "{\n"
        "    weft_write_u32_le(p, (uint32_t)(v & 0xFFFFFFFFu));\n"
        "    weft_write_u32_le(p + 4, (uint32_t)(v >> 32));\n"
        "}\n"
        "\n");
    wsb_puts(b,
        "typedef union { uint32_t u; float f; } weft_f32_bits;\n"
        "typedef union { uint64_t u; double d; } weft_f64_bits;\n"
        "\n"
        "static inline float weft_read_f32_le(const uint8_t* p)\n"
        "{\n"
        "    weft_f32_bits b; b.u = weft_read_u32_le(p); return b.f;\n"
        "}\n"
        "static inline double weft_read_f64_le(const uint8_t* p)\n"
        "{\n"
        "    weft_f64_bits b; b.u = weft_read_u64_le(p); return b.d;\n"
        "}\n"
        "static inline void weft_write_f32_le(uint8_t* p, float v)\n"
        "{\n"
        "    weft_f32_bits b; b.f = v; weft_write_u32_le(p, b.u);\n"
        "}\n"
        "static inline void weft_write_f64_le(uint8_t* p, double v)\n"
        "{\n"
        "    weft_f64_bits b; b.d = v; weft_write_u64_le(p, b.u);\n"
        "}\n"
        "\n"
        "/// IEEE 754 binary16 (half) codec — round-to-nearest-even both directions,\n"
        "/// denormals and infinities exact. Zero dependencies: no <math.h>.\n"
        "typedef uint16_t weft_f16_t;\n"
        "static inline float weft_f16_to_f32(weft_f16_t h)\n"
        "{\n"
        "    const uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16;\n"
        "    const uint32_t exp  = ((uint32_t)(h >> 10)) & 0x1Fu;\n"
        "    uint32_t man = h & 0x3FFu;\n"
        "    uint32_t bits;\n"
        "    if (exp == 0u) {\n"
        "        if (man == 0u) {\n"
        "            bits = sign;                                    /* +-0 */\n"
        "        } else {\n"
        "            uint32_t shift = 0u;                            /* subnormal -> normal */\n"
        "            while ((man & 0x0400u) == 0u) { man <<= 1; shift++; }\n"
        "            bits = sign | ((113u - shift) << 23) | ((man & 0x03FFu) << 13);\n"
        "        }\n"
        "    } else if (exp == 0x1Fu) {\n"
        "        bits = sign | 0x7F800000u | (man ? (man << 13) : 0u);   /* inf / nan */\n"
        "    } else {\n"
        "        bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);\n"
        "    }\n"
        "    weft_f32_bits b; b.u = bits; return b.f;\n"
        "}\n"
        "static inline weft_f16_t weft_f32_to_f16(float f)\n"
        "{\n"
        "    weft_f32_bits b; b.f = f;\n"
        "    const uint32_t sign = (b.u >> 16) & 0x8000u;\n"
        "    const uint32_t be = (b.u >> 23) & 0xFFu;\n"
        "    const uint32_t man = b.u & 0x7FFFFFu;\n"
        "    const int32_t exp = (int32_t)be - 127;\n"
        "    if (be == 0xFFu) {\n"
        "        return (weft_f16_t)(sign | 0x7C00u | (man ? 0x0200u : 0u));  /* inf/nan */\n"
        "    }\n"
        "    if (exp > 15) {\n"
        "        return (weft_f16_t)(sign | 0x7C00u);                    /* overflow -> inf (RNE) */\n"
        "    }\n"
        "    if (exp >= -14) {\n"
        "        uint32_t r = ((uint32_t)(exp + 15) << 10) | (man >> 13);\n"
        "        const uint32_t rb = man & 0x1FFFu;\n"
        "        if (rb > 0x1000u || (rb == 0x1000u && (r & 1u))) { r++; }\n"
        "        return (weft_f16_t)(sign | r);\n"
        "    }\n"
        "    if (exp < -25) {\n"
        "        return (weft_f16_t)sign;                               /* underflow -> 0 */\n"
        "    }\n"
        "    {\n"
        "        const uint32_t m = man | 0x800000u;                    /* subnormal path */\n"
        "        const uint32_t shift = (uint32_t)(-14 - exp);\n"
        "        uint32_t r = m >> (13 + shift);\n"
        "        const uint32_t rb = m & (((uint32_t)1 << (13 + shift)) - 1u);\n"
        "        const uint32_t half = ((uint32_t)1 << (12 + shift));\n"
        "        if (rb > half || (rb == half && (r & 1u))) {\n"
        "            r++;\n"
        "            if (r == 0x0400u) { return (weft_f16_t)(sign | 0x0400u); }\n"
        "        }\n"
        "        return (weft_f16_t)(sign | r);\n"
        "    }\n"
        "}\n"
        "\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n"
        "\n");
    wsb_printf(b, "#endif // %s_H\n\n", core_upper);
}

// ---------------------------------------------------------------------------
// struct header emission
// ---------------------------------------------------------------------------

static void emit_pad_members(strbuf* b, uint32_t cursor, uint32_t target, int* pad_idx,
                             const char* first_prefix, bool* first_member)
{
    while (cursor < target) {
        uint32_t gap = target - cursor;
        const char* ty = "uint8_t";
        uint32_t w = 1;
        if (gap % 8u == 0u && cursor % 8u == 0u) { ty = "uint64_t"; w = 8; }
        else if (gap % 4u == 0u && cursor % 4u == 0u) { ty = "uint32_t"; w = 4; }
        else if (gap % 2u == 0u && cursor % 2u == 0u) { ty = "uint16_t"; w = 2; }
        uint32_t cnt = gap / w;
        const char* pre = *first_member ? first_prefix : "";
        // pads are ALWAYS arrays (uniform shape; packed IO treats them as
        // arrays of their scalar width so roundtrips stay byte-identical)
        wsb_printf(b, "    %s%s weft_pad%d[%u];            // @%u (%u)\n", pre, ty, *pad_idx, cnt, cursor, gap);
        *first_member = false;
        cursor += gap;
        (*pad_idx)++;
    }
}

static void emit_batch_validator(strbuf* b, const weft_struct* s)
{
    char up[160];
    weft_upper(s->name, up);

    wsb_printf(b,
        "/// Batch schema-hash scan over an array of packed frames. AVX2 / NEON when\n"
        "/// the compiler targets them, plain C11 scalar everywhere else (Law: the\n"
        "/// portable build pulls no extra headers). Reads are alignment-safe on all\n"
        "/// paths. On the first mismatching frame: returns WEFT_ERR_SCHEMA_MISMATCH,\n"
        "/// *out_bad_index = its index, *out_count = frames validated before it.\n"
        "static inline weft_error_t weft_validate_batch_%s(const void* buffer, size_t len,\n"
        "                                                 size_t* out_count, size_t* out_bad_index)\n"
        "{\n"
        "    const size_t stride = (size_t)WEFT_%s_SIZE;\n"
        "    if (out_count) { *out_count = 0u; }\n"
        "    if (out_bad_index) { *out_bad_index = 0u; }\n"
        "    if ((len %% stride) != 0u) { return WEFT_ERR_SHORT_BUFFER; }\n"
        "    const size_t n = len / stride;\n"
        "    const uint8_t* base = (const uint8_t*)buffer;\n"
        "    size_t i = 0u;\n",
        s->name, up);

    if (s->schema_width == 64) {
        wsb_printf(b,
            "#if defined(__AVX2__)\n"
            "    if (n >= 4u) {\n"
            "        const __m256i want = _mm256_set1_epi64x((long long)WEFT_%s_SCHEMA_ID);\n"
            "        for (; (i + 4u) <= n; i += 4u) {\n"
            "            const __m128i l0 = _mm_loadl_epi64((const __m128i*)(const void*)(base + (i + 0u) * stride));\n"
            "            const __m128i l1 = _mm_loadl_epi64((const __m128i*)(const void*)(base + (i + 1u) * stride));\n"
            "            const __m128i l2 = _mm_loadl_epi64((const __m128i*)(const void*)(base + (i + 2u) * stride));\n"
            "            const __m128i l3 = _mm_loadl_epi64((const __m128i*)(const void*)(base + (i + 3u) * stride));\n"
            "            const __m256i got = _mm256_set_epi64x(_mm_cvtsi128_si64(l3), _mm_cvtsi128_si64(l2),\n"
            "                                                  _mm_cvtsi128_si64(l1), _mm_cvtsi128_si64(l0));\n"
            "            const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi64(got, want));\n"
            "            if (mask != -1) {\n"
            "                for (int lane = 0; lane < 4; lane++) {\n"
            "                    // movemask_epi8: 8 mask bits per 64-bit lane\n"
            "                    if (((mask >> (lane * 8)) & 0xFF) != 0xFF) {\n"
            "                        if (out_count) { *out_count = i + (size_t)lane; }\n"
            "                        if (out_bad_index) { *out_bad_index = i + (size_t)lane; }\n"
            "                        return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "                    }\n"
            "                }\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "#elif defined(__ARM_NEON) && defined(__aarch64__)\n"
            "    if (n >= 2u) {\n"
            "        const uint64x2_t want = vdupq_n_u64((uint64_t)WEFT_%s_SCHEMA_ID);\n"
            "        for (; (i + 2u) <= n; i += 2u) {\n"
            "            const uint64x2_t got = vcombine_u64(\n"
            "                vld1_u64((const uint64_t*)(const void*)(base + (i + 0u) * stride)),\n"
            "                vld1_u64((const uint64_t*)(const void*)(base + (i + 1u) * stride)));\n"
            "            const uint64x2_t eq = vceqq_u64(got, want);\n"
            "            if (vgetq_lane_u64(eq, 0) == 0u) {\n"
            "                if (out_count) { *out_count = i; }\n"
            "                if (out_bad_index) { *out_bad_index = i; }\n"
            "                return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "            }\n"
            "            if (vgetq_lane_u64(eq, 1) == 0u) {\n"
            "                if (out_count) { *out_count = i + 1u; }\n"
            "                if (out_bad_index) { *out_bad_index = i + 1u; }\n"
            "                return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "#endif\n",
            up, up);
    } else {
        wsb_printf(b,
            "#if defined(__AVX2__)\n"
            "    if (n >= 8u) {\n"
            "        const __m256i want = _mm256_set1_epi32((int)WEFT_%s_SCHEMA_ID);\n"
            "        for (; (i + 8u) <= n; i += 8u) {\n"
            "            const __m256i got = _mm256_set_epi32(\n",
            up);
        for (int lane = 7; lane >= 0; lane--) {
            wsb_printf(b, "                (int)weft_read_u32_le(base + (i + %du) * stride)%s\n",
                       lane, lane == 0 ? ");" : ",");
        }
        wsb_printf(b,
            "            const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi32(got, want));\n"
            "            if (mask != -1) {\n"
            "                for (int lane = 0; lane < 8; lane++) {\n"
            "                    if (((mask >> (lane * 4)) & 0xF) != 0xF) {\n"
            "                        if (out_count) { *out_count = i + (size_t)lane; }\n"
            "                        if (out_bad_index) { *out_bad_index = i + (size_t)lane; }\n"
            "                        return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "                    }\n"
            "                }\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "#elif defined(__ARM_NEON)\n"
            "    if (n >= 4u) {\n"
            "        const uint32x4_t want = vdupq_n_u32((uint32_t)WEFT_%s_SCHEMA_ID);\n"
            "        for (; (i + 4u) <= n; i += 4u) {\n"
            "            const uint32x4_t got = vcombine_u32(\n"
            "                vcreate_u32((uint64_t)weft_read_u32_le(base + (i + 0u) * stride)\n"
            "                            | ((uint64_t)weft_read_u32_le(base + (i + 1u) * stride) << 32)),\n"
            "                vcreate_u32((uint64_t)weft_read_u32_le(base + (i + 2u) * stride)\n"
            "                            | ((uint64_t)weft_read_u32_le(base + (i + 3u) * stride) << 32)));\n"
            "            const uint32x4_t eq = vceqq_u32(got, want);\n"
            "            if (vgetq_lane_u32(eq, 0) != 0xFFFFFFFFu) {\n"
            "                if (out_count) { *out_count = i + 0u; }\n"
            "                if (out_bad_index) { *out_bad_index = i + 0u; }\n"
            "                return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "            }\n"
            "            if (vgetq_lane_u32(eq, 1) != 0xFFFFFFFFu) {\n"
            "                if (out_count) { *out_count = i + 1u; }\n"
            "                if (out_bad_index) { *out_bad_index = i + 1u; }\n"
            "                return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "            }\n"
            "            if (vgetq_lane_u32(eq, 2) != 0xFFFFFFFFu) {\n"
            "                if (out_count) { *out_count = i + 2u; }\n"
            "                if (out_bad_index) { *out_bad_index = i + 2u; }\n"
            "                return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "            }\n"
            "            if (vgetq_lane_u32(eq, 3) != 0xFFFFFFFFu) {\n"
            "                if (out_count) { *out_count = i + 3u; }\n"
            "                if (out_bad_index) { *out_bad_index = i + 3u; }\n"
            "                return WEFT_ERR_SCHEMA_MISMATCH;\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "#endif\n",
            up);
    }

    // scalar tail (always present; also the full path on plain C11 targets)
    wsb_printf(b,
        "    for (; i < n; i++) {\n"
        "        if (%s(base + i * stride) != WEFT_%s_SCHEMA_ID) {\n"
        "            if (out_count) { *out_count = i; }\n"
        "            if (out_bad_index) { *out_bad_index = i; }\n"
        "            return WEFT_ERR_SCHEMA_MISMATCH;\n"
        "        }\n"
        "    }\n"
        "    if (out_count) { *out_count = n; }\n"
        "    return WEFT_OK;\n"
        "}\n\n",
        (s->schema_width == 64) ? "weft_read_u64_le" : "weft_read_u32_le", up);
}

static void emit_struct_header(const weft_ir* ir, const weft_struct* s, const weft_emit_opts* o)
{
    strbuf b;
    wsb_init(&b);
    char up[160];
    weft_upper(s->name, up);
    char core_upper[160];
    weft_upper(o->core_name, core_upper);

    // ---- banner --------------------------------------------------------
    char sig_note[80] = "";
    if (s->root) {
        snprintf(sig_note, sizeof(sig_note), "%s (%s)", s->schema_id_str,
                 s->schema_auto ? "auto: FNV-1a-64 of the canonical layout signature" : "explicit");
    }
    wsb_printf(&b,
        "// %s.h — generated by weftc-codegen %s (target: c). DO NOT EDIT.\n"
        "// module: %s | struct: %s | root: %s\n",
        s->name, WEFTC_CODEGEN_VERSION, ir->module, s->name, s->root ? "yes" : "no (embedded layout type)");
    if (s->root) {
        wsb_printf(&b, "// schema_id: %s\n", sig_note);
    }
    wsb_printf(&b,
        "// layout: %u bytes, align %u%s\n"
        "//\n"
        "// Laws (Pillar 1 briefing):\n"
        "//   1. zero dynamic allocation on any read/write path below\n"
        "//   2. pure C11 — the baseline build includes ONLY <stdint.h> <stdbool.h>\n"
        "//      <stddef.h> <stdalign.h> <assert.h> (SIMD paths are behind\n"
        "//      compiler capability macros and appear only when targeted)\n"
        "//   3. unaligned-safe: cast helpers validate alignment; packed readers\n"
        "//      are byte assembly — no UB on any architecture\n"
        "//   4. host layout is guarded by static_assert below — a mismatched\n"
        "//      compile is a build failure, never a silent misread\n"
        "\n",
        s->size, s->align, s->repr_align ? " (explicit alignas)" : "");

    // ---- shared core ----------------------------------------------------
    emit_core_block(&b, core_upper);

    // ---- SIMD capability includes (root structs only) --------------------
    if (s->root) {
        wsb_puts(&b,
            "#if defined(__AVX2__)\n"
            "#include <immintrin.h>\n"
            "#elif defined(__ARM_NEON)\n"
            "#include <arm_neon.h>\n"
            "#endif\n\n");
    }

    // ---- header guard + nested includes ---------------------------------
    wsb_printf(&b, "#ifndef WEFT_%s_H\n#define WEFT_%s_H\n\n", up, up);
    for (uint32_t f = 0; f < s->nfields; f++) {
        const weft_field* fl = &s->fields[f];
        weft_type_kind dep = (fl->kind == WT_ARRAY) ? fl->elem_kind : fl->kind;
        if (dep == WT_STRUCT) {
            wsb_printf(&b, "#include \"%s.h\" // embedded %s\n", fl->struct_ref, fl->struct_ref);
        }
    }

    // ---- macros ----------------------------------------------------------
    if (s->root) {
        if (s->schema_width == 64) {
            wsb_printf(&b, "#define WEFT_%s_SCHEMA_ID UINT64_C(%s)\n", up, s->schema_id_str);
        } else {
            wsb_printf(&b, "#define WEFT_%s_SCHEMA_ID UINT32_C(%s)\n", up, s->schema_id_str);
        }
    }
    wsb_printf(&b, "#define WEFT_%s_SIZE %uu\n", up, s->size);
    wsb_printf(&b, "#define WEFT_%s_ALIGN %uu\n", up, s->align);
    for (uint32_t f = 0; f < s->nfields; f++) {
        const weft_field* fl = &s->fields[f];
        char fup[160];
        weft_upper(fl->name, fup);
        wsb_printf(&b, "#define WEFT_%s_OFF_%s %uu\n", up, fup, fl->offset);
        if (fl->kind == WT_ARRAY) {
            wsb_printf(&b, "#define WEFT_%s_LEN_%s %uu\n", up, fup, fl->count);
        } else if (fl->kind == WT_VEC2F32 || fl->kind == WT_VEC2F16) {
            wsb_printf(&b, "#define WEFT_%s_LEN_%s 2u\n", up, fup);
        } else if (fl->kind == WT_VEC3F32 || fl->kind == WT_VEC3F16) {
            wsb_printf(&b, "#define WEFT_%s_LEN_%s 3u\n", up, fup);
        } else if (fl->kind == WT_VEC4F32 || fl->kind == WT_VEC4F16) {
            wsb_printf(&b, "#define WEFT_%s_LEN_%s 4u\n", up, fup);
        }
    }
    wsb_puts(&b, "\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    // ---- struct decl with synthesized explicit padding -------------------
    wsb_printf(&b, "/// %s — %u bytes, align %u. Offsets are normative (weft-ir),\n"
                   "/// synthesized padding members make the C declaration match them exactly.\n",
               s->c_name, s->size, s->align);
    if (s->doc) {
        wsb_printf(&b, "/// %s\n", s->doc);
    }
    wsb_printf(&b, "typedef struct weft_%s {\n", s->name);
    uint32_t cursor = 0;
    int pad_idx = 0;
    bool first_member = true;
    char first_prefix[48] = "";
    if (s->repr_align) {
        // C11 portable explicit alignment: _Alignas on the FIRST member raises
        // the struct alignment without perturbing any offset.
        snprintf(first_prefix, sizeof(first_prefix), "alignas(%u) ", s->align);
    }
    for (uint32_t f = 0; f < s->nfields; f++) {
        const weft_field* fl = &s->fields[f];
        emit_pad_members(&b, cursor, fl->offset, &pad_idx, first_prefix, &first_member);
        char base[96], sfx[80];
        c_field_parts(fl, base, sfx);
        char docline[160] = "";
        if (fl->doc) {
            snprintf(docline, sizeof(docline), " // %s", fl->doc);
        }
        const char* pre = first_member ? first_prefix : "";
        wsb_printf(&b, "    %s%s %s%s;%s              // @%u (%u)\n", pre, base, fl->name, sfx, docline, fl->offset, fl->size);
        first_member = false;
        cursor = fl->offset + fl->size;
    }
    emit_pad_members(&b, cursor, s->size, &pad_idx, "", &first_member);
    wsb_printf(&b, "} weft_%s_t;\n\n", s->name);

    // ---- static asserts ---------------------------------------------------
    wsb_printf(&b,
        "// Law 4 (host side): compile-time ABI guards.\n"
        "static_assert(sizeof(weft_%s_t) == %u, \"weft ABI layout violation: %s size\");\n",
        s->name, s->size, s->name);
    if (s->repr_align) {
        wsb_printf(&b, "static_assert(alignof(weft_%s_t) == %u, \"weft ABI layout violation: %s alignment\");\n",
                   s->name, s->align, s->name);
    }
    for (uint32_t f = 0; f < s->nfields; f++) {
        const weft_field* fl = &s->fields[f];
        wsb_printf(&b,
            "static_assert(offsetof(weft_%s_t, %s) == %u, \"weft ABI: %s.%s offset mismatch\");\n",
            s->name, fl->name, fl->offset, s->name, fl->name);
    }
    wsb_puts(&b, "\n");

    // ---- zero-copy cast helpers (roots only) ------------------------------
    if (s->root) {
        const char* ctype = s->c_name;
        const char* deref = (s->schema_width == 64)
            ? "*(const uint64_t*)(const void*)p"
            : "*(const uint32_t*)(const void*)p";
        const char* cmp = (s->schema_width == 64)
            ? "!= (uint64_t)WEFT_x_SCHEMA_ID"
            : "!= (uint32_t)WEFT_x_SCHEMA_ID";
        // build comparison string with the real macro
        char cmpbuf[220];
        if (s->schema_width == 64) {
            snprintf(cmpbuf, sizeof(cmpbuf), "!= (uint64_t)WEFT_%s_SCHEMA_ID", up);
        } else {
            snprintf(cmpbuf, sizeof(cmpbuf), "!= (uint32_t)WEFT_%s_SCHEMA_ID", up);
        }
        (void)cmp;
        wsb_printf(&b,
            "/// Zero-copy projection: validates byte length, buffer alignment and the\n"
            "/// %u-bit schema hash header, then casts — no copy, no allocation. The hot\n"
            "/// path is three compares and one load.\n"
            "static inline const %s* weft_cast_%s(const void* buffer, size_t len, weft_error_t* err)\n"
            "{\n"
            "    weft_error_t rc = WEFT_OK;\n"
            "    const %s* out = NULL;\n"
            "    const uint8_t* p = (const uint8_t*)buffer;\n"
            "    if (len < (size_t)WEFT_%s_SIZE) {\n"
            "        rc = WEFT_ERR_SHORT_BUFFER;\n"
            "    } else if ((((uintptr_t)p) & (WEFT_%s_ALIGN - 1u)) != 0u) {\n"
            "        rc = WEFT_ERR_BAD_ALIGN;\n"
            "    } else if (%s %s) {\n"
            "        rc = WEFT_ERR_SCHEMA_MISMATCH;\n"
            "    } else {\n"
            "        out = (const %s*)p;\n"
            "    }\n"
            "    if (err) { *err = rc; }\n"
            "    return out;\n"
            "}\n\n",
            s->schema_width, ctype, s->name, ctype, up, up, deref, cmpbuf, ctype);
        wsb_printf(&b,
            "/// Mutable twin of weft_cast_%s (writer side: UMEM/DMA ring slots).\n"
            "static inline %s* weft_cast_%s_mut(void* buffer, size_t len, weft_error_t* err)\n"
            "{\n"
            "    return (%s*)(void*)(uintptr_t)weft_cast_%s((const void*)buffer, len, err);\n"
            "}\n\n",
            s->name, ctype, s->name, ctype, s->name);
    }

    // ---- packed readers / writers (all structs) ---------------------------
    {
        const char* ctype = s->c_name;
        char stem[96];
        c_stem(s, stem);
        wsb_printf(&b,
            "/// Alignment-free by-value read from a possibly-packed source (Law 3).\n"
            "/// Byte-assembly reads: safe on every architecture, no allocation.\n"
            "static inline weft_error_t %s_read_packed(const void* buffer, size_t len, %s* out)\n"
            "{\n"
            "    if (len < (size_t)WEFT_%s_SIZE) { return WEFT_ERR_SHORT_BUFFER; }\n"
            "    const uint8_t* p = (const uint8_t*)buffer;\n",
            stem, ctype, up);
        for (uint32_t f = 0; f < s->nfields; f++) {
            const weft_field* fl = &s->fields[f];
            char dst[96];
            snprintf(dst, sizeof(dst), "out->%s", fl->name);
            emit_field_read(&b, fl, dst, "p", fl->offset);
        }
        // synthesized pads round-trip too: byte-identical serialization is a
        // contract, and "don't-care" bytes still need a defined value
        emit_pad_reads(&b, s);
        wsb_puts(&b, "    return WEFT_OK;\n}\n\n");

        wsb_printf(&b,
            "/// Alignment-free write to a possibly-packed destination (UMEM/DMA paths).\n"
            "static inline weft_error_t %s_write_packed(void* buffer, size_t len, const %s* src)\n"
            "{\n"
            "    if (len < (size_t)WEFT_%s_SIZE) { return WEFT_ERR_SHORT_BUFFER; }\n"
            "    uint8_t* p = (uint8_t*)buffer;\n",
            stem, ctype, up);
        for (uint32_t f = 0; f < s->nfields; f++) {
            const weft_field* fl = &s->fields[f];
            char src[96];
            snprintf(src, sizeof(src), "src->%s", fl->name);
            emit_field_write(&b, fl, src, "p", fl->offset);
        }
        emit_pad_writes(&b, s);
        wsb_puts(&b, "    return WEFT_OK;\n}\n\n");
    }

    // ---- bitfield accessors ------------------------------------------------
    for (uint32_t f = 0; f < s->nfields; f++) {
        const weft_field* fl = &s->fields[f];
        for (uint32_t bt = 0; bt < fl->nbits; bt++) {
            const weft_bits* bf = &fl->bits[bt];
            uint32_t width = bf->hi - bf->lo + 1;
            char mask[40];
            if (fl->kind == WT_U64 || fl->kind == WT_I64) {
                if (width >= 64) {
                    strcpy(mask, "0xFFFFFFFFFFFFFFFFull");
                } else {
                    snprintf(mask, sizeof(mask), "0x%llXull", (unsigned long long)((1ull << width) - 1));
                }
            } else {
                if (width >= 32) {
                    strcpy(mask, "0xFFFFFFFFu");
                } else {
                    snprintf(mask, sizeof(mask), "0x%Xu", (unsigned)((1u << width) - 1));
                }
            }
            const char* castty = (fl->kind == WT_U64 || fl->kind == WT_I64) ? "uint64_t" : "uint32_t";
            wsb_printf(&b,
                "/// %s.%s: bits [%u..%u] of %s\n"
                "static inline %s weft_%s_get_%s(const weft_%s_t* f)\n"
                "{\n"
                "    return (%s)((f->%s >> %u) & %s);\n"
                "}\n"
                "static inline void weft_%s_set_%s(weft_%s_t* f, %s v)\n"
                "{\n"
                "    f->%s = (%s)((f->%s & ~(%s << %u)) | ((v & %s) << %u));\n"
                "}\n\n",
                s->name, bf->name, bf->lo, bf->hi, fl->name,
                castty, s->name, bf->name, s->name,
                castty, fl->name, bf->lo, mask,
                s->name, bf->name, s->name, castty,
                fl->name, fl->kind == WT_U64 || fl->kind == WT_I64 ? "uint64_t" : "uint32_t",
                fl->name, mask, bf->lo, mask, bf->lo);
        }
    }

    // ---- batch validation (roots only) -------------------------------------
    if (s->root) {
        emit_batch_validator(&b, s);
    }

    wsb_puts(&b, "#ifdef __cplusplus\n}\n#endif\n\n");
    wsb_printf(&b, "#endif // WEFT_%s_H\n", up);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.h", o->out_dir, s->name);
    weft_write_file(path, &b);
    wsb_free(&b);
}

int weft_emit_c(const weft_ir* ir, const weft_emit_opts* o)
{
    // defensive: core name must not collide with any struct's guard
    char core_upper[160];
    weft_upper(o->core_name, core_upper);
    for (uint32_t k = 0; k < ir->nstructs; k++) {
        char up[160];
        weft_upper(ir->ss[k].name, up);
        if (strcmp(up, core_upper) == 0) {
            weft_warn("core name '%s' collides with struct '%s' header guard — pick another --core-name",
                      o->core_name, ir->ss[k].name);
            return -1;
        }
    }

    // standalone core header
    {
        strbuf core;
        wsb_init(&core);
        wsb_printf(&core,
            "// %s.h — generated by weftc-codegen %s. DO NOT EDIT.\n"
            "// Shared projection core for all weftc-generated C headers.\n"
            "// Every generated header embeds this block itself (self-guarded),\n"
            "// so including this file directly is optional — it exists for\n"
            "// tooling and for humans who want the core alone.\n\n",
            o->core_name, WEFTC_CODEGEN_VERSION);
        emit_core_block(&core, core_upper);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.h", o->out_dir, o->core_name);
        weft_write_file(path, &core);
        wsb_free(&core);
    }

    for (uint32_t k = 0; k < ir->nstructs; k++) {
        emit_struct_header(ir, &ir->ss[k], o);
    }
    return 0;
}
