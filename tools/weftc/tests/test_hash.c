/* test_hash.c — WH-series: schema fingerprint quality gate (RFC-0017 §5).
 *
 *   WH1  pinned reference vectors (regenerated via tests/gen_vectors.c)
 *   WH2  FNV-1a-64 external anchors (published test vectors)
 *   WH3  avalanche: every input bit flip must flip each output bit ~50%
 *   WH4  order + length sensitivity
 *   WH5  collision freedom over a 1M-input deterministic corpus
 *   WH6  schema mutation sensitivity: every semantic change flips hashes;
 *        pure field RENAME keeps abi_hash (wire compat) and flips
 *        schema_id (source identity) — the documented split
 *   WH7  determinism + trivia insensitivity (comments/whitespace do not
 *        change any hash — identity is semantic, not textual)
 *   WH8  WH64 and FNV-1a-64 are independent fingerprints
 */
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

static void section(const char *name) { printf("WH  %-28s", name); }
static void done(void) { printf("  [%d checks]\n", checks); checks = 0; }

static WeftUnit *compile_str(const char *src)
{
    return weft_compile_source("h.weft", src, strlen(src), 0);
}

static uint64_t H(const char *src) /* whole-schema abi hash */
{
    WeftUnit *u = compile_str(src);
    uint64_t h = u->abi_hash;
    weft_unit_free(u);
    return h;
}
static uint64_t SID(const char *src) /* whole-schema id hash */
{
    WeftUnit *u = compile_str(src);
    uint64_t h = u->schema_id;
    weft_unit_free(u);
    return h;
}
static uint64_t DECLH(const char *src, const char *decl)
{
    WeftUnit *u = compile_str(src);
    const DeclLayout *dl = weft_find(u, decl);
    uint64_t h = dl ? dl->abi_hash : 0;
    weft_unit_free(u);
    return h;
}

