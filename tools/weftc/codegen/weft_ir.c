// weft_ir.c — weft-ir JSON v1 loader + layout verification (header §5).
//
// Everything here is REFUSAL by design: an IR that violates a layout law is
// rejected before any backend runs, with a message precise enough to fix the
// upstream layout engine. The five fixture schemas in tests/fixtures/ are the
// reference corpus for what "verified" means.

#include "weftc_codegen.h"

#include <errno.h>

#define IR_MAX_STRUCTS 256
#define IR_MAX_FIELDS 256
#define IR_MAX_BITS 32
#define IR_MAX_FILE (8u * 1024 * 1024)

typedef struct {
    weft_arena* a;
    const char* path;
    char err[512];
    bool failed;
    // struct name table for type resolution (pass 1)
    const char* names[IR_MAX_STRUCTS];
    uint32_t nnames;
} ird;

static void ird_err(ird* d, const char* fmt, ...)
{
    if (d->failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->err, sizeof(d->err), fmt, ap);
    va_end(ap);
    d->failed = true;
}

static char* ird_read_file(ird* d, size_t* out_len)
{
    FILE* f = fopen(d->path, "rb");
    if (!f) {
        ird_err(d, "cannot open '%s'", d->path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { ird_err(d, "seek failed on '%s'", d->path); fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > IR_MAX_FILE) {
        ird_err(d, "'%s' too large (max %u bytes)", d->path, IR_MAX_FILE);
        fclose(f);
        return NULL;
    }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { ird_err(d, "out of memory reading '%s'", d->path); fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        ird_err(d, "short read on '%s'", d->path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *out_len = (size_t)sz;
    return buf;
}

// scalar + vector token table (also used to reject struct names that shadow
// type tokens)
static const struct { const char* tok; weft_type_kind k; } k_type_tokens[] = {
    {"u8", WT_U8}, {"i8", WT_I8}, {"u16", WT_U16}, {"i16", WT_I16},
    {"u32", WT_U32}, {"i32", WT_I32}, {"u64", WT_U64}, {"i64", WT_I64},
    {"f16", WT_F16}, {"f32", WT_F32}, {"f64", WT_F64}, {"bool", WT_BOOL},
    {"vec2<f32>", WT_VEC2F32}, {"vec3<f32>", WT_VEC3F32}, {"vec4<f32>", WT_VEC4F32},
    {"vec2<f16>", WT_VEC2F16}, {"vec3<f16>", WT_VEC3F16}, {"vec4<f16>", WT_VEC4F16},
    {NULL, WT_U8}
};

static bool parse_base_type(const char* tok, weft_type_kind* out)
{
    for (int k = 0; k_type_tokens[k].tok; k++) {
        if (strcmp(k_type_tokens[k].tok, tok) == 0) {
            *out = k_type_tokens[k].k;
            return true;
        }
    }
    return false;
}

static bool is_type_token(const char* tok)
{
    weft_type_kind unused;
    return parse_base_type(tok, &unused);
}

static uint32_t round_up(uint32_t v, uint32_t a)
{
    return ((v + a - 1) / a) * a;
}

static bool is_pow2(uint32_t v)
{
    return v && (v & (v - 1)) == 0;
}

static uint32_t type_bits(weft_type_kind k)
{
    switch (k) {
    case WT_U8: case WT_I8: case WT_BOOL: return 8;
    case WT_U16: case WT_I16: case WT_F16: return 16;
    case WT_U32: case WT_I32: case WT_F32: return 32;
    case WT_U64: case WT_I64: case WT_F64: return 64;
    default: return 0;
    }
}

// --- type spec: "u16" | "vec3<f32>" | "[u16; 4]" | struct-name ---------------
static bool parse_type_spec(ird* d, const char* spec, weft_field* f, const char* struct_name, const char* field_name)
{
    char ctx[256];
    snprintf(ctx, sizeof(ctx), "struct %s field %s: type '%s'", struct_name, field_name, spec);

    size_t n = strlen(spec);
    if (n >= 2 && spec[0] == '[') {
        // [T; N]
        const char* semi = strchr(spec, ';');
        const char* close = (n > 0) ? spec + n - 1 : NULL;
        if (!semi || !close || *close != ']' || semi > close) {
            ird_err(d, "%s: malformed array type (expected \"[elem; count]\")", ctx);
            return false;
        }
        size_t elem_len = (size_t)(semi - spec - 1);
        char elem[64];
        if (elem_len == 0 || elem_len >= sizeof(elem)) {
            ird_err(d, "%s: array element type missing or too long", ctx);
            return false;
        }
        memcpy(elem, spec + 1, elem_len);
        elem[elem_len] = '\0';
        char count_buf[24];
        size_t count_len = (size_t)(close - semi - 1);
        if (count_len == 0 || count_len >= sizeof(count_buf)) {
            ird_err(d, "%s: array count missing or too long", ctx);
            return false;
        }
        memcpy(count_buf, semi + 1, count_len);
        count_buf[count_len] = '\0';
        char* end = NULL;
        errno = 0;
        unsigned long long cnt = strtoull(count_buf, &end, 10);
        if (errno != 0 || !end || *end != '\0' || cnt == 0 || cnt > 65536) {
            ird_err(d, "%s: array count must be a decimal in [1, 65536]", ctx);
            return false;
        }
        f->kind = WT_ARRAY;
        f->count = (uint32_t)cnt;
        // element: scalar, vector, or struct name (never another array)
        if (parse_base_type(elem, &f->elem_kind)) {
            return true;
        }
        // struct element
        for (uint32_t k = 0; k < d->nnames; k++) {
            if (strcmp(d->names[k], elem) == 0) {
                f->elem_kind = WT_STRUCT;
                f->struct_ref = weft_arena_strdup(d->a, elem);
                return true;
            }
        }
        ird_err(d, "%s: unknown element type '%s'", ctx, elem);
        return false;
    }
    if (parse_base_type(spec, &f->kind)) {
        return true;
    }
    for (uint32_t k = 0; k < d->nnames; k++) {
        if (strcmp(d->names[k], spec) == 0) {
            f->kind = WT_STRUCT;
            f->struct_ref = weft_arena_strdup(d->a, spec);
            return true;
        }
    }
    ird_err(d, "%s: unknown type (see README §IR for the v1 type set)", ctx);
    return false;
}

// --- one field ---------------------------------------------------------------
static bool load_field(ird* d, jv* jf, weft_struct* s, weft_field* f, uint32_t fidx)
{
    const char* name = jv_get_str(jf, "name", NULL);
    if (!name || !name[0]) {
        ird_err(d, "struct %s field #%u: 'name' (string) is required", s->name, fidx);
        return false;
    }
    if (!weft_ident_ok(name)) {
        ird_err(d, "struct %s: field name '%s' is invalid or reserved in C/Rust/WGSL/GLSL", s->name, name);
        return false;
    }
    for (uint32_t k = 0; k < fidx; k++) {
        if (strcmp(s->fields[k].name, name) == 0) {
            ird_err(d, "struct %s: duplicate field name '%s'", s->name, name);
            return false;
        }
    }
    f->name = weft_arena_strdup(d->a, name);

    const char* type_spec = jv_get_str(jf, "type", NULL);
    if (!type_spec) {
        ird_err(d, "struct %s field %s: 'type' (string) is required", s->name, name);
        return false;
    }
    if (!parse_type_spec(d, type_spec, f, s->name, name)) return false;

    uint32_t offset;
    if (!jv_get_u32(jf, "offset", 0, 0xFFFFFFFEu, &offset)) {
        ird_err(d, "struct %s field %s: 'offset' must be an integer in [0, 2^31)", s->name, name);
        return false;
    }
    f->offset = offset;

    f->doc = jv_get_str(jf, "doc", NULL);
    if (f->doc) f->doc = weft_arena_strdup(d->a, f->doc);

    f->gpu_type = jv_get_str(jf, "gpu_type", NULL);
    if (f->gpu_type) {
        f->gpu_type = weft_arena_strdup(d->a, f->gpu_type);
        // v1 whitelist: mat4x4<f32> over [f32; 16]
        if (strcmp(f->gpu_type, "mat4x4<f32>") != 0) {
            ird_err(d, "struct %s field %s: gpu_type '%s' is not in the v1 whitelist {mat4x4<f32>}",
                    s->name, name, f->gpu_type);
            return false;
        }
        if (!(f->kind == WT_ARRAY && f->elem_kind == WT_F32 && f->count == 16)) {
            ird_err(d, "struct %s field %s: gpu_type mat4x4<f32> requires host type [f32; 16]",
                    s->name, name);
            return false;
        }
    }

    // bitfields
    jv* jbits = jv_get(jf, "bits");
    if (jbits) {
        if (jbits->kind != JV_ARR || jbits->nitems == 0 || jbits->nitems > IR_MAX_BITS) {
            ird_err(d, "struct %s field %s: 'bits' must be a non-empty array (max %d)",
                    s->name, name, IR_MAX_BITS);
            return false;
        }
        if (f->kind == WT_ARRAY || f->kind == WT_STRUCT || type_bits(f->kind) == 0) {
            ird_err(d, "struct %s field %s: 'bits' requires an integer scalar field", s->name, name);
            return false;
        }
        f->nbits = jbits->nitems;
        f->bits = (weft_bits*)weft_arena_alloc(d->a, sizeof(weft_bits) * f->nbits, 8);
        for (uint32_t b = 0; b < f->nbits; b++) {
            jv* jb = jbits->items[b];
            if (!jb || jb->kind != JV_OBJ) {
                ird_err(d, "struct %s field %s: bits[%u] must be an object", s->name, name, b);
                return false;
            }
            const char* bname = jv_get_str(jb, "name", NULL);
            if (!bname || !weft_ident_ok(bname)) {
                ird_err(d, "struct %s field %s: bits[%u].name '%s' is invalid or reserved",
                        s->name, name, b, bname ? bname : "(missing)");
                return false;
            }
            // bitfield names share the struct-wide accessor namespace
            // (skip the field being built: its bits[] entries are not
            // initialized yet — that is the duplicate check further below)
            for (uint32_t p = 0; p < s->nfields; p++) {
                if (&s->fields[p] == f) continue;
                for (uint32_t q = 0; q < s->fields[p].nbits; q++) {
                    if (strcmp(s->fields[p].bits[q].name, bname) == 0) {
                        ird_err(d, "struct %s: duplicate bitfield name '%s' (accessors collide)",
                                s->name, bname);
                        return false;
                    }
                }
            }
            for (uint32_t q = 0; q < b; q++) {
                if (strcmp(f->bits[q].name, bname) == 0) {
                    ird_err(d, "struct %s field %s: duplicate bitfield name '%s'", s->name, name, bname);
                    return false;
                }
            }
            uint32_t lo, hi;
            uint32_t width = type_bits(f->kind);
            if (!jv_get_u32(jb, "lo", 0, width - 1, &lo) || !jv_get_u32(jb, "hi", 0, width - 1, &hi) || lo > hi) {
                ird_err(d, "struct %s field %s: bits[%u] lo/hi must satisfy 0 <= lo <= hi < %u",
                        s->name, name, b, width);
                return false;
            }
            f->bits[b].name = weft_arena_strdup(d->a, bname);
            f->bits[b].lo = lo;
            f->bits[b].hi = hi;
        }
    }
    return true;
}

// --- per-struct verification after refs resolved ------------------------------
static bool verify_struct(ird* d, weft_struct* s)
{
    // compute field layouts
    uint32_t max_end = 0;
    uint32_t natural = 1;
    for (uint32_t k = 0; k < s->nfields; k++) {
        weft_field* f = &s->fields[k];
        weft_layout l = weft_type_layout(f);
        f->size = l.size;
        f->align = l.align;
        if (f->offset % f->align != 0) {
            ird_err(d, "struct %s field %s: offset %u violates C alignment %u",
                    s->name, f->name, f->offset, f->align);
            return false;
        }
        if (k > 0 && f->offset < s->fields[k - 1].offset + s->fields[k - 1].size) {
            ird_err(d, "struct %s field %s: offset %u overlaps previous field (ends at %u)",
                    s->name, f->name, f->offset,
                    s->fields[k - 1].offset + s->fields[k - 1].size);
            return false;
        }
        if (f->offset + f->size > s->size) {
            ird_err(d, "struct %s field %s: end %u exceeds struct size %u",
                    s->name, f->name, f->offset + f->size, s->size);
            return false;
        }
        if (f->offset + f->size > max_end) max_end = f->offset + f->size;
        if (f->align > natural) natural = f->align;
    }
    // struct alignment laws
    if (!is_pow2(s->align)) {
        ird_err(d, "struct %s: align %u must be a power of two", s->name, s->align);
        return false;
    }
    if (s->align < natural) {
        ird_err(d, "struct %s: declared align %u < natural member alignment %u",
                s->name, s->align, natural);
        return false;
    }
    s->repr_align = (s->align > natural);
    uint32_t want_size = round_up(max_end, s->align);
    if (s->size != want_size) {
        ird_err(d, "struct %s: declared size %u != roundUp(align %u, end %u) = %u "
                "(tail padding must be explicit and exact)",
                s->name, s->size, s->align, max_end, want_size);
        return false;
    }
    // schema header law for roots
    if (s->root) {
        const weft_field* f0 = &s->fields[0];
        bool width_ok = (s->schema_width == 64 && f0->kind == WT_U64) ||
                        (s->schema_width == 32 && f0->kind == WT_U32);
        if (strcmp(f0->name, "schema_id") != 0 || f0->offset != 0 || !width_ok) {
            ird_err(d, "struct %s: root structs must lead with field 'schema_id' of type %s at offset 0 "
                    "(found '%s' at %u)",
                    s->name, s->schema_width == 32 ? "u32" : "u64", f0->name, f0->offset);
            return false;
        }
    }
    return true;
}

// --- topological sort (dependencies first, stable) ----------------------------
static bool topo_visit(ird* d, uint32_t idx, weft_struct* all, uint32_t nall,
                       uint8_t* state, weft_struct** out, uint32_t* nout)
{
    if (state[idx] == 2) return true;
    if (state[idx] == 1) {
        ird_err(d, "struct cycle detected at '%s'", all[idx].name);
        return false;
    }
    state[idx] = 1;
    for (uint32_t f = 0; f < all[idx].nfields; f++) {
        weft_field* fl = &all[idx].fields[f];
        weft_type_kind dep = fl->kind == WT_ARRAY ? fl->elem_kind : fl->kind;
        if (dep == WT_STRUCT) {
            for (uint32_t j = 0; j < nall; j++) {
                if (strcmp(all[j].name, fl->struct_ref) == 0) {
                    if (!topo_visit(d, j, all, nall, state, out, nout)) return false;
                    break;
                }
            }
        }
    }
    state[idx] = 2;
    out[(*nout)++] = &all[idx];
    return true;
}

// --- entry --------------------------------------------------------------------
weft_ir* weft_ir_load(const char* path, char err[512])
{
    static _Thread_local char pbuf[256];
    snprintf(pbuf, sizeof(pbuf), "%.250s", path);
    ird d;
    memset(&d, 0, sizeof(d));
    d.path = pbuf;
    d.a = (weft_arena*)calloc(1, sizeof(weft_arena));

    size_t len = 0;
    char* text = ird_read_file(&d, &len);
    if (!text) {
        snprintf(err, 512, "%s: %.300s", path, d.err);
        return NULL;
    }

    char jerr[256];
    unsigned jline = 0, jcol = 0;
    jv* root = jv_parse(d.a, text, len, jerr, &jline, &jcol);
    free(text);
    if (!root) {
        snprintf(err, 512, "%s:%u:%u: %s", path, jline, jcol, jerr);
        return NULL;
    }
    if (root->kind != JV_OBJ) {
        snprintf(err, 512, "%s: top level must be an object", path);
        return NULL;
    }
    int64_t version;
    {
        jv* v = jv_get(root, "weft_ir");
        if (!v || v->kind != JV_INT || v->i != 1) {
            snprintf(err, 512, "%s: 'weft_ir' must be 1 (this is weft-ir v1)", path);
            return NULL;
        }
        version = v->i;
        (void)version;
    }
    const char* module = jv_get_str(root, "module", NULL);
    if (!module || !weft_snake_ok(module)) {
        snprintf(err, 512, "%s: 'module' must be a lower_snake identifier (reserved words rejected)", path);
        return NULL;
    }
    jv* jtypes = jv_get(root, "types");
    if (!jtypes || jtypes->kind != JV_ARR || jtypes->nitems == 0 || jtypes->nitems > IR_MAX_STRUCTS) {
        snprintf(err, 512, "%s: 'types' must be a non-empty array (max %d structs)", path, IR_MAX_STRUCTS);
        return NULL;
    }

    // pass 1: collect struct names
    uint32_t nall = jtypes->nitems;
    for (uint32_t k = 0; k < nall; k++) {
        jv* js = jtypes->items[k];
        const char* nm = js ? jv_get_str(js, "name", NULL) : NULL;
        if (!nm) {
            snprintf(err, 512, "%s: types[%u]: 'name' is required", path, k);
            return NULL;
        }
        if (!weft_snake_ok(nm) || is_type_token(nm)) {
            snprintf(err, 512, "%s: struct name '%s' must be lower_snake, unreserved, and not a type token",
                     path, nm);
            return NULL;
        }
        for (uint32_t j = 0; j < d.nnames; j++) {
            if (strcmp(d.names[j], nm) == 0) {
                snprintf(err, 512, "%s: duplicate struct name '%s'", path, nm);
                return NULL;
            }
        }
        d.names[d.nnames++] = weft_arena_strdup(d.a, nm);
    }

    // pass 2: parse structs + fields
    weft_struct* all = (weft_struct*)weft_arena_alloc(d.a, sizeof(weft_struct) * nall, 8);
    memset(all, 0, sizeof(weft_struct) * nall);
    for (uint32_t k = 0; k < nall; k++) {
        jv* js = jtypes->items[k];
        if (js->kind != JV_OBJ) {
            snprintf(err, 512, "%s: types[%u] must be an object", path, k);
            return NULL;
        }
        weft_struct* s = &all[k];
        const char* kind = jv_get_str(js, "kind", "struct");
        if (strcmp(kind, "struct") != 0) {
            snprintf(err, 512, "%s: struct %s: kind '%s' unsupported in weft-ir v1 (only \"struct\")",
                     path, jv_get_str(js, "name", "?"), kind);
            return NULL;
        }
        s->name = weft_arena_strdup(d.a, jv_get_str(js, "name", NULL));
        s->doc = jv_get_str(js, "doc", NULL);
        if (s->doc) s->doc = weft_arena_strdup(d.a, s->doc);

        if (!jv_get_u32(js, "size", 1, 1u << 20, &s->size)) {
            snprintf(err, 512, "%s: struct %s: 'size' must be an integer in [1, 2^20]", path, s->name);
            return NULL;
        }
        if (!jv_get_u32(js, "align", 1, 4096, &s->align)) {
            snprintf(err, 512, "%s: struct %s: 'align' must be an integer in [1, 4096]", path, s->name);
            return NULL;
        }
        if (!jv_get_bool(js, "root", true, &s->root)) {
            snprintf(err, 512, "%s: struct %s: 'root' must be a boolean", path, s->name);
            return NULL;
        }
        uint32_t width = 64;
        if (!jv_get_u32(js, "schema_id_width", 32, 64, &width)) {
            jv* w = jv_get(js, "schema_id_width");
            if (w) {
                snprintf(err, 512, "%s: struct %s: 'schema_id_width' must be 32 or 64", path, s->name);
                return NULL;
            }
        }
        s->schema_width = width;

        const char* c_name = jv_get_str(js, "c_name", NULL);
        s->c_name = c_name ? weft_arena_strdup(d.a, c_name) : NULL;
        const char* rust_name = jv_get_str(js, "rust_name", NULL);
        s->rust_name = rust_name ? weft_arena_strdup(d.a, rust_name) : NULL;

        // schema id: "auto" (default) or explicit 0x hex
        const char* sid = jv_get_str(js, "schema_id", "auto");
        if (strcmp(sid, "auto") == 0) {
            s->schema_auto = true;
        } else {
            if (!s->root) {
                snprintf(err, 512, "%s: struct %s: explicit schema_id is only valid on root structs",
                         path, s->name);
                return NULL;
            }
            if (sid[0] != '0' || (sid[1] != 'x' && sid[1] != 'X')) {
                snprintf(err, 512, "%s: struct %s: schema_id must be \"auto\" or \"0x...\" hex",
                         path, s->name);
                return NULL;
            }
            errno = 0;
            char* end = NULL;
            unsigned long long v = strtoull(sid + 2, &end, 16);
            if (errno != 0 || !end || *end != '\0') {
                snprintf(err, 512, "%s: struct %s: malformed schema_id hex '%s'", path, s->name, sid);
                return NULL;
            }
            if (s->schema_width == 32 && v > 0xFFFFFFFFULL) {
                snprintf(err, 512, "%s: struct %s: schema_id exceeds 32 bits", path, s->name);
                return NULL;
            }
            if (v == 0) {
                snprintf(err, 512, "%s: struct %s: schema_id must not be zero", path, s->name);
                return NULL;
            }
            s->schema_id = (uint64_t)v;
        }

        jv* jfields = jv_get(js, "fields");
        if (!jfields || jfields->kind != JV_ARR || jfields->nitems == 0 || jfields->nitems > IR_MAX_FIELDS) {
            snprintf(err, 512, "%s: struct %s: 'fields' must be a non-empty array (max %d)",
                     path, s->name, IR_MAX_FIELDS);
            return NULL;
        }
        s->nfields = jfields->nitems;
        s->fields = (weft_field*)weft_arena_alloc(d.a, sizeof(weft_field) * s->nfields, 8);
        memset(s->fields, 0, sizeof(weft_field) * s->nfields);
        for (uint32_t fi = 0; fi < s->nfields; fi++) {
            jv* jf = jfields->items[fi];
            if (!jf || jf->kind != JV_OBJ) {
                snprintf(err, 512, "%s: struct %s: fields[%u] must be an object", path, s->name, fi);
                return NULL;
            }
            if (!load_field(&d, jf, s, &s->fields[fi], fi)) {
                snprintf(err, 512, "%s: struct %s: %.340s", path, s->name, d.err);
                return NULL;
            }
        }
    }

    // resolve struct refs
    for (uint32_t k = 0; k < nall; k++) {
        for (uint32_t f = 0; f < all[k].nfields; f++) {
            weft_field* fl = &all[k].fields[f];
            weft_type_kind dep = fl->kind == WT_ARRAY ? fl->elem_kind : fl->kind;
            if (dep == WT_STRUCT) {
                bool found = false;
                for (uint32_t j = 0; j < nall; j++) {
                    if (strcmp(all[j].name, fl->struct_ref) == 0) {
                        fl->struct_type = &all[j];
                        found = true;
                        break;
                    }
                }
                if (!found) { // unreachable (names pre-validated) but stays defensive
                    snprintf(err, 512, "%s: struct %s field %s: unresolvable type '%s'",
                             path, all[k].name, fl->name, fl->struct_ref);
                    return NULL;
                }
            }
        }
    }

    // verify each struct (after refs resolve so nested layouts compute)
    for (uint32_t k = 0; k < nall; k++) {
        if (!verify_struct(&d, &all[k])) {
            snprintf(err, 512, "%s: %.300s", path, d.err);
            return NULL;
        }
    }

    // names
    for (uint32_t k = 0; k < nall; k++) {
        char nerr[256];
        if (!weft_names_derive(d.a, &all[k], nerr)) {
            snprintf(err, 512, "%s: %s", path, nerr);
            return NULL;
        }
    }

    // schema ids (auto -> FNV-1a-64 of the canonical signature)
    for (uint32_t k = 0; k < nall; k++) {
        weft_struct* s = &all[k];
        if (!s->root) continue;
        if (s->schema_auto) {
            char sig[512];
            weft_schema_signature(s, sig);
            s->schema_id = weft_fnv1a64(sig);
        }
        if (s->schema_width == 64) {
            weft_hex64(s->schema_id, s->schema_id_str);
        } else {
            weft_hex32((uint32_t)s->schema_id, s->schema_id_str);
        }
    }

    // topo sort into the final order
    weft_struct** ordered = (weft_struct**)weft_arena_alloc(d.a, sizeof(weft_struct*) * nall, 8);
    uint8_t* state = (uint8_t*)weft_arena_alloc(d.a, nall, 8);
    memset(state, 0, nall);
    uint32_t nout = 0;
    for (uint32_t k = 0; k < nall; k++) {
        if (!topo_visit(&d, k, all, nall, state, ordered, &nout)) {
            snprintf(err, 512, "%s: %.300s", path, d.err);
            return NULL;
        }
    }
    weft_struct* sorted = (weft_struct*)weft_arena_alloc(d.a, sizeof(weft_struct) * nall, 8);
    for (uint32_t k = 0; k < nall; k++) {
        sorted[k] = *ordered[k];
        // fix self-referencing struct_type pointers after the copy
        for (uint32_t f = 0; f < sorted[k].nfields; f++) {
            weft_field* fl = &sorted[k].fields[f];
            weft_type_kind dep = fl->kind == WT_ARRAY ? fl->elem_kind : fl->kind;
            if (dep == WT_STRUCT) {
                for (uint32_t j = 0; j < nall; j++) {
                    if (strcmp(sorted[j].name, fl->struct_ref) == 0) {
                        fl->struct_type = &sorted[j];
                        break;
                    }
                }
            }
        }
    }

    weft_ir* ir = (weft_ir*)calloc(1, sizeof(weft_ir));
    ir->module = weft_arena_strdup(d.a, module);
    ir->ss = sorted;
    ir->nstructs = nall;
    ir->arena = d.a;
    return ir;
}
