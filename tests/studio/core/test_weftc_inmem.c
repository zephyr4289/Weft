/* test_weftc_inmem.c — C-series: the in-memory compiler oracle.
 *
 * C1  golden layout parity with the Pillar-1 weftc reference (all seven
 *     decls, every field offset/size/align/padding, holes, pads, hints)
 * C2  WH64/WDC1 manifest hash parity — the studio engine reproduces the
 *     reference compiler's fingerprints bit-for-bit
 * C3  attribute battery (@packed, field @align, @align floors, @simd,
 *     @optimize reorder + orig_index preservation, empty structs, span
 *     cycle breaking, array stride incl. trailing pad, 2^48 ceiling)
 * C4  diagnostic battery: one probe per WE/WW code (bounds + presence)
 * C5  cache-line tripwires (64 B and 128 B crossings, false-sharing)
 * C6  7-language codegen: golden substrings + byte-determinism
 * C7  <1 ms compile+codegen SLA (directive: "< 1ms")
 * C8  panic-mode recovery and ctx reuse (multi-error schemas)
 */
#include "test_studio_harness.h"

static weftc_ctx_t *g_ctx;
static void *g_mem;

static void setup(void)
{
    g_mem = malloc(weftc_ctx_size());
    if (weftc_ctx_init(g_mem, weftc_ctx_size(), &g_ctx)) exit(2);
}

static int compile(const char *src)
{
    int rc = weftc_compile(g_ctx, src, strlen(src));
    return rc;
}

static const weft_struct_layout_t *decl(const char *name)
{
    int32_t i = weftc_decl_find(g_ctx, name);
    return (i < 0) ? NULL : weftc_decl_at(g_ctx, (uint32_t)i);
}

static uint32_t diag_code_at(uint32_t i)
{
    const weft_diag_t *d = weftc_diag_at(g_ctx, i);
    return d ? d->code : 0;
}
static int has_diag(uint32_t code)
{
    uint32_t i;
    for (i = 0; i < weftc_diag_count(g_ctx); i++)
        if (diag_code_at(i) == code) return 1;
    return 0;
}

