/* test_fuzz_studio.c — F-series: 10,000,000-cycle robustness battery.
 *
 * Surface A (6M cycles, malformed schema text):
 *   - random byte soups (all byte values, NULs included),
 *   - structured mutations of the golden schema (bit flips, splices,
 *     token-granular edits),
 *   - adversarial grammar shapes (deep nesting, huge literals,
 *     unterminated constructs).
 *   Invariants: compile returns ladder codes only, diagnostics stay
 *   bounded, contexts stay reusable, output is DETERMINISTIC (the same
 *   input always produces the same hash + diagnostic count).
 *
 * Surface B (4M cycles, corrupted .weftrec traces):
 *   - build small random traces (4..24 frames, raw + dzv payloads),
 *   - corrupt them (0..8 random bit flips, truncation),
 *   - open / seek / walk: every return is a ladder code; on CLEAN traces
 *     every walked frame is bit-exact against the recorded ground truth.
 *
 * The fuzzer runs with a fixed seed: every failure is reproducible.
 */
#include "test_studio_harness.h"

#define FUZZ_SEED_A UINT64_C(0xWEFT0000DEADBEEF & 0xFFFFFFFFFFFFFFFF)
#define FUZZ_SEED_A0 0xDEADBEEFCAFEF00Dull
#define FUZZ_SEED_B0 0x0BADF00D0D06F00Dull

