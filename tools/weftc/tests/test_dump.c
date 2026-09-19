/* test_dump.c — WD-series: artifact emitters + golden pinning (RFC-0017 §6).
 *
 *   WD1  byte-determinism: every emitter run twice is byte-identical
 *   WD2  binary IR parses back: magic/version/endianness/hashes agree
 *   WD3  verify header pins every decl (sizeof + offsetof lines)
 *   WD4  inspect map sanity (legend, padding, cacheline markers)
 *   WD5  GOLDEN byte-compare: JSON, binary IR, verify header, inspect —
 *        any accidental change to layout, manifests, hashing or
 *        formatting breaks this gate loudly (ir_version must bump)
 *   WD6  name-free abi / name-bearing id manifest invariants
 */
#define _POSIX_C_SOURCE 200809L
#include "weftc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

static void section(const char *name) { printf("WD  %-28s", name); }
static void done(void) { printf("  [%d checks]\n", checks); checks = 0; }

/* capture an emitter into a malloc'd NUL-terminated buffer */
static char *capture_json(const WeftUnit *u)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    weft_dump_json(u, f);
    fclose(f);
    return buf;
}
static char *capture_header(const WeftUnit *u)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    weft_emit_verify_header(u, f);
    fclose(f);
    return buf;
}
static char *capture_inspect(const WeftUnit *u)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    weft_inspect(u, f);
    fclose(f);
    return buf;
}

static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

static const char *GOLDEN = "tests/golden/frames.weft";

