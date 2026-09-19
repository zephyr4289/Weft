/* dump.c — the four artifact emitters (RFC-0017 §6):
 *
 *   weft_dump_json          --dump-ir: the human-reviewable IR
 *   weft_dump_binary        WIR1 binary IR for Engineer 2/3 tooling
 *   weft_emit_verify_header C11 _Static_assert layout pinning (zero-cost
 *                           ABI drift detection in the consumer's build)
 *   weft_inspect            terminal ASCII memory map: offsets, padding
 *                           holes, cacheline boundaries
 *
 * Determinism (L3): every emitter is a pure function of the WeftUnit.
 * Fixed key order, fixed hex (lowercase, 0x, 16 digits), fixed decimal
 * formatting, '\n' newlines, no locale dependence, no timestamps.
 */
#include "weftc.h"

#include <stdlib.h>
#include <string.h>

static int json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20) fprintf(out, "\\u%04x", *p);
            else fputc(*p, out);
        }
    }
    fputc('"', out);
    return ferror(out) ? -1 : 0;
}

static void hx64(FILE *out, uint64_t v)
{
    fprintf(out, "0x%016llx", (unsigned long long)v);
}

/* JSON string form of a hash: "0x…" (quoted — a bare 0x… is not JSON). */
static void jhx64(FILE *out, uint64_t v)
{
    fputc('"', out);
    hx64(out, v);
    fputc('"', out);
}

/* ------------------------------------------------------------------ */
/* JSON IR                                                              */
/* ------------------------------------------------------------------ */
#define JJ(level) fputc('\n', out); for (int i_ = 0; i_ < (level); i_++) fputs("  ", out)

static int json_decl(const WeftUnit *u, size_t i, FILE *out, int level)
{
    const DeclLayout *dl = &u->layouts[i];
    JJ(level); fputs("\"kind\": ", out);
    fputs(dl->kind == DECL_STRUCT ? "\"struct\""
        : dl->kind == DECL_ENUM ? "\"enum\"" : "\"bitflags\"", out);
    fputs(",", out);
    JJ(level); fputs("\"name\": ", out); json_str(out, dl->name); fputs(",", out);
    JJ(level); fprintf(out, "\"size\": %llu,", (unsigned long long)dl->size);
    JJ(level); fprintf(out, "\"align\": %llu,", (unsigned long long)dl->align);
    JJ(level); fputs("\"abi_hash\": ", out); jhx64(out, dl->abi_hash); fputs(",", out);
    if (dl->kind == DECL_STRUCT) {
        JJ(level); fputs("\"packed\": ", out);
        fputs(dl->packed ? "true" : "false", out); fputs(",", out);
        JJ(level); fputs("\"reordered\": ", out);
        fputs(dl->reordered ? "true" : "false", out); fputs(",", out);
        JJ(level); fprintf(out, "\"align_attr\": %llu,",
                           (unsigned long long)dl->align_attr);
        JJ(level); fprintf(out, "\"simd_attr\": %llu,",
                           (unsigned long long)dl->simd_attr);
        JJ(level); fprintf(out, "\"internal_pad\": %llu,",
                           (unsigned long long)dl->internal_pad);
        JJ(level); fprintf(out, "\"trailing_pad\": %llu,",
                           (unsigned long long)dl->trailing_pad);
        JJ(level); fprintf(out, "\"optimize_hint\": %llu,",
                           (unsigned long long)dl->hint_opt_size);
        JJ(level); fputs("\"fields\": [", out);
        for (size_t f = 0; f < dl->nfields; f++) {
            const FieldLayout *fl = &dl->fields[f];
            JJ(level + 1); fputc('{', out);
            JJ(level + 2); fputs("\"name\": ", out);
            json_str(out, fl->name); fputs(",", out);
            JJ(level + 2); fputs("\"type\": ", out);
            json_str(out, fl->type_str); fputs(",", out);
            JJ(level + 2); fprintf(out, "\"offset\": %llu,",
                                   (unsigned long long)fl->offset);
            JJ(level + 2); fprintf(out, "\"size\": %llu,",
                                   (unsigned long long)fl->size);
            JJ(level + 2); fprintf(out, "\"align\": %llu,",
                                   (unsigned long long)fl->align);
            JJ(level + 2); fprintf(out, "\"index\": %u",
                                   (unsigned)fl->orig_index);
            JJ(level + 1); fputc('}', out);
            if (f + 1 < dl->nfields) fputc(',', out);
        }
        if (dl->nfields == 0) {} /* "[]" stays closed on the same line */
        JJ(level); fputs("],", out);
        JJ(level); fputs("\"holes\": [", out);
        for (size_t h = 0; h < dl->nholes; h++) {
            JJ(level + 1); fputc('{', out);
            JJ(level + 2); fprintf(out, "\"offset\": %llu,",
                                   (unsigned long long)dl->holes[h].offset);
            JJ(level + 2); fprintf(out, "\"size\": %llu",
                                   (unsigned long long)dl->holes[h].size);
            JJ(level + 1); fputc('}', out);
            if (h + 1 < dl->nholes) fputc(',', out);
        }
        JJ(level); fputc(']', out);
    } else {
        JJ(level); fputs("\"backing\": ", out);
        json_str(out, prim_info(dl->backing)->name); fputs(",", out);
        JJ(level); fprintf(out, "\"align_attr\": %llu,",
                           (unsigned long long)dl->align_attr);
        JJ(level); fputs("\"variants\": [", out);
        for (size_t v = 0; v < dl->nvariants; v++) {
            const Variant *vr = &dl->variants[v];
            JJ(level + 1); fputc('{', out);
            JJ(level + 2); fputs("\"name\": ", out);
            json_str(out, vr->name); fputs(",", out);
            JJ(level + 2); fprintf(out, "\"value\": %lld",
                                   (long long)vr->value);
            JJ(level + 1); fputc('}', out);
            if (v + 1 < dl->nvariants) fputc(',', out);
        }
        JJ(level); fputc(']', out);
    }
    return ferror(out) ? -1 : 0;
}

