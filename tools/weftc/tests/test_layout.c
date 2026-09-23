/* test_layout.c — WL-series: the deterministic layout engine conformance
 * gate (RFC-0017 §4). Every check pins an exact byte offset, size, hole
 * or diagnostic code; layout math must be host-independent (L3).
 *
 * Run via tests/run.sh (plain + ASAN legs).
 */
#include "weftc.h"

#include <string.h>
#include <stdio.h>

static int fails = 0, checks = 0;

#define CHECK(cond, ...) do {                                              \
    checks++;                                                              \
    if (!(cond)) {                                                         \
        fails++;                                                           \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
        printf(__VA_ARGS__);                                               \
        putchar('\n');                                                     \
    }                                                                      \
} while (0)

static WeftUnit *compile_str(const char *src)
{
    return weft_compile_source("test.weft", src, strlen(src), 0);
}

static int has_diag(const WeftUnit *u, const char *code)
{
    for (size_t i = 0; i < u->diags.n; i++)
        if (u->diags.items[i].code && strcmp(u->diags.items[i].code, code) == 0)
            return 1;
    return 0;
}

static void section(const char *name)
{
    printf("WL  %-28s", name);
}

static void done(void)
{
    printf("  [%d checks]\n", checks);
    checks = 0;
}

/* field shortcuts */
static const FieldLayout *F(const WeftUnit *u, const char *s, const char *f)
{
    const DeclLayout *dl = weft_find(u, s);
    return dl ? weft_field(dl, f) : NULL;
}