int main(void)
{
    WeftUnit *u = weft_compile_file(GOLDEN, 0);
    CHECK(u && weft_error_count(u) == 0, "golden schema compiles");
    if (!u || weft_error_count(u) > 0) return 1;

    /* ------------------------------------------ WD1: byte determinism */
    section("WD1 emitter byte-determinism");
    {
        char *j1 = capture_json(u), *j2 = capture_json(u);
        CHECK(strcmp(j1, j2) == 0, "JSON deterministic");
        ByteBuf b1, b2;
        bb_init(&b1, u->ar);
        bb_init(&b2, u->ar);
        weft_dump_binary(u, &b1);
        weft_dump_binary(u, &b2);
        CHECK(b1.len == b2.len && memcmp(b1.data, b2.data, b1.len) == 0,
              "binary IR deterministic");
        char *h1 = capture_header(u), *h2 = capture_header(u);
        CHECK(strcmp(h1, h2) == 0, "verify header deterministic");
        char *i1 = capture_inspect(u), *i2 = capture_inspect(u);
        CHECK(strcmp(i1, i2) == 0, "inspect deterministic");
        free(j1); free(j2); free(h1); free(h2); free(i1); free(i2);
    }
    done();

    /* ---------------------------------------- WD2: binary IR parse-back */
    section("WD2 binary IR structure");
    {
        ByteBuf b;
        bb_init(&b, u->ar);
        weft_dump_binary(u, &b);
        CHECK(b.len >= 4 + 1 + 1 + 8 * 3 + 2 && memcmp(b.data, "WIR1", 4) == 0,
              "WIR1 magic");
        size_t o = 4;
        CHECK(b.data[o] == WEFTC_IR_VERSION, "ir version");
        o += 1;
        CHECK(b.data[o] == (u->endian_big ? 1 : 0), "endianness flag");
        o += 1;
        uint64_t abi = 0, sid = 0, fnv = 0;
        for (int i = 0; i < 8; i++) abi |= (uint64_t)b.data[o + i] << (8 * i);
        for (int i = 0; i < 8; i++) sid |= (uint64_t)b.data[o + 8 + i] << (8 * i);
        for (int i = 0; i < 8; i++) fnv |= (uint64_t)b.data[o + 16 + i] << (8 * i);
        CHECK(abi == u->abi_hash && sid == u->schema_id && fnv == u->fnv_debug,
              "hashes embedded in IR match the unit");
    }
    done();

    /* --------------------------------------------- WD3: verify header */
    section("WD3 verify header content");
    {
        char *h = capture_header(u);
        const char *must[] = {
            "WEFT_STATIC_ASSERT(sizeof(Kind) == 1",
            "WEFT_STATIC_ASSERT(sizeof(BigFrame) == 136",
            "WEFT_STATIC_ASSERT(offsetof(BigFrame, hdr) == 0",
            "WEFT_STATIC_ASSERT(offsetof(BigFrame, name) == 52",
            "WEFT_STATIC_ASSERT(offsetof(BigFrame, blob) == 120",
            "WEFT_STATIC_ASSERT(sizeof(TelemetryMsg) == 16",
            "WEFT_STATIC_ASSERT(offsetof(TelemetryMsg, flags) == 12",
            "WEFT_STATIC_ASSERT(sizeof(CachelineFrame) == 64",
            "#define WEFT_SCHEMA_ID",
            "#define WEFT_ABI_HASH",
        };
        for (size_t i = 0; i < sizeof must / sizeof must[0]; i++)
            CHECK(strstr(h, must[i]) != NULL, "missing `%s`", must[i]);
        free(h);
    }
    done();

    /* -------------------------------------------------- WD4: inspect */
    section("WD4 inspect map sanity");
    {
        char *s = capture_inspect(u);
        const char *must[] = {
            "weft-schema-v1 (endianness: little)",
            "struct BigFrame — 136 B, align 8",
            "bytes (1 char = 1 B, '.' = padding, words of 8)",
            "; cacheline 1",
            "internal padding",
            "[reordered @optimize(packing)]",
            "[@align(64)]",
            "span<u8>",
        };
        for (size_t i = 0; i < sizeof must / sizeof must[0]; i++)
            CHECK(strstr(s, must[i]) != NULL, "missing `%s`", must[i]);
        free(s);
    }
    done();

    /* ------------------------------------------------- WD5: goldens */
    section("WD5 golden byte-compare");
    {
        struct { const char *file; } g[] = {
            { "tests/golden/frames.json" },
            { "tests/golden/frames.weftir" },
            { "tests/golden/frames.verify.h" },
            { "tests/golden/frames.inspect.txt" },
        };
        /* JSON */
        char *j = capture_json(u);
        size_t n = 0;
        char *gold = read_file(g[0].file, &n);
        CHECK(gold && strcmp(j, gold) == 0,
              "JSON golden drift (regenerate deliberately + bump "
              "WEFTC_IR_VERSION if intended)");
        free(j); free(gold);
        /* binary IR */
        ByteBuf b;
        bb_init(&b, u->ar);
        weft_dump_binary(u, &b);
        gold = read_file(g[1].file, &n);
        CHECK(gold && n == b.len && memcmp(gold, b.data, b.len) == 0,
              "binary IR golden drift");
        free(gold);
        /* verify header */
        char *h = capture_header(u);
        gold = read_file(g[2].file, &n);
        CHECK(gold && strcmp(h, gold) == 0, "verify header golden drift");
        free(h); free(gold);
        /* inspect */
        char *s = capture_inspect(u);
        gold = read_file(g[3].file, &n);
        CHECK(gold && strcmp(s, gold) == 0, "inspect golden drift");
        free(s); free(gold);
    }
    done();

    /* ---------------------------------------- WD6: manifest invariants */
    section("WD6 manifest invariants");
    {
        ByteBuf abi, id;
        bb_init(&abi, u->ar);
        bb_init(&id, u->ar);
        weft_build_abi_manifest(u, &abi);
        weft_build_id_manifest(u, &id);
        CHECK(memcmp(abi.data, "WAB1", 4) == 0, "abi magic");
        CHECK(memcmp(id.data, "WID1", 4) == 0, "id magic");
        CHECK(id.len > abi.len, "id manifest carries the names (larger)");
        /* the golden schema has 7 decls with names -> id is longer */
        CHECK(wh64(abi.data, abi.len) == u->abi_hash, "abi hash reproducible");
        CHECK(wh64(id.data, id.len) == u->schema_id, "id hash reproducible");
    }
    done();

    weft_unit_free(u);
    printf("WD-series: %d failures\n", fails);
    return fails ? 1 : 0;
}