int main(void)
{
    /* ------------------------------------------------ WH1: pinned vectors */
    section("WH1 WH64 reference vectors");
    {
        struct { const char *in; size_t len; uint64_t want; } v[] = {
            { "", 0, 0xe220a8397b1dcdafull },
            { "a", 1, 0x9a77fac740b84baaull },
            { "ab", 2, 0xf5703bc76549e1d8ull },
            { "abc", 3, 0xd381d35ec4461542ull },
            { "abcd", 4, 0xdd053e9a4d5abf68ull },
            { "abcde", 5, 0xa1944b6af33d8058ull },
            { "abcdef", 6, 0xe3c4ee90c50fe01aull },
            { "abcdefg", 7, 0x8c507f0d91334fb0ull },
            { "abcdefgh", 8, 0x8da3a7171570a24aull },
            { "abcdefghi", 9, 0x3224a93b15815454ull },
            { "The quick brown fox jumps over the lazy dog", 43,
              0x6c025b5578b8b613ull },
            { "WABI-manifest-shaped-input-0123456789abcdef", 43,
              0x575aded8decd72fbull },
            { "AAAABBBB", 8, 0x02c20ab21443f75dull },
            { "BBBBAAAA", 8, 0xd02e7721d29f78e1ull },
        };
        for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
            uint64_t got = wh64((const uint8_t *)v[i].in, v[i].len);
            CHECK(got == v[i].want, "WH64(\"%.20s\") = 0x%016llx want 0x%016llx",
                  v[i].in, (unsigned long long)got,
                  (unsigned long long)v[i].want);
        }
        static const uint8_t z8[8] = { 0 };
        static const uint8_t f8[8] = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
        CHECK(wh64(z8, 8) == 0x702af533482ce389ull, "WH64(00 x8) pinned");
        CHECK(wh64(f8, 8) == 0x5724b89123a1b221ull, "WH64(ff x8) pinned");
    }
    done();

    /* ------------------------------------------------ WH2: FNV anchors */
    section("WH2 FNV-1a-64 external anchors");
    {
        CHECK(fnv1a64((const uint8_t *)"", 0) == 0xcbf29ce484222325ull,
              "FNV offset basis");
        CHECK(fnv1a64((const uint8_t *)"a", 1) == 0xaf63dc4c8601ec8cull,
              "FNV(\"a\") published vector");
        CHECK(fnv1a64((const uint8_t *)"foobar", 6) == 0x85944171f73967e8ull,
              "FNV(\"foobar\") published vector");
    }
    done();

    /* ------------------------------------------------ WH3: avalanche */
    section("WH3 avalanche (bit-flip spread)");
    {
        /* 48 distinct 64-byte inputs (deterministic LCG); flip every input
         * bit once -> 48*512 = 24576 trials per output bit. */
        static uint64_t flips[64];
        uint64_t trials = 0;
        uint8_t buf[64];
        for (uint64_t s = 0; s < 48; s++) {
            uint64_t st = 0x9E3779B97F4A7C15ull * (s + 1);
            for (int i = 0; i < 64; i++) {
                st ^= st << 13; st *= 7; st ^= st >> 7;
                buf[i] = (uint8_t)(st >> 32);
            }
            for (int byte = 0; byte < 64; byte++) {
                for (int bit = 0; bit < 8; bit++) {
                    uint8_t save = buf[byte];
                    buf[byte] ^= (uint8_t)(1 << bit);
                    uint64_t a = wh64(buf, 64);
                    buf[byte] = save;
                    uint64_t b = wh64(buf, 64);
                    uint64_t d = a ^ b;
                    for (int k = 0; k < 64; k++)
                        if (d & (1ull << k)) flips[k]++;
                    trials++;
                }
            }
        }
        for (int k = 0; k < 64; k++) {
            double p = (double)flips[k] / (double)trials;
            CHECK(p > 0.45 && p < 0.55,
                  "output bit %d flip rate %.4f outside [0.45,0.55]",
                  k, p);
        }
    }
    done();

    /* ------------------------------------- WH4: order + length sensitivity */
    section("WH4 order/length sensitivity");
    {
        const char *x = "AAAABBBB", *y = "BBBBAAAA";
        CHECK(wh64((const uint8_t *)x, 8) != wh64((const uint8_t *)y, 8),
              "chunk transposition must change the hash");
        const char *p1 = "ab", *p2 = "ba";
        CHECK(wh64((const uint8_t *)p1, 2) != wh64((const uint8_t *)p2, 2),
              "byte transposition must change the hash");
        static const uint8_t one[1] = { 0 };
        CHECK(wh64(one, 0) != wh64(one, 1),
              "appending a zero byte must change the hash");
        static const uint8_t z8[8] = { 0 };
        CHECK(wh64(z8, 8) != wh64(one, 0),
              "8 zero bytes differ from empty");
    }
    done();

    /* ------------------------------------------------ WH5: collisions */
    section("WH5 collision freedom (1M corpus)");
    {
        /* open-addressing set of the low 64 bits; 2^21 slots */
        static uint64_t *set; /* alloc'd once, big */
        static uint8_t *used;
        size_t cap = 1u << 21;
        set = malloc(cap * sizeof(uint64_t));
        used = calloc(cap, 1);
        CHECK(set && used, "alloc");
        if (set && used) {
            uint64_t st = 0x123456789abcdef0ull;
            int collided = 0;
            for (int i = 0; i < 1000000 && !collided; i++) {
                uint8_t buf[100];
                for (int j = 0; j < 100; j++) {
                    st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                    buf[j] = (uint8_t)(st >> 32);
                }
                buf[0] = (uint8_t)(i & 0xff);
                buf[1] = (uint8_t)((i >> 8) & 0xff);
                buf[2] = (uint8_t)((i >> 16) & 0xff);
                uint64_t h = wh64(buf, 100);
                size_t slot = (size_t)(h >> 21) & (cap - 1);
                for (;;) {
                    if (!used[slot]) {
                        used[slot] = 1;
                        set[slot] = h;
                        break;
                    }
                    if (set[slot] == h) { collided = 1; break; }
                    slot = (slot + 1) & (cap - 1);
                }
            }
            CHECK(!collided, "two distinct inputs collided in WH64");
        }
        free(set);
        free(used);
    }
    done();

    /* ------------------------------------ WH6: schema mutation sensitivity */
    section("WH6 schema mutation sensitivity");
    {
        const char *base =
            "struct T { a: u8, b: u64, c: f32, }";
        const char *mut_type =
            "struct T { a: u8, b: u32, c: f32, }";
        const char *mut_order =
            "struct T { b: u64, a: u8, c: f32, }";
        const char *mut_addfield =
            "struct T { a: u8, b: u64, c: f32, d: u16, }";
        const char *mut_rename_field =
            "struct T { a: u8, renamed: u64, c: f32, }";
        const char *mut_endian =
            "endianness big;\nstruct T { a: u8, b: u64, c: f32, }";

        uint64_t h0 = H(base), s0 = SID(base), d0 = DECLH(base, "T");

        CHECK(H(mut_type) != h0, "field type change flips abi_hash");
        CHECK(SID(mut_type) != s0, "field type change flips schema_id");
        CHECK(H(mut_order) != h0, "field order change flips abi_hash");
        CHECK(H(mut_addfield) != h0, "adding a field flips abi_hash");
        CHECK(H(mut_endian) != h0, "endianness change flips abi_hash");
        CHECK(DECLH(mut_endian, "T") != d0,
              "endianness change flips per-decl abi_hash");

        /* THE documented split: renaming a FIELD keeps wire compatibility
         * (abi manifest is name-free) but changes source identity */
        CHECK(DECLH(mut_rename_field, "T") == d0,
              "field rename keeps per-decl abi_hash (wire compat)");
        CHECK(H(mut_rename_field) == h0,
              "field rename keeps whole-schema abi_hash");
        CHECK(SID(mut_rename_field) != s0,
              "field rename flips schema_id (source identity)");

        /* renaming the STRUCT itself: per-decl hash is name-free? No —
         * the decl's own name is not in its structural record, so its
         * abi_hash stays; whole-schema hashes change. */
        const char *mut_rename_struct =
            "struct U { a: u8, b: u64, c: f32, }";
        CHECK(DECLH(mut_rename_struct, "U") == d0,
              "struct rename keeps per-decl abi_hash (same wire shape)");
        CHECK(SID(mut_rename_struct) != s0, "struct rename flips schema_id");

        /* renaming a REFERENCED type changes type strings -> all flip */
        const char *rbase =
            "struct Inner { x: u32, } struct T { i: Inner, }";
        const char *rmut =
            "struct Renamed { x: u32, } struct T { i: Renamed, }";
        CHECK(DECLH(rbase, "T") != DECLH(rmut, "T"),
              "referenced type rename flips the container's abi_hash");

        /* enum variant VALUES are ABI; variant NAMES are identity only */
        const char *ebase = "enum E : u8 { A = 1, B = 2, }";
        const char *evalue = "enum E : u8 { A = 1, B = 3, }";
        const char *ename = "enum E : u8 { A = 1, Renamed = 2, }";
        CHECK(DECLH(ebase, "E") != DECLH(evalue, "E"),
              "variant value change flips abi_hash");
        CHECK(DECLH(ebase, "E") == DECLH(ename, "E"),
              "variant rename keeps abi_hash");
        CHECK(SID(ebase) != SID(ename), "variant rename flips schema_id");

        /* decl reorder: per-decl hashes stable, whole-schema flips */
        const char *obase = "struct A { x: u8, } struct B { y: u16, }";
        const char *omut = "struct B { y: u16, } struct A { x: u8, }";
        CHECK(DECLH(obase, "A") == DECLH(omut, "A"),
              "decl reorder keeps per-decl abi_hash");
        CHECK(H(obase) != H(omut), "decl reorder flips whole abi_hash");
        CHECK(SID(obase) != SID(omut), "decl reorder flips schema_id");

        /* attributes that change layout flip; pure no-op attr spellings
         * with identical layout: @align(1) on a u8-only struct is a no-op */
        const char *abase = "struct T { a: u8, b: u8, }";
        const char *amut = "@align(1) struct T { a: u8, b: u8, }";
        CHECK(H(abase) == H(amut),
              "layout-identical @align(1) keeps abi_hash");
        CHECK(SID(abase) != SID(amut),
              "...but flips schema_id (declared intent is identity)");
    }
    done();

    /* ------------------------------------------------ WH7: determinism */
    section("WH7 determinism + trivia immunity");
    {
        const char *a = "struct T { a: u8, b: u64, }";
        const char *b = "struct T { a: u8, b: u64, }";
        const char *commented =
            "// leading comment\n"
            "/* block\n   comment */\n"
            "struct T {\n"
            "    a: u8,   // trailing\n"
            "    b: u64,\n"
            "}\n";
        CHECK(H(a) == H(b), "identical sources hash identically");
        CHECK(SID(a) == SID(b), "identical sources id identically");
        CHECK(H(a) == H(commented),
              "comments do not change abi_hash (semantic identity)");
        CHECK(SID(a) == SID(commented),
              "comments do not change schema_id (semantic identity)");
        WeftUnit *u1 = compile_str(a), *u2 = compile_str(commented);
        CHECK(u1->fnv_debug == u2->fnv_debug, "fnv parity too");
        weft_unit_free(u1);
        weft_unit_free(u2);
    }
    done();

    /* ------------------------------------------------ WH8: independence */
    section("WH8 fingerprint independence");
    {
        WeftUnit *u = compile_str("struct T { a: u8, b: u64, }");
        ByteBuf abi;
        bb_init(&abi, u->ar);
        weft_build_abi_manifest(u, &abi);
        CHECK(wh64(abi.data, abi.len) == u->abi_hash,
              "abi_hash = WH64(abi manifest) — reproducible");
        CHECK(fnv1a64(abi.data, abi.len) == u->fnv_debug,
              "fnv_debug = FNV(abi manifest) — reproducible");
        CHECK(u->abi_hash != u->fnv_debug,
              "WH64 and FNV-1a are independent fingerprints");
        CHECK(u->abi_hash != u->schema_id,
              "abi manifest and id manifest are domain-separated");
        weft_unit_free(u);
    }
    done();

    printf("WH-series: %d failures\n", fails);
    return fails ? 1 : 0;
}