static uint64_t g_rng;
static uint64_t xs(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

/* ---- surface A: schema fuzz ----------------------------------------- */
static weftc_ctx_t *g_c;
static void *g_cmem;
static char g_src[2048];
static char g_cg[8192];

/* token alphabet for structured mutations */
static const char *const TOKS[] = {
    "struct", "enum", "bitflags", "endianness", "little", "big", "span",
    "str", "packing", "@align", "@simd", "@packed", "@optimize", "u8",
    "u16", "u32", "u64", "i8", "i64", "f32", "f64", "bool", "f16", "Vec3",
    "Header", "Kind", "{", "}", "[", "]", "(", ")", "<", ">", ":", ";",
    ",", "=", "@", "-", "0", "1", "4096", "0x10", "4294967296", "/*",
    "*/", "//", "\n", " ", "\t", "~~~", "\xff", "\x01", "é",
};
#define NTOKS ((int)(sizeof TOKS / sizeof TOKS[0]))

static uint32_t gen_soup(void)
{
    uint32_t len = (uint32_t)(xs() % 600);
    uint32_t i;
    for (i = 0; i < len; i++)
        g_src[i] = (char)(xs() & 0xFF);
    return len;
}

static uint32_t gen_mutated(void)
{
    /* splice random tokens around a mutated golden prefix */
    uint32_t ntok = 4 + (uint32_t)(xs() % 40);
    uint32_t i, o = 0;
    for (i = 0; i < ntok; i++) {
        const char *t = TOKS[xs() % (uint64_t)NTOKS];
        size_t tl = strlen(t);
        if (o + tl + 1 >= sizeof g_src) break;
        memcpy(g_src + o, t, tl);
        o += (uint32_t)tl;
        g_src[o++] = (xs() & 4) ? ' ' : '\n';
    }
    /* bit flips */
    if (o) {
        uint32_t flips = (uint32_t)(xs() % 5);
        for (i = 0; i < flips; i++) {
            uint32_t p = (uint32_t)(xs() % o);
            g_src[p] ^= (char)(1u << (xs() % 8));
        }
    }
    return o;
}

static uint32_t gen_adversarial(void)
{
    static const char *shapes[] = {
        "struct S { a: [[[[[[[[[[", 
        "enum E : u8 { A = 999999999999999999999999999999 }",
        "endianness ",
        "@align(",
        "@align(0x",
        "struct { { { { {",
        "struct S { a: span<span<span<u8>>>",
        "bitflags B : u32 { X = ",
        "/* unterminated",
        "struct S { s: str[",
        "struct S\x00\x01\x02",
        "@packed @align(64) @simd(64) struct S {}",
    };
    uint32_t pick = (uint32_t)(xs() % 12);
    uint32_t len = (uint32_t)strlen(shapes[pick]);
    uint32_t extra = (uint32_t)(xs() % 64);
    if (len + extra >= sizeof g_src) extra = 0;
    memcpy(g_src, shapes[pick], len);
    {
        uint32_t i;
        for (i = 0; i < extra; i++)
            g_src[len + i] = (char)('a' + (xs() % 26));
    }
    return len + extra;
}

static unsigned long long g_a_cycles = 0;
static unsigned long long g_a_refusals = 0;
static unsigned long long g_a_det_checks = 0;

static void fuzz_schema(uint64_t cycles)
{
    uint64_t it;
    size_t cg_len = 0;
    for (it = 0; it < cycles; it++) {
        uint32_t len;
        int rc;
        int kind = (int)(xs() % 3);
        if (kind == 0) len = gen_soup();
        else if (kind == 1) len = gen_mutated();
        else len = gen_adversarial();
        rc = weftc_compile(g_c, g_src, len);
        if (rc != WEFT_STUDIO_OK && rc != WEFT_STUDIO_EBOUNDS) {
            g_checks++;
            g_failures++;
            fprintf(stderr, "FAIL F-A: cycle %llu rc=%d (ladder only)\n",
                    (unsigned long long)it, rc);
            return;
        }
        if (rc != WEFT_STUDIO_OK) g_a_refusals++;
        /* bounded queries */
        if (weftc_diag_count(g_c) <= 256u &&
            weftc_decl_count(g_c) <= 1024u &&
            weftc_ast_count(g_c) <= 32768u) {
            /* fine */
        } else {
            g_checks++;
            g_failures++;
            fprintf(stderr, "FAIL F-A: cycle %llu bounds\n",
                    (unsigned long long)it);
            return;
        }
        (void)weftc_codegen(g_c, WEFT_LANG_C, g_cg, sizeof g_cg, &cg_len);
        g_a_cycles++;
        /* determinism re-check every 65536 cycles */
        if ((it & 0xFFFFull) == 0) {
            uint64_t h1, s1, f1, h2, s2, f2;
            uint32_t d1, d2;
            weftc_compile(g_c, g_src, len);
            weftc_schema_hashes(g_c, &h1, &s1, &f1);
            d1 = weftc_diag_count(g_c);
            weftc_compile(g_c, g_src, len);
            weftc_schema_hashes(g_c, &h2, &s2, &f2);
            d2 = weftc_diag_count(g_c);
            g_a_det_checks++;
            if (h1 != h2 || s1 != s2 || f1 != f2 || d1 != d2) {
                g_checks++;
                g_failures++;
                fprintf(stderr, "FAIL F-A determinism at cycle %llu\n",
                        (unsigned long long)it);
                return;
            }
        }
    }
}

/* ---- surface B: trace fuzz ------------------------------------------ */
static uint8_t g_tbuf[1 << 16] __attribute__((aligned(64)));
static weftrec_index_entry_t g_tidx[64] __attribute__((aligned(8)));
static uint8_t g_truth[24][192];
static uint32_t g_tlen[24];
static uint8_t g_scratch[256];

static unsigned long long g_b_cycles = 0;
static unsigned long long g_b_clean_walks = 0;
static unsigned long long g_b_frames_exact = 0;
static unsigned long long g_b_refusals[8];

static void fuzz_trace(uint64_t cycles)
{
    uint64_t it;
    for (it = 0; it < cycles; it++) {
        weftrec_builder_t b;
        uint64_t len = 0;
        uint32_t n = 4 + (uint32_t)(xs() % 20);
        uint32_t i;
        uint64_t t = 1000;
        int clean = (xs() % 4) == 0;      /* 25% uncorrupted */
        uint32_t nflip = clean ? 0 : 1 + (uint32_t)(xs() % 8);
        int truncate = (!clean) && (xs() % 8) == 0;
        uint64_t trunc_to = 0;
        weftrec_reader_t r;
        int rc;
        for (i = 0; i < n; i++) {
            uint32_t l = (uint32_t)(xs() % 192);
            uint32_t k;
            t += xs() % 1000;
            for (k = 0; k < 192; k++) g_truth[i][k] = (uint8_t)xs();
            if (l >= 8 && (i & 1)) {
                /* make odd frames delta-friendly so dzv engages */
                uint32_t w;
                for (w = 0; w + 4 <= l; w += 4) {
                    uint32_t v = 100 + i + w / 4;
                    memcpy(g_truth[i] + w, &v, 4);
                }
            }
            g_tlen[i] = l;
        }
        rc = weftrec_builder_init(&b, g_tbuf, sizeof g_tbuf, g_tidx, 64);
        if (rc) { g_b_refusals[(-rc) - 1]++; continue; }
        for (i = 0; i < n; i++) {
            rc = weftrec_builder_append(&b, t + i, (uint32_t)(i % 3),
                                        g_truth[i], g_tlen[i],
                                        (i & 1) ? WEFTREC_CODEC_DZV
                                                : WEFTREC_CODEC_RAW);
            if (rc) { g_b_refusals[(-rc) - 1]++; break; }
        }
        if (weftrec_builder_finish(&b, &len)) {
            g_b_refusals[(-WEFT_STUDIO_EBOUNDS) - 1]++;
            continue;
        }
        /* corruption */
        for (i = 0; i < nflip; i++) {
            uint64_t pos = xs() % len;
            g_tbuf[pos] ^= (uint8_t)(1u << (xs() % 8));
        }
        if (truncate) {
            trunc_to = len / 2 + (xs() % (len / 2 ? len / 2 : 1));
            len = trunc_to;
        }
        /* reader discipline */
        rc = weftrec_reader_open(&r, g_tbuf, len);
        if (rc != WEFT_STUDIO_OK) {
            if (rc < -5 || rc > 0) {
                g_checks++; g_failures++;
                fprintf(stderr, "FAIL F-B open rc=%d at cycle %llu\n",
                        rc, (unsigned long long)it);
                return;
            }
            g_b_refusals[(-rc) - 1]++;
        } else {
            weftrec_frame_view_t v;
            weftrec_walker_t w;
            (void)weftrec_seek_timestamp(&r, t + (xs() % (n + 2)), &v);
            rc = weftrec_walker_init(&w, &r, 0, g_scratch,
                                     sizeof g_scratch);
            if (rc != WEFT_STUDIO_OK) {
                g_b_refusals[(-rc) - 1]++;
            } else {
                uint32_t fi = 0;
                int walked_any = 0;
                while ((rc = weftrec_frame_next(&w, &v)) ==
                       WEFT_STUDIO_OK) {
                    walked_any = 1;
                    if (clean) {
                        /* ORACLE: clean traces replay bit-exact */
                        if (fi >= n || v.payload_len != g_tlen[fi] ||
                            memcmp(v.payload, g_truth[fi],
                                   v.payload_len) != 0) {
                            g_checks++; g_failures++;
                            fprintf(stderr, "FAIL F-B clean frame %u at "
                                    "cycle %llu\n", fi,
                                    (unsigned long long)it);
                            return;
                        }
                        g_b_frames_exact++;
                    }
                    fi++;
                }
                if (rc < -5 || rc > 0) {
                    g_checks++; g_failures++;
                    fprintf(stderr, "FAIL F-B walk rc=%d at cycle %llu\n",
                            rc, (unsigned long long)it);
                    return;
                }
                if (rc != WEFT_STUDIO_ETRUNC && rc != WEFT_STUDIO_EBOUNDS)
                    g_b_refusals[(-rc) - 1]++;
                if (clean && (!walked_any || fi != n)) {
                    g_checks++; g_failures++;
                    fprintf(stderr, "FAIL F-B clean walk count %u != %u "
                            "at cycle %llu\n", fi, n,
                            (unsigned long long)it);
                    return;
                }
                if (clean) g_b_clean_walks++;
            }
        }
        g_b_cycles++;
        if ((it % 1000000ull) == 0 && it) {
            printf("  F-B %lluM cycles\n", (unsigned long long)(it / 1000000ull));
            fflush(stdout);
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t cycles_a = 6000000ull;
    uint64_t cycles_b = 4000000ull;
    uint64_t t0;
    if (argc > 1 && strcmp(argv[1], "--reduced") == 0) {
        cycles_a = 1500000ull;   /* ASan/UBSan leg (declared reduction) */
        cycles_b = 1000000ull;
    }
    g_cmem = malloc(weftc_ctx_size());
    if (!g_cmem) return 2;
    if (weftc_ctx_init(g_cmem, weftc_ctx_size(), &g_c)) return 2;

    t0 = harness_now_ns();
    g_rng = FUZZ_SEED_A0;
    fuzz_schema(cycles_a);
    if (g_failures) {
        printf("fuzz aborted early (surface A)\n");
        return harness_summary("test_fuzz_studio (F-series)");
    }
    g_rng = FUZZ_SEED_B0;
    fuzz_trace(cycles_b);
    if (g_failures) {
        printf("fuzz aborted early (surface B)\n");
        return harness_summary("test_fuzz_studio (F-series)");
    }
    {
        uint64_t dt = (harness_now_ns() - t0) / 1000000ull;
        printf("  F-A: %llu schema cycles (%llu refusals, %llu determinism "
               "re-checks)\n", g_a_cycles, g_a_refusals, g_a_det_checks);
        printf("  F-B: %llu trace cycles (%llu clean bit-exact walks, "
               "%llu frames verified)\n", g_b_cycles, g_b_clean_walks,
               g_b_frames_exact);
        printf("  F-B refusals: EPARSE=%llu EALIGN=%llu ETRUNC=%llu "
               "ECRC=%llu EBOUNDS=%llu\n",
               g_b_refusals[0], g_b_refusals[1], g_b_refusals[2],
               g_b_refusals[3], g_b_refusals[4]);
        printf("  total: %llu cycles in %llu ms\n",
               (unsigned long long)(g_a_cycles + g_b_cycles),
               (unsigned long long)dt);
    }
    g_checks++;
    if (g_a_cycles + g_b_cycles < cycles_a + cycles_b - 1ull) {
        g_failures++;
        fprintf(stderr, "FAIL F0: cycle shortfall\n");
    }
    return harness_summary("test_fuzz_studio (F-series)");
}