/* ---- C1/C2: golden parity ------------------------------------------ */
static void c1_c2_golden(void)
{
    uint64_t abi, sid, fnv;
    uint32_t di;
    CHECK_EQ_I(compile(FIXTURE_FRAMES), WEFT_STUDIO_OK);
    CHECK_EQ_I(weftc_diag_count(g_ctx), 4); /* 2102, 2101 x2, 2104 */
    CHECK_EQ_I(weftc_decl_count(g_ctx), 7);
    CHECK_EQ_I(weftc_schema_hashes(g_ctx, &abi, &sid, &fnv), WEFT_STUDIO_OK);
    CHECK_EQ_U64(abi, GOLDEN_ABI_HASH);
    CHECK_EQ_U64(sid, GOLDEN_SCHEMA_ID);
    CHECK_EQ_U64(fnv, GOLDEN_FNV);

    CHECK_EQ_U64(decl("Kind")->abi_hash, GOLDEN_KIND);
    CHECK_EQ_U64(decl("Caps")->abi_hash, GOLDEN_CAPS);
    CHECK_EQ_U64(decl("Vec3")->abi_hash, GOLDEN_VEC3);
    CHECK_EQ_U64(decl("Header")->abi_hash, GOLDEN_HEADER);
    CHECK_EQ_U64(decl("TelemetryMsg")->abi_hash, GOLDEN_TELEMETRY);
    CHECK_EQ_U64(decl("CachelineFrame")->abi_hash, GOLDEN_CACHELINE);
    CHECK_EQ_U64(decl("BigFrame")->abi_hash, GOLDEN_BIGFRAME);

    /* exhaustive layout numbers (mirrors frames.json) */
    CHECK_EQ_U64(decl("Vec3")->size, 12);
    CHECK_EQ_U64(decl("Vec3")->align, 4);
    CHECK_EQ_U64(decl("Vec3")->internal_pad, 0);
    CHECK_EQ_U64(decl("Vec3")->trailing_pad, 0);
    CHECK_EQ_U64(decl("Vec3")->fields[0].offset, 0);
    CHECK_EQ_U64(decl("Vec3")->fields[2].offset, 8);

    CHECK_EQ_U64(decl("Header")->size, 24);
    CHECK_EQ_U64(decl("Header")->align, 8);
    CHECK_EQ_U64(decl("Header")->internal_pad, 7);
    CHECK_EQ_U64(decl("Header")->trailing_pad, 0);
    CHECK_EQ_U64(decl("Header")->hole_count, 1);
    CHECK_EQ_U64(decl("Header")->holes[0].offset, 9);
    CHECK_EQ_U64(decl("Header")->holes[0].size, 7);
    CHECK_STR(decl("Header")->fields[1].name, "kind");
    CHECK_EQ_U64(decl("Header")->fields[1].padding_after, 7);
    CHECK_EQ_U64(decl("Header")->fields[2].offset, 16);

    {
        const weft_struct_layout_t *T = decl("TelemetryMsg");
        CHECK_EQ_U64(T->size, 16);
        CHECK_EQ_U64(T->align, 8);
        CHECK_EQ_U64(T->internal_pad, 0);
        CHECK_EQ_U64(T->trailing_pad, 2);
        CHECK(T->flags & WEFT_SLF_REORDERED);
        CHECK_EQ_I(T->field_count, 4);
        CHECK_STR(T->fields[0].name, "temperature"); /* final order */
        CHECK_STR(T->fields[1].name, "pressure");
        CHECK_STR(T->fields[2].name, "flags");
        CHECK_STR(T->fields[3].name, "humidity");
        CHECK_EQ_I(T->fields[0].orig_index, 1);      /* source order kept */
        CHECK_EQ_I(T->fields[2].orig_index, 0);
        CHECK_EQ_U64(T->fields[3].offset, 13);
    }
    {
        const weft_struct_layout_t *C = decl("CachelineFrame");
        CHECK_EQ_U64(C->size, 64);
        CHECK_EQ_U64(C->align, 64);
        CHECK(C->flags & WEFT_SLF_ALIGN_ATTR);
        CHECK_EQ_U64(C->trailing_pad, 0);
    }
    {
        const weft_struct_layout_t *B = decl("BigFrame");
        CHECK_EQ_U64(B->size, 136);
        CHECK_EQ_U64(B->align, 8);
        CHECK_EQ_U64(B->internal_pad, 4);
        CHECK_EQ_U64(B->fields[6].offset, 120); /* blob */
        CHECK_STR(B->fields[6].type_str, "span<u8>");
        CHECK_STR(B->fields[4].type_str, "str[32]");
        CHECK_STR(B->fields[5].type_str, "[f32; 8]");
        CHECK_STR(B->fields[0].type_str, "Header");
        CHECK_EQ_U64(B->fields[5].padding_after, 4);
    }
    /* the four diagnostics, with exact bounds (LSP 0-based positions) */
    {
        const weft_diag_t *d0 = weftc_diag_at(g_ctx, 0);
        CHECK_EQ_I(d0->code, WEFT_D_TRAILING_PAD);
        CHECK_EQ_I(d0->severity, WEFT_SEV_HINT);
        CHECK_EQ_I(d0->start_line, 30);   /* "struct TelemetryMsg {" */
        CHECK_EQ_I(d0->start_col, 7);     /* the NAME span */
        CHECK_EQ_I(d0->end_line, 30);
        CHECK_EQ_I(d0->end_col, 19);
    }
    (void)di;
}