int main(void)
{
    /* ------------------------------------------------ WL1: primitives */
    section("WL1 primitive geometry");
    {
        const char *prims[] = { "u8", "i8", "u16", "i16", "u32", "i32",
                                "u64", "i64", "f16", "f32", "f64", "bool" };
        const uint64_t sizes[] = { 1,1,2,2,4,4,8,8,2,4,8,1 };
        const uint64_t aligns[] = { 1,1,2,2,4,4,8,8,2,4,8,1 };
        for (int i = 0; i < 12; i++) {
            char src[128];
            snprintf(src, sizeof src, "struct T { v: %s, }", prims[i]);
            WeftUnit *u = compile_str(src);
            CHECK(u && weft_error_count(u) == 0, "%s must compile", prims[i]);
            const DeclLayout *dl = weft_find(u, "T");
            CHECK(dl && dl->size == sizes[i] && dl->align == aligns[i],
                  "%s: size/align %llu/%llu want %llu/%llu", prims[i],
                  dl ? (unsigned long long)dl->size : 0,
                  dl ? (unsigned long long)dl->align : 0,
                  (unsigned long long)sizes[i], (unsigned long long)aligns[i]);
            weft_unit_free(u);
        }
    }
    done();

    /* ------------------------------------------- WL2: natural alignment */
    section("WL2 natural alignment + holes");
    {
        WeftUnit *u = compile_str(
            "struct T { a: u8, b: u64, c: u8, }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *dl = weft_find(u, "T");
        CHECK(dl->size == 24, "size %llu want 24", (unsigned long long)dl->size);
        CHECK(dl->align == 8, "align %llu want 8", (unsigned long long)dl->align);
        CHECK(F(u, "T", "a")->offset == 0, "a@0");
        CHECK(F(u, "T", "b")->offset == 8, "b@8");
        CHECK(F(u, "T", "c")->offset == 16, "c@16");
        CHECK(dl->nholes == 1 && dl->holes[0].offset == 1 &&
              dl->holes[0].size == 7, "hole [1,8)");
        CHECK(dl->internal_pad == 7, "internal 7");
        CHECK(dl->trailing_pad == 7, "trailing 7");
        weft_unit_free(u);
    }
    done();

    /* -------------------------------------------- WL3: nested cascade */
    section("WL3 nested struct cascade");
    {
        WeftUnit *u = compile_str(
            "struct Inner { a: u16, b: u32, }\n"
            "struct Outer { p: u8, i: Inner, q: u64, }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *in = weft_find(u, "Inner");
        CHECK(in->size == 8 && in->align == 4, "Inner 8/4");
        CHECK(F(u, "Inner", "b")->offset == 4, "Inner.b@4");
        const DeclLayout *out = weft_find(u, "Outer");
        CHECK(out->size == 24 && out->align == 8, "Outer 24/8");
        CHECK(F(u, "Outer", "p")->offset == 0, "p@0");
        CHECK(F(u, "Outer", "i")->offset == 4, "i@4");
        CHECK(F(u, "Outer", "q")->offset == 16, "q@16");
        weft_unit_free(u);
    }
    done();

    /* --------------------------------------------- WL4: arrays/str/span */
    section("WL4 arrays, str, span");
    {
        WeftUnit *u = compile_str(
            "struct Inner { a: u16, b: u32, }\n"
            "struct T { arr: [u32; 5], inners: [Inner; 3], blob: [u8; 4096],"
            " name: str[32], sp1: span<u8>, sp2: span<Inner>, nest: [[u16; 3]; 2], }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const FieldLayout *a = F(u, "T", "arr");
        CHECK(a->size == 20 && a->align == 4, "[u32;5] 20/4");
        const FieldLayout *inners = F(u, "T", "inners");
        CHECK(inners->size == 24 && inners->align == 4,
              "[Inner;3] stride incl. trailing pad: %llu want 24",
              (unsigned long long)inners->size);
        CHECK(F(u, "T", "blob")->size == 4096, "[u8;4096]");
        CHECK(F(u, "T", "name")->size == 32 && F(u, "T", "name")->align == 1,
              "str[32] 32/1");
        CHECK(F(u, "T", "sp1")->size == 16 && F(u, "T", "sp1")->align == 8,
              "span<u8> 16/8");
        CHECK(F(u, "T", "sp2")->size == 16 && F(u, "T", "sp2")->align == 8,
              "span<Inner> 16/8");
        CHECK(F(u, "T", "nest")->size == 12 && F(u, "T", "nest")->align == 2,
              "[[u16;3];2] 12/2");
        weft_unit_free(u);
    }
    done();

    /* ---------------------------------------------------- WL5: @packed */
    section("WL5 @packed");
    {
        WeftUnit *u = compile_str(
            "@packed struct T { a: u8, b: u64, c: u16, }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *dl = weft_find(u, "T");
        CHECK(dl->packed, "packed flag");
        CHECK(dl->size == 11 && dl->align == 1, "size 11 align 1, got %llu/%llu",
              (unsigned long long)dl->size, (unsigned long long)dl->align);
        CHECK(F(u, "T", "b")->offset == 1, "b@1");
        CHECK(F(u, "T", "c")->offset == 9, "c@9");
        CHECK(dl->nholes == 0 && dl->internal_pad == 0, "no holes");
        /* field-level @packed tightens a single member */
        WeftUnit *u2 = compile_str(
            "struct P { a: u8, @packed b: u64, }");
        CHECK(weft_error_count(u2) == 0, "field @packed clean");
        const DeclLayout *p = weft_find(u2, "P");
        CHECK(p->size == 9 && p->align == 1 && F(u2, "P", "b")->offset == 1,
              "field-packed: size 9, b@1");
        weft_unit_free(u);
        weft_unit_free(u2);
    }
    done();

    /* ------------------------------------------------- WL6: @align(N) */
    section("WL6 @align(N) cacheline floor");
    {
        WeftUnit *u = compile_str(
            "@align(64) struct T { ctrl: u32, payload: [u8; 60], }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *dl = weft_find(u, "T");
        CHECK(dl->align == 64, "align 64");
        CHECK(dl->size == 64, "size 64");
        CHECK(dl->has_align_attr && dl->align_attr == 64, "attr recorded");
        /* field-level @align raises the member */
        WeftUnit *u2 = compile_str(
            "struct Q { a: u8, @align(8) b: u32, }");
        CHECK(weft_error_count(u2) == 0, "field @align clean");
        CHECK(F(u2, "Q", "b")->offset == 8, "b@8");
        const DeclLayout *q = weft_find(u2, "Q");
        /* the member's @align(8) raises the struct alignment too */
        CHECK(q->size == 16 && q->align == 8, "Q 16/8");
        weft_unit_free(u);
        weft_unit_free(u2);
    }
    done();

    /* --------------------------------------------------- WL7: @simd(N) */
    section("WL7 @simd(N) lane alignment");
    {
        WeftUnit *u = compile_str(
            "@simd(16) struct T { x: f32, y: f32, v: [f32; 4], t: u64, }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *dl = weft_find(u, "T");
        CHECK(dl->has_simd_attr && dl->simd_attr == 16, "simd attr recorded");
        const FieldLayout *v = F(u, "T", "v");
        CHECK(v->align == 16, "vector field raised to 16, got %llu",
              (unsigned long long)v->align);
        CHECK(v->offset == 16, "v@16");
        CHECK(F(u, "T", "t")->offset == 32, "t@32 (u64 stays natural 8)");
        CHECK(dl->size == 48 && dl->align == 16, "size 48 align 16");
        weft_unit_free(u);
    }
    done();

    /* ---------------------------------------- WL8: @optimize(packing) */
    section("WL8 @optimize(packing)");
    {
        WeftUnit *u = compile_str(
            "@optimize(packing) struct T { flags: u8, temp: f64,"
            " pressure: f32, humidity: u8, }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *dl = weft_find(u, "T");
        CHECK(dl->reordered, "reordered flag");
        CHECK(dl->size == 16, "size 16 (was 24 declared-order)");
        CHECK(F(u, "T", "temp")->offset == 0, "temp@0");
        CHECK(F(u, "T", "pressure")->offset == 8, "pressure@8");
        CHECK(F(u, "T", "flags")->offset == 12, "flags@12");
        CHECK(F(u, "T", "humidity")->offset == 13, "humidity@13");
        CHECK(F(u, "T", "flags")->orig_index == 0, "orig_index preserved");
        CHECK(dl->trailing_pad == 2, "trailing 2");
        /* the hint on the UN-optimized twin */
        WeftUnit *u2 = compile_str(
            "struct T2 { flags: u8, temp: f64, pressure: f32, humidity: u8, }");
        const DeclLayout *d2 = weft_find(u2, "T2");
        CHECK(d2->size == 24, "T2 declared-order 24");
        CHECK(d2->hint_opt_size == 16, "hint 16, got %llu",
              (unsigned long long)d2->hint_opt_size);
        weft_unit_free(u);
        weft_unit_free(u2);
    }
    done();

    /* ------------------------------------------- WL9: empty + zero-size */
    section("WL9 empty structs");
    {
        WeftUnit *u = compile_str(
            "struct Unit { }\n"
            "struct T { a: u8, u: Unit, b: u16, }");
        CHECK(u && weft_error_count(u) == 0, "clean");
        const DeclLayout *un = weft_find(u, "Unit");
        CHECK(un->size == 0 && un->align == 1, "Unit 0/1");
        const DeclLayout *t = weft_find(u, "T");
        CHECK(F(u, "T", "b")->offset == 2, "b@2");
        CHECK(t->size == 4 && t->align == 2, "T 4/2");
        weft_unit_free(u);
    }
    done();

    /* --------------------------------------------- WL10: semantic gate */
    section("WL10 semantic error battery");
    {
        struct { const char *src; const char *code; } cases[] = {
            { "struct A { b: B, } struct B { a: A, }", "WE008" },
            { "struct A { b: [B; 2], } struct B { a: A, }", "WE008" },
            { "struct T { x: Vec3f, }", "WE007" },
            { "struct T { a: u32, a: u32, }", "WE004" },
            { "struct T { a: u32, } struct T { b: u8, }", "WE005" },
            { "enum E : u8 { A = 256, }", "WE013" },
            { "enum E : i8 { A = 128, }", "WE013" },
            { "enum E : i8 { A = -129, }", "WE013" },
            { "enum E : u8 { A = 1, B = 1, }", "WE011" },
            { "enum E : i8 { A = -1, B = -1, }", "WE011" },
            { "bitflags B : i32 { A = 1, }", "WE016" },
            { "enum E : u8 { }", "WE014" },
            { "bitflags B : u8 { }", "WE014" },
            { "@align(3) struct T { a: u8, }", "WE025" },
            { "@align(8192) struct T { a: u8, }", "WE026" },
            { "@simd(12) struct T { a: u8, }", "WE025" },
            { "struct T { @align(2) a: u64, }", "WE030" },
            { "@packed struct T { @align(8) a: u64, }", "WE009" },
            { "@packed struct T { @simd(16) a: u64, }", "WE009" },
            { "struct T { @packed @align(8) a: u64, }", "WE009" },
            { "struct T { @optimize(packing) a: u8, }", "WE006" },
            { "@packed enum E : u8 { A = 1, }", "WE006" },
            { "@simd(16) enum E : u8 { A = 1, }", "WE006" },
            { "@optimize(packing) enum E : u8 { A = 1, }", "WE006" },
            { "struct T { a: u8, } struct U { t: [T; 0], }", "WE012" },
            { "struct T { a: u64, } struct U { t: [[T; 4294967295]; 4294967295], }", "WE033" },
            { "endianness little; endianness big;", "WE018" },
            { "struct T { a: span<span<u8>>, }", "WE024" },
            { "struct T { @align(8) @align(16) a: u8, }", "WE040" },
            { "enum E : u8 { A, A = 2, }", "WE010" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            WeftUnit *u = compile_str(cases[i].src);
            CHECK(u && weft_error_count(u) > 0, "case %zu must error", i);
            CHECK(has_diag(u, cases[i].code),
                  "case %zu (`%.60s`) wants %s", i, cases[i].src, cases[i].code);
            weft_unit_free(u);
        }
        /* warnings */
        WeftUnit *w1 = compile_str(
            "@packed @optimize(packing) struct T { a: u8, b: u8, }");
        CHECK(weft_error_count(w1) == 0 && has_diag(w1, "WW001"),
              "@packed+@optimize warns WW001");
        weft_unit_free(w1);
        WeftUnit *w2 = compile_str(
            "bitflags B : u8 { A = 3, C = 3, }");
        CHECK(weft_error_count(w2) == 0 && has_diag(w2, "WW002"),
              "bitflags alias warns WW002");
        weft_unit_free(w2);
    }
    done();

    /* ---------------------------------------------- WL11: endianness */
    section("WL11 endianness");
    {
        WeftUnit *u = compile_str("endianness big;\nstruct T { a: u16, }");
        CHECK(u && weft_error_count(u) == 0, "big endian accepted");
        CHECK(u->endian_big == 1, "endian_big flag");
        weft_unit_free(u);
    }
    done();

    /* ------------------------------------ WL12: malformed-input armor */
    section("WL12 malformed-input armor (L2)");
    {
        /* the empty schema is VALID (0 decls, 0 errors) — check that first */
        WeftUnit *e = compile_str("");
        CHECK(e && weft_error_count(e) == 0 && e->ndecls == 0,
              "empty schema is legal");
        weft_unit_free(e);
        const char *nasty[] = { "struct", "@", "@align(", "@align", "enum E:", "enum E :",
            "struct S { f: [u8; }", "struct S { f: span<", "endianness",
            "endianness sideways;", "\x01\x02\x03\xff struct S {",
            "/* unterminated", "struct S { a: u8 ", "@@@", "@()",
            "[[[[[[[[[[[[[[[[[[[[[", "struct 123 { }", "struct S { ,,, }",
            "struct S { a: 5, }", "enum E : u8 { = 3, }",
            "struct S { a: u8 } struct", "endianness little struct S {}",
            "struct S { a: u8, b: [str; 2], }",
            "enum E : u8 { A = 0x, }", "struct S { a: [u8; -1], }",
            "bitflags", "struct S { a: span<u8>> , }",
            "struct S { a: u8,, }", "@optimize struct S { a: u8, }",
            "@packed(8) struct S { a: u8, }", "@align() struct S { a: u8, }",
        };
        for (size_t i = 0; i < sizeof nasty / sizeof nasty[0]; i++) {
            WeftUnit *u = compile_str(nasty[i]);
            CHECK(u != NULL, "case %zu: unit built", i);
            CHECK(weft_error_count(u) > 0, "case %zu: errors reported", i);
            weft_unit_free(u); /* double-free / corruption would trap */
        }
    }
    done();

    /* -------------------------------------------- WL13: forward refs */
    section("WL13 forward references");
    {
        WeftUnit *u = compile_str(
            "struct A { b: B, } struct B { x: u32, }");
        CHECK(u && weft_error_count(u) == 0, "forward ref resolves");
        CHECK(F(u, "A", "b")->size == 4, "B known: 4");
        weft_unit_free(u);
    }
    done();

    /* -------------------------------------------- WL14: span breaks cycles */
    section("WL14 span is a reference, not a value");
    {
        WeftUnit *u = compile_str(
            "struct Node { next: span<Node>, data: [u8; 16], }");
        CHECK(u && weft_error_count(u) == 0,
              "span<Node> legal — linked topologies are non-recursive");
        const DeclLayout *dl = weft_find(u, "Node");
        CHECK(dl->size == 32 && dl->align == 8, "Node 32/8 (span is 16 B)");
        weft_unit_free(u);
    }
    done();

    /* -------------------------------------------- WL15: nesting bound */
    section("WL15 containment depth bound");
    {
        char buf[65536];
        size_t n = 0;
        for (int i = 0; i < 300; i++)
            n += (size_t)snprintf(buf + n, sizeof buf - n,
                                  "struct S%d { inner: S%d, }\n", i, i + 1);
        n += (size_t)snprintf(buf + n, sizeof buf - n,
                              "struct S300 { x: u8, }\n");
        WeftUnit *u = compile_str(buf);
        CHECK(weft_error_count(u) > 0, "deep chain must error");
        CHECK(has_diag(u, "WE038"), "WE038 depth bound");
        weft_unit_free(u);
    }
    done();

    printf("WL-series: %d failures\n", fails);
    return fails ? 1 : 0;
}