int weft_dump_json(const WeftUnit *u, FILE *out)
{
    if (!u->has_layout) return -1;
    int bad = 0;
    fputs("{", out);
    JJ(1); fputs("\"weftc_version\": ", out);
    json_str(out, WEFTC_VERSION); fputs(",", out);
    JJ(1); fprintf(out, "\"ir_version\": %u,", (unsigned)WEFTC_IR_VERSION);
    JJ(1); fputs("\"path\": ", out);
    json_str(out, u->file.path); fputs(",", out);
    JJ(1); fputs("\"endianness\": ", out);
    fputs(u->endian_big ? "\"big\"" : "\"little\"", out); fputs(",", out);
    JJ(1); fputs("\"abi_hash\": ", out);
    jhx64(out, u->abi_hash); fputs(",", out);
    JJ(1); fputs("\"schema_id\": ", out);
    jhx64(out, u->schema_id); fputs(",", out);
    JJ(1); fputs("\"fnv1a64_abi\": ", out);
    jhx64(out, u->fnv_debug); fputs(",", out);
    JJ(1); fprintf(out, "\"decl_count\": %zu,", u->ndecls);
    JJ(1); fputs("\"decls\": [", out);
    for (size_t i = 0; i < u->ndecls; i++) {
        JJ(2); fputc('{', out);
        bad |= json_decl(u, i, out, 3);
        JJ(2); fputc('}', out);
        if (i + 1 < u->ndecls) fputc(',', out);
    }
    JJ(1); fputc(']', out);
    fputs("\n}\n", out);
    return bad || ferror(out) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Binary IR (WIR1) — all multi-byte integers little-endian (L3)        */
/* ------------------------------------------------------------------ */
static void bin_decl(const WeftUnit *u, size_t i, ByteBuf *b)
{
    const DeclLayout *dl = &u->layouts[i];
    bb_u8(b, dl->kind == DECL_STRUCT ? 0 : dl->kind == DECL_ENUM ? 1 : 2);
    bb_str(b, dl->name);
    bb_u64le(b, dl->size);
    bb_u64le(b, dl->align);
    bb_u64le(b, dl->align_attr);
    if (dl->kind == DECL_STRUCT) {
        uint8_t flags = 0;
        if (dl->packed) flags |= 1u << 0;
        if (dl->reordered) flags |= 1u << 1;
        bb_u8(b, flags);
        bb_u64le(b, dl->simd_attr);
        bb_u64le(b, dl->internal_pad);
        bb_u64le(b, dl->trailing_pad);
        bb_u64le(b, dl->hint_opt_size);
        bb_u32le(b, (uint32_t)dl->nfields);
        for (size_t f = 0; f < dl->nfields; f++) {
            const FieldLayout *fl = &dl->fields[f];
            bb_str(b, fl->name);
            bb_str(b, fl->type_str);
            bb_u64le(b, fl->offset);
            bb_u64le(b, fl->size);
            bb_u64le(b, fl->align);
            bb_u32le(b, fl->orig_index);
        }
        bb_u32le(b, (uint32_t)dl->nholes);
        for (size_t h = 0; h < dl->nholes; h++) {
            bb_u64le(b, dl->holes[h].offset);
            bb_u64le(b, dl->holes[h].size);
        }
    } else {
        bb_u8(b, (uint8_t)dl->backing);
        bb_u32le(b, (uint32_t)dl->nvariants);
        for (size_t v = 0; v < dl->nvariants; v++) {
            bb_str(b, dl->variants[v].name);
            bb_i64le(b, dl->variants[v].value);
        }
    }
}

int weft_dump_binary(const WeftUnit *u, ByteBuf *out)
{
    if (!u->has_layout) return -1;
    bb_bytes(out, "WIR1", 4);
    bb_u8(out, WEFTC_IR_VERSION);
    bb_u8(out, u->endian_big ? 1 : 0);
    bb_u64le(out, u->abi_hash);
    bb_u64le(out, u->schema_id);
    bb_u64le(out, u->fnv_debug);
    bb_str(out, u->file.path);
    bb_u32le(out, (uint32_t)u->ndecls);
    for (size_t i = 0; i < u->ndecls; i++)
        bin_decl(u, i, out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Verify header — compile-time layout pinning                         */
/* ------------------------------------------------------------------ */
int weft_emit_verify_header(const WeftUnit *u, FILE *out)
{
    if (!u->has_layout) return -1;
    int bad = 0;
    fprintf(out,
        "/* weftc %s verify header — generated from ", WEFTC_VERSION);
    bad |= json_str(out, u->file.path) < 0;
    fprintf(out,
        "\n * schema_id ");
    hx64(out, u->schema_id);
    fprintf(out, "  abi_hash ");
    hx64(out, u->abi_hash);
    fprintf(out, "  fnv1a64 ");
    hx64(out, u->fnv_debug);
    fprintf(out,
        "\n *\n"
        " * DO NOT EDIT — regenerate with:\n"
        " *     weftc compile ");
    bad |= json_str(out, u->file.path) < 0;
    fprintf(out,
        "\n *\n"
        " * Usage (Engineer 2 workflow): include this header AFTER the\n"
        " * generated struct definitions. The assertions pin the frozen\n"
        " * layout at consumer compile time — ABI drift becomes a hard\n"
        " * build error, at zero runtime cost.\n"
        " *\n"
        " * Decl and field names are used verbatim; the .weft grammar only\n"
        " * admits [A-Za-z_][A-Za-z0-9_]* and .weft keywords never shadow\n"
        " * these uses. C keyword collisions are a codegen mapping concern.\n"
        " */\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "\n"
        "#if defined(__cplusplus)\n"
        "#  if __cplusplus < 201103L\n"
        "#    error \"weftc verify header requires C++11 or later\"\n"
        "#  endif\n"
        "#  define WEFT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)\n"
        "#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L\n"
        "#  define WEFT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)\n"
        "#else\n"
        "#  error \"weftc verify header requires C11 or later\"\n"
        "#endif\n"
        "\n");
    fprintf(out, "#define WEFT_SCHEMA_ID    ");
    hx64(out, u->schema_id);
    fprintf(out, "ull\n#define WEFT_ABI_HASH     ");
    hx64(out, u->abi_hash);
    fprintf(out, "ull\n#define WEFT_FNV1A64_ABI ");
    hx64(out, u->fnv_debug);
    fprintf(out, "ull\n#define WEFT_ENDIANNESS_BIG %d\n",
            u->endian_big ? 1 : 0);

    for (size_t i = 0; i < u->ndecls; i++) {
        const DeclLayout *dl = &u->layouts[i];
        fprintf(out, "\n/* %s %s — %llu B, align %llu */\n",
                dl->kind == DECL_STRUCT ? "struct"
                : dl->kind == DECL_ENUM ? "enum" : "bitflags",
                dl->name, (unsigned long long)dl->size,
                (unsigned long long)dl->align);
        fprintf(out, "#define WEFT_ABI_HASH_%s ", dl->name);
        hx64(out, dl->abi_hash);
        fprintf(out, "ull\n");
        fprintf(out, "WEFT_STATIC_ASSERT(sizeof(%s) == %llu,\n"
                     "    \"weftc: `%s` size drifted (expected %llu B)\");\n",
                dl->name, (unsigned long long)dl->size,
                dl->name, (unsigned long long)dl->size);
        if (dl->kind == DECL_STRUCT) {
            for (size_t f = 0; f < dl->nfields; f++) {
                const FieldLayout *fl = &dl->fields[f];
                fprintf(out,
                    "WEFT_STATIC_ASSERT(offsetof(%s, %s) == %llu,\n"
                    "    \"weftc: `%s.%s` offset drifted (expected %llu)\");\n",
                    dl->name, fl->name, (unsigned long long)fl->offset,
                    dl->name, fl->name, (unsigned long long)fl->offset);
            }
        }
    }
    return bad || ferror(out) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Inspect — the terminal ASCII memory map                             */
/* ------------------------------------------------------------------ */
/* Pick a display char per field: first unused character of the field
 * name, else a generated letter. Deterministic in final offset order. */
static void pick_chars(const DeclLayout *dl, char *chars)
{
    char used[128];
    memset(used, 0, sizeof used);
    for (size_t f = 0; f < dl->nfields; f++) {
        const char *nm = dl->fields[f].name;
        char pick = 0;
        for (const char *p = nm; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c >= 33 && c < 127 && !used[c]) {
                pick = (char)c;
                used[c] = 1;
                break;
            }
        }
        if (!pick) {
            for (int k = 0; k < 26 && !pick; k++) {
                char c = (char)('a' + (int)(f % 26) + k);
                if (c > 'z') c = (char)(c - 26);
                if (!used[(unsigned char)c]) { pick = c; used[(unsigned char)c] = 1; }
            }
        }
        if (!pick) pick = '?';
        chars[f] = pick;
    }
}

/* Renders one row of 32 bytes at `row` (bytes row..row+32 clipped to
 * size). Fields are in final order = strictly increasing offsets, so a
 * single walking cursor resolves each byte's owner. */
static void map_row(FILE *out, const DeclLayout *dl, const char *chars,
                    uint64_t row, uint64_t limit)
{
    fprintf(out, "  0x%04llx  ", (unsigned long long)row);
    size_t fi = 0;
    /* skip fields entirely before this row */
    while (fi < dl->nfields &&
           dl->fields[fi].offset + dl->fields[fi].size <= row)
        fi++;
    for (int g = 0; g < 4; g++) {
        if (g) fputs("  ", out);
        for (int k = 0; k < 8; k++) {
            uint64_t off = row + (uint64_t)(g * 8 + k);
            char c = '.';
            if (off < limit) {
                while (fi < dl->nfields &&
                       dl->fields[fi].offset + dl->fields[fi].size <= off)
                    fi++;
                if (fi < dl->nfields) {
                    const FieldLayout *fl = &dl->fields[fi];
                    if (off >= fl->offset && off < fl->offset + fl->size)
                        c = chars[fi];
                }
            }
            fputc(off < limit ? c : ' ', out);
        }
    }
    if (row > 0 && row % 64 == 0)
        fprintf(out, "  ; cacheline %llu", (unsigned long long)(row / 64));
    fputc('\n', out);
}

static void inspect_struct(FILE *out, const WeftUnit *u,
                           const DeclLayout *dl)
{
    fprintf(out, "struct %s — %llu B, align %llu",
            dl->name, (unsigned long long)dl->size,
            (unsigned long long)dl->align);
    if (dl->packed) fputs("  [packed]", out);
    if (dl->reordered) fputs("  [reordered @optimize(packing)]", out);
    if (dl->has_align_attr)
        fprintf(out, "  [@align(%llu)]", (unsigned long long)dl->align_attr);
    if (dl->has_simd_attr)
        fprintf(out, "  [@simd(%llu)]", (unsigned long long)dl->simd_attr);
    fputs("  abi_hash ", out);
    hx64(out, dl->abi_hash);
    fputc('\n', out);

    if (dl->nfields > 0) {
        size_t tw = 10;
        for (size_t f = 0; f < dl->nfields; f++) {
            size_t l = strlen(dl->fields[f].type_str);
            if (l > tw) tw = l;
        }
        fprintf(out, "  %3s  %8s  %5s  %5s  %-*s  %s\n",
                "#", "offset", "size", "align", (int)tw, "type", "field");
        for (size_t f = 0; f < dl->nfields; f++) {
            const FieldLayout *fl = &dl->fields[f];
            fprintf(out, "  %3u  %8llu  %5llu  %5llu  %-*s  %s\n",
                    (unsigned)fl->orig_index,
                    (unsigned long long)fl->offset,
                    (unsigned long long)fl->size,
                    (unsigned long long)fl->align,
                    (int)tw, fl->type_str, fl->name);
        }
    } else {
        fputs("  (no fields)\n", out);
    }

    if (dl->nholes > 0) {
        fprintf(out, "  internal padding: %llu B in %zu hole%s\n",
                (unsigned long long)dl->internal_pad, dl->nholes,
                dl->nholes == 1 ? "" : "s");
        for (size_t h = 0; h < dl->nholes; h++)
            fprintf(out, "    [%llu, %llu)  %llu B\n",
                    (unsigned long long)dl->holes[h].offset,
                    (unsigned long long)(dl->holes[h].offset +
                                         dl->holes[h].size),
                    (unsigned long long)dl->holes[h].size);
    }
    if (dl->trailing_pad > 0)
        fprintf(out, "  trailing padding: %llu B\n",
                (unsigned long long)dl->trailing_pad);

    if (dl->size > 0 && dl->size <= 256) {
        char *chars = arena_alloc(u->ar, dl->nfields ? dl->nfields : 1, 1);
        pick_chars(dl, chars);
        fputs("  bytes (1 char = 1 B, '.' = padding, words of 8):\n", out);
        for (uint64_t row = 0; row < dl->size; row += 32)
            map_row(out, dl, chars, row, dl->size);
    } else if (dl->size > 256) {
        char *chars = arena_alloc(u->ar, dl->nfields ? dl->nfields : 1, 1);
        pick_chars(dl, chars);
        fputs("  bytes (map elided: first 128 B and last 32 B of ", out);
        fprintf(out, "%llu B):\n", (unsigned long long)dl->size);
        for (uint64_t row = 0; row < 128; row += 32)
            map_row(out, dl, chars, row, dl->size);
        fprintf(out, "    ... %llu B elided ...\n",
                (unsigned long long)(dl->size - 160));
        for (uint64_t row = dl->size - 32; row < dl->size; row += 32)
            map_row(out, dl, chars, row, dl->size);
    }
    if (dl->hint_opt_size > 0)
        fprintf(out, "  @optimize(packing) hint: %llu B (saves %llu B)\n",
                (unsigned long long)dl->hint_opt_size,
                (unsigned long long)(dl->size - dl->hint_opt_size));
}

static void inspect_enum(FILE *out, const DeclLayout *dl)
{
    const char *what = dl->kind == DECL_ENUM ? "enum" : "bitflags";
    const PrimInfo *pi = prim_info(dl->backing);
    fprintf(out, "%s %s : %s — %llu B, align %llu",
            what, dl->name, pi->name,
            (unsigned long long)dl->size, (unsigned long long)dl->align);
    if (dl->has_align_attr)
        fprintf(out, "  [@align(%llu)]", (unsigned long long)dl->align_attr);
    fputs("  abi_hash ", out);
    hx64(out, dl->abi_hash);
    fputc('\n', out);
    for (size_t v = 0; v < dl->nvariants; v++) {
        const Variant *vr = &dl->variants[v];
        if (dl->kind == DECL_BITFLAGS)
            fprintf(out, "  %s = 0x%0*llx\n", vr->name,
                    (int)(pi->size * 2), (unsigned long long)vr->value);
        else
            fprintf(out, "  %s = %lld\n", vr->name, (long long)vr->value);
    }
}

void weft_inspect(const WeftUnit *u, FILE *out)
{
    if (!u->has_layout) return;
    fprintf(out, "%s — weft-schema-v1 (endianness: %s)\n",
            u->file.path, u->endian_big ? "big" : "little");
    fputs("schema_id ", out); hx64(out, u->schema_id);
    fputs("  abi_hash ", out); hx64(out, u->abi_hash);
    fputs("  fnv1a64 ", out); hx64(out, u->fnv_debug);
    fprintf(out, "\n%zu decl%s\n", u->ndecls, u->ndecls == 1 ? "" : "s");
    for (size_t i = 0; i < u->ndecls; i++) {
        fputc('\n', out);
        if (u->layouts[i].kind == DECL_STRUCT)
            inspect_struct(out, u, &u->layouts[i]);
        else
            inspect_enum(out, &u->layouts[i]);
    }
}