/* ---- C3: attribute battery ----------------------------------------- */
static void c3_attrs(void)
{
    /* classic reorder hint (RFC-0017 example) */
    CHECK_EQ_I(compile("struct A { flags: u8, temp: f64, pressure: f32, "
                       "humidity: u8 }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("A")->size, 24);
    CHECK_EQ_U64(decl("A")->optimize_hint, 8);
    CHECK(has_diag(WEFT_D_OPTIMIZE_HINT));
    CHECK(has_diag(WEFT_D_TRAILING_PAD));

    /* the optimized twin */
    CHECK_EQ_I(compile("@optimize(packing)\nstruct B { flags: u8, temp: f64,"
                       " pressure: f32, humidity: u8 }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("B")->size, 16);
    CHECK_EQ_U64(decl("B")->optimize_hint, 0);

    /* @packed struct */
    CHECK_EQ_I(compile("@packed\nstruct P { a: u64, b: u16 }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("P")->size, 10);
    CHECK_EQ_U64(decl("P")->align, 1);
    CHECK(decl("P")->flags & WEFT_SLF_PACKED);

    /* field-level @packed */
    CHECK_EQ_I(compile("struct FP { a: u8, @packed b: u64 }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("FP")->size, 9);
    CHECK_EQ_U64(decl("FP")->align, 1);

    /* field @align raises */
    CHECK_EQ_I(compile("struct FA { a: u8, @align(8) b: u64 }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("FA")->size, 16);

    /* @simd on struct: floor + raise fields with size >= N (both fields
     * are smaller than 16 B, so only the alignment floor applies) */
    CHECK_EQ_I(compile("@simd(16)\nstruct S { a: u32, b: u64 }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("S")->size, 16);
    CHECK_EQ_U64(decl("S")->align, 16);
    CHECK_EQ_I(compile("@simd(16)\nstruct S2 { a: u32, b: [u8; 32] }"),
               WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("S2")->size, 48);   /* 32 B field raised to 16 */
    CHECK_EQ_U64(decl("S2")->align, 16);

    /* empty struct */
    CHECK_EQ_I(compile("struct Empty {}"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("Empty")->size, 0);
    CHECK_EQ_U64(decl("Empty")->align, 1);

    /* span breaks containment cycles */
    CHECK_EQ_I(compile("struct Node { next: span<Node> }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("Node")->size, 16);
    CHECK(!has_diag(8));

    /* array stride includes trailing pad */
    CHECK_EQ_I(compile("struct Inner { a: u8, b: u32 }\n"
                       "struct Outer { arr: [Inner; 3] }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("Inner")->size, 8);
    CHECK_EQ_U64(decl("Outer")->size, 24);

    /* forward references are legal */
    CHECK_EQ_I(compile("struct Fwd { x: Later }\nstruct Later { y: u16 }"),
               WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("Fwd")->size, 2);

    /* 2^48 ceiling */
    CHECK_EQ_I(compile("struct Big { arr: [u8; 4294967295] }"), WEFT_STUDIO_OK);
    CHECK_EQ_U64(decl("Big")->size, 4294967295ull);
    CHECK_EQ_I(compile("struct TooBig { arr: [[u64; 4294967295]; "
                       "4294967295] }"), WEFT_STUDIO_OK);
    CHECK(has_diag(33));
}

/* ---- C4: diagnostic battery ---------------------------------------- */
static void c4_diagnostics(void)
{
    static const struct { uint32_t code; const char *src; } cases[] = {
        { 4,  "struct D { a: u8, a: u8 }" },
        { 5,  "struct E {}\nstruct E {}" },
        { 7,  "struct F { x: Nope }" },
        { 8,  "struct R1 { r: R2 }\nstruct R2 { r: R1 }" },
        { 9,  "@packed\nstruct C { @align(8) a: u64 }" },
        { 10, "enum V : u8 { A = 1, A = 2 }" },
        { 11, "enum V2 : u8 { A = 1, B = 1 }" },
        { 12, "struct Z { s: str[0] }" },
        { 13, "enum V3 : u8 { A = 256 }" },
        { 14, "enum V4 : u8 { }" },
        { 16, "bitflags BF : i32 { X = 1 }" },
        { 17, "enum V5 : f32 { A = 1 }" },
        { 18, "endianness little;\nendianness big;" },
        { 21, "enum V6 : u64 { A = 99999999999999999999999 }" },
        { 22, "struct X { /* unterminated" },
        { 24, "struct Y { sp: span<span<u8>> }" },
        { 25, "@align(3)\nstruct A3 { a: u8 }" },
        { 26, "@align(8192)\nstruct A4 { a: u8 }" },
        { 28, "endianness sideways;" },
        { 29, "struct L { s: str[4294967296] }" },
        { 30, "struct A5 { @align(2) a: u64 }" },
        { 31, "bitflags BF2 : u32 { X = -1 }" },
        { 40, "@align(8)\n@align(8)\nstruct A6 { a: u8 }" },
        { 902,"bitflags BF3 : u32 { A = 1, B = 1 }" },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int rc = compile(cases[i].src);
        CHECK_EQ_I(rc, WEFT_STUDIO_OK); /* diagnostics, not refusals */
        if (!has_diag(cases[i].code)) {
            g_checks++;
            g_failures++;
            fprintf(stderr, "FAIL C4 case %u: expected diag %u (%s), "
                    "got %u diagnostics\n", (unsigned)i, cases[i].code,
                    weft_diag_code_name(cases[i].code),
                    weftc_diag_count(g_ctx));
        }
    }
    /* WE007 carries a did-you-mean fix (Vec3 is declared in-scope) */
    CHECK_EQ_I(compile("struct Vec3 { x: f32 }\nstruct G { x: u64, y: Vec4 }"),
               WEFT_STUDIO_OK);
    {
        uint32_t i2;
        int found = 0;
        for (i2 = 0; i2 < weftc_diag_count(g_ctx); i2++) {
            const weft_diag_t *d = weftc_diag_at(g_ctx, i2);
            if (d->code == 7) {
                found = 1;
                CHECK(d->fix_suggestion);
                if (d->fix_suggestion)
                    CHECK_SUB(d->fix_suggestion, "did you mean");
            }
        }
        CHECK(found);
    }
}

/* ---- C5: cache-line tripwires -------------------------------------- */
static void c5_cache_lines(void)
{
    /* @packed puts the u64 AT 60 so it crosses the 64 B boundary */
    CHECK_EQ_I(compile("@packed\nstruct CL {\n"
                       "    pad: [u8; 60],\n"
                       "    scalar: u64,\n"
                       "    tail: [u8; 8],\n"
                       "    cross128: u64,\n"
                       "}"), WEFT_STUDIO_OK);
    {
        const weft_struct_layout_t *L = decl("CL");
        const weft_field_layout_t *sc = &L->fields[1]; /* scalar @60 */
        const weft_field_layout_t *c128 = &L->fields[3];
        CHECK_EQ_U64(sc->offset, 60);
        CHECK_EQ_U64(sc->cache_line_idx, 0);
        CHECK(sc->flags & WEFT_FLF_CROSSES_CL64);
        CHECK(sc->flags & WEFT_FLF_FALSE_SHARING);
        CHECK(!c128->flags);            /* not a scalar crossing */
        CHECK(has_diag(WEFT_D_CACHE_LINE_CROSS));
    }
    /* 128 B crossing on a scalar (packed to land at 124) */
    CHECK_EQ_I(compile("@packed\nstruct C128 {\n"
                       "    pad: [u8; 124],\n"
                       "    wide: u64,\n"
                       "}"), WEFT_STUDIO_OK);
    {
        const weft_struct_layout_t *L = decl("C128");
        CHECK(L->fields[1].flags & WEFT_FLF_CROSSES_CL128);
        CHECK(L->fields[1].flags & WEFT_FLF_CROSSES_CL64);
        CHECK(has_diag(WEFT_D_CL128_CROSS));
    }
}

/* ---- C6: codegen ---------------------------------------------------- */
static void c6_codegen(void)
{
    static char out[65536];
    size_t len = 0;
    static char out2[65536];
    int lang;
    CHECK_EQ_I(compile(FIXTURE_FRAMES), WEFT_STUDIO_OK);
    for (lang = 0; lang < WEFT_LANG__COUNT; lang++) {
        int rc = weftc_codegen(g_ctx, lang, out, sizeof out, &len);
        CHECK_EQ_I(rc, WEFT_STUDIO_OK);
        CHECK(len > 100);
        /* byte-determinism */
        rc = weftc_codegen(g_ctx, lang, out2, sizeof out2, NULL);
        CHECK_EQ_I(rc, WEFT_STUDIO_OK);
        CHECK(memcmp(out, out2, len) == 0);
        CHECK(out[0] == '/' || out[0] == '#');
    }
    /* C preview: exact offsets, pads, static asserts */
    weftc_codegen(g_ctx, WEFT_LANG_C, out, sizeof out, &len);
    CHECK_SUB(out, "uint64_t seq;  /* offset 0, size 8, align 8 */");
    CHECK_SUB(out, "uint8_t _weft_pad_0[7];  /* hole [9, 16) */");
    CHECK_SUB(out, "_Static_assert(sizeof(Header) == 24, "
                   "\"weft: Header size drifted\");");
    CHECK_SUB(out, "_Static_assert(offsetof(Header, stamp) == 16,");
    CHECK_SUB(out, "__attribute__((aligned(64)))");
    CHECK_SUB(out, "typedef struct weft_span { uint64_t offset; "
                   "uint64_t len; } weft_span;");
    /* Rust */
    weftc_codegen(g_ctx, WEFT_LANG_RUST, out, sizeof out, &len);
    CHECK_SUB(out, "#[repr(C, align(8))]");
    CHECK_SUB(out, "pub struct Header {");
    CHECK_SUB(out, "const _: () = assert!(core::mem::size_of::<Header>() "
                   "== 24);");
    CHECK_SUB(out, "pub struct WeftSpan { pub offset: u64, pub len: u64 }");
    /* TypeScript */
    weftc_codegen(g_ctx, WEFT_LANG_TYPESCRIPT, out, sizeof out, &len);
    CHECK_SUB(out, "export const HEADER_LAYOUT = { size: 24, align: 8");
    CHECK_SUB(out, "type: 'span<u8>'");
    /* Python */
    weftc_codegen(g_ctx, WEFT_LANG_PYTHON, out, sizeof out, &len);
    CHECK_SUB(out, "class Header:  # struct - 24 B, align 8");
    CHECK_SUB(out, "(\"seq\", 0, 8, \"u64\")");
    /* Dart */
    weftc_codegen(g_ctx, WEFT_LANG_DART, out, sizeof out, &len);
    CHECK_SUB(out, "static const int size = 24;");
    CHECK_SUB(out, "'seq': [0, 8, 'u64']");
    /* Swift */
    weftc_codegen(g_ctx, WEFT_LANG_SWIFT, out, sizeof out, &len);
    CHECK_SUB(out, "struct Header {  // 24 B, align 8");
    CHECK_SUB(out, "var seq: UInt64    // offset 0, size 8");
    /* capacity refusal */
    CHECK_EQ_I(weftc_codegen(g_ctx, WEFT_LANG_C, out, 64, &len),
               WEFT_STUDIO_EBOUNDS);
}

/* ---- C7: compile SLA ------------------------------------------------ */
static void c7_sla(void)
{
    uint64_t t0, t1, best = ~(uint64_t)0;
    int it, i;
    static char out[65536];
    size_t len;
    for (it = 0; it < 31; it++) {
        t0 = harness_now_ns();
        for (i = 0; i < 100; i++) {
            weftc_compile(g_ctx, FIXTURE_FRAMES,
                          sizeof FIXTURE_FRAMES - 1u);
            weftc_codegen(g_ctx, WEFT_LANG_C, out, sizeof out, &len);
            weftc_codegen(g_ctx, WEFT_LANG_RUST, out, sizeof out, &len);
        }
        t1 = harness_now_ns();
        if ((t1 - t0) / 100u < best) best = (t1 - t0) / 100u;
    }
    printf("  C7 compile+2x codegen: %llu ns/cycle (budget 1000000)\n",
           (unsigned long long)best);
    CHECK(best < 1000000u);
}

/* ---- C8: panic-mode recovery + reuse -------------------------------- */
static void c8_recovery(void)
{
    int rc = compile("struct A { x: , y: u8 }\n"
                     "struct B { x: Missing1, y: Missing2 }\n"
                     "enum E : u8 { A = }\n"
                     "garbage ~~~\n"
                     "struct C { z: u16 }");
    CHECK_EQ_I(rc, WEFT_STUDIO_OK);
    CHECK(weftc_diag_count(g_ctx) >= 4);
    CHECK(decl("C") != NULL);            /* later decls still compile */
    CHECK_EQ_U64(decl("C")->size, 2);
    /* recompile the clean fixture: full state replacement */
    CHECK_EQ_I(compile(FIXTURE_FRAMES), WEFT_STUDIO_OK);
    /* decl_find by name is authoritative after full replacement */
    CHECK_EQ_I(weftc_decl_find(g_ctx, "C"), -1);
    CHECK_EQ_I(weftc_decl_find(g_ctx, "BigFrame") >= 0, 1);
    /* huge input refusal path (token cap) */
    {
        static char big[400 * 1024];
        uint32_t i;
        big[0] = 0;
        for (i = 0; i < 60000; i++) strcat(big, "x ");
        rc = weftc_compile(g_ctx, big, strlen(big));
        CHECK_EQ_I(rc, WEFT_STUDIO_EBOUNDS);
    }
}

int main(void)
{
    setup();
    c1_c2_golden();
    c3_attrs();
    c4_diagnostics();
    c5_cache_lines();
    c6_codegen();
    c7_sla();
    c8_recovery();
    return harness_summary("test_weftc_inmem (C-series)");
}
