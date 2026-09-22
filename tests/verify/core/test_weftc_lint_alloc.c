/* ---------------------------------------------------------------------------
 * test_weftc_lint_alloc.c — Weft Pillar 8: linter precision / recall suite
 *
 * GATE COVERAGE: G3 (mandate §2.D.2): "Positive and negative test cases
 * verifying that weftc --lint-alloc catches all illicit allocations and
 * passes clean hot-paths" — plus the §2.B SLA (> 100,000 AST nodes in
 * < 15 ms) and the Law 1 zero-allocation probe on the SCAN pass.
 *
 * LAYERS:
 *   1. Corpus: poisoned + clean sources across C, C++, Rust, TypeScript,
 *      Swift, Dart. Every planted violation MUST be found (100% recall);
 *      every clean case MUST produce zero error-severity diagnostics
 *      (0 false positives at error level). Expected rule counts are
 *      asserted exactly.
 *   2. Determinism: two scans of the same AST must produce byte-identical
 *      diagnostic sequences.
 *   3. Law 1 probe: parse (setup, allocation allowed), then the SCAN pass
 *      must perform ZERO heap calls (binary linked with -Wl,--wrap=...).
 *   4. SLA benchmark: weftc_lint_benchmark(150000, 5) — best scan must be
 *      under 15 ms for > 100,000 nodes.
 * ------------------------------------------------------------------------- */

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "weftc_lint_alloc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* malloc interposition probe (Law 1)                                  */
/* ------------------------------------------------------------------ */

#ifdef WEFT_VERIFY_WRAP_PROBE
extern void *__real_malloc(size_t n);
extern void *__real_calloc(size_t n, size_t m);
extern void *__real_realloc(void *p, size_t n);
extern void *__real_free(void *p);
extern char *__real_strdup(const char *s);

static unsigned long long g_probe_calls = 0u;

void *__wrap_malloc(size_t n)
{
    g_probe_calls++;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t m)
{
    g_probe_calls++;
    return __real_calloc(n, m);
}
void *__wrap_realloc(void *p, size_t n)
{
    g_probe_calls++;
    return __real_realloc(p, n);
}
void __wrap_free(void *p)
{
    g_probe_calls++;
    __real_free(p);
}
char *__wrap_strdup(const char *s)
{
    g_probe_calls++;
    return __real_strdup(s);
}
static unsigned long long weft_probe_calls(void)
{
    return g_probe_calls;
}
#else
static unsigned long long weft_probe_calls(void)
{
    return 0u;
}
#endif

/* ------------------------------------------------------------------ */
/* corpus                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *rule;
    int         count;
} expect_ent_t;

typedef struct {
    const char      *name;
    weft_lint_lang_t lang;
    const char      *source;
    expect_ent_t     expect[8]; /* NULL rule terminates */
    int              expect_errors;
    int              expect_warnings;
} corpus_case_t;

#define SRC_C_POISON "/* @hot */\n"    "static int parse_frame(const char *src, int n) {\n" \
"    char *copy = strdup(src);\n" \
"    char *big = malloc(4096);\n" \
"    void *ali = aligned_alloc(64, 512);\n" \
"    void *pm; posix_memalign(&pm, 64, 128);\n" \
"    char *cat; asprintf(&cat, \"x%d\", n);\n" \
"    void *mm = mmap(NULL, 4096, 1, 1, -1, 0);\n" \
"    free(big); free(copy);\n" \
"    return 0;\n" \
"}\n"

#define SRC_CPP_ATTR "#include <cstdio>\n"    "[[clang::annotate(\"weft_hot\")]]\n" \
"int hot_cpp(int n) {\n" \
"    int *p = new int[64];\n" \
"    delete[] p;\n" \
"    return n;\n" \
"}\n"

#define SRC_ATTR_GCC "static __attribute__((weft_hot)) int attr_hot(int n) {\n"    "    char *p = malloc(4);\n" \
"    (void)p;\n" \
"    return n;\n" \
"}\n"

#define SRC_RUST_POISON "#[weft_hot]\n"    "fn hot_rust(x: u32) -> u32 {\n" \
"    let b = Box::new(x);\n" \
"    let s = format!(\"{}\", x);\n" \
"    let v = vec![1u32, 2];\n" \
"    let t = x.to_string();\n" \
"    let w: [u32; 4] = [x; 4];\n" \
"    b.as_ref().wrapping_add(v[0]) + w[0] + v[1]\n" \
"}\n" \
"fn cold_rust(x: u32) -> u32 { let v = vec![x]; v[0] }\n"

#define SRC_RUST_LEAK "#[weft_hot]\n"    "fn leaky(x: u32) -> &'static u32 {\n" \
"    let b = Box::new(x);\n" \
"    Box::leak(b)\n" \
"}\n"

#define SRC_TS_POISON "@hot\n"    "function hotTs(a: number, b: string): string {\n" \
"    const dyn = new Array(16);\n" \
"    const s = \"prefix:\" + b;\n" \
"    const t = `val=${a}`;\n" \
"    return s;\n" \
"}\n"

#define SRC_SWIFT_POISON "@WeftHot func hotSwift(_ n: Int) -> Int {\n"    "    let s = \"count: \\(n)\"\n" \
"    let t = \"a\" + s\n" \
"    return n\n" \
"}\n"

#define SRC_DART_POISON "class Parser {\n"    "  @hot int hotParse(int x) {\n" \
"    var s = 'val=$x';\n" \
"    var u = \"pre\" + s;\n" \
"    return x;\n" \
"  }\n" \
"}\n"

#define SRC_RECURSION "/* @hot */ static int rec_a(int n) { return n ? rec_b(n-1) : 0; }\n"    "static int rec_b(int n) { return n ? rec_a(n-1) : 0; }\n" \
"/* @hot */ static int self_hot(int n) { return self_hot(n-1); }\n" \
"static int self_cold(int n) { return self_cold(n-1); }\n"

#define SRC_CLEAN_C "/* @hot */\n"    "static int clean_hot(int x) {\n" \
"    int buf[16];\n" \
"    buf[0] = x + 1;\n" \
"    return buf[0] * 3;\n" \
"}\n" \
"static int cold_ok(int x) {\n" \
"    char *p = malloc(8);\n" \
"    int r = p ? x : 0;\n" \
"    free(p);\n" \
"    return r;\n" \
"}\n"

#define SRC_CLEAN_FP_TRAPS "static int malloc_count(int x) { return x; }\n"    "static int news(int delete_me) {\n" \
"    return delete_me ? news(delete_me-1) : 0;\n" \
"}\n" \
"/* the word free appears, never as a call */\n" \
"static int freed_helpers(int x) { return malloc_count(x); }\n"

#define SRC_CLEAN_RUST "#[weft_hot]\n"    "fn clean_rust(x: u32) -> u32 {\n" \
"    let buf: [u32; 8] = [x; 8];\n" \
"    buf[0].wrapping_add(buf[1])\n" \
"}\n"

#define SRC_CLEAN_TS "const arrowCold = (x: number) => \"n\" + x;\n" \
"function pure(a: number) { return a * 2; }\n"

/* clang-format off */
static const corpus_case_t k_cases[] = {
    { "c-poisoned",   WEFT_LINT_LANG_C,    SRC_C_POISON,
      { {"alloc-string", 1}, {"alloc-heap", 5}, {"alloc-fmt", 1},
        {"alloc-mmap", 1}, {NULL, 0} }, 8, 0 },
    { "cpp-attrs",    WEFT_LINT_LANG_CPP,  SRC_CPP_ATTR,
      { {"alloc-new", 2}, {NULL, 0} }, 2, 0 },
    { "attr-gcc",     WEFT_LINT_LANG_C,    SRC_ATTR_GCC,
      { {"alloc-heap", 1}, {NULL, 0} }, 1, 0 },
    { "rust-poison",  WEFT_LINT_LANG_RUST, SRC_RUST_POISON,
      { {"alloc-heap", 2}, {"alloc-fmt", 1}, {"alloc-rust-std", 1},
        {NULL, 0} }, 3, 1 },
    { "rust-leak",    WEFT_LINT_LANG_RUST, SRC_RUST_LEAK,
      { {"alloc-heap", 1}, {NULL, 0} }, 1, 0 },
    { "ts-poison",    WEFT_LINT_LANG_TYPESCRIPT, SRC_TS_POISON,
      { {"alloc-new", 1}, {"alloc-concat", 2}, {NULL, 0} }, 3, 0 },
    { "swift-poison", WEFT_LINT_LANG_SWIFT, SRC_SWIFT_POISON,
      { {"alloc-concat", 2}, {NULL, 0} }, 2, 0 },
    { "dart-poison",  WEFT_LINT_LANG_DART, SRC_DART_POISON,
      { {"alloc-concat", 2}, {NULL, 0} }, 2, 0 },
    { "recursion",    WEFT_LINT_LANG_C,    SRC_RECURSION,
      { {"alloc-recursion", 2}, {NULL, 0} }, 2, 0 },
    { "clean-c",      WEFT_LINT_LANG_C,    SRC_CLEAN_C,
      { {NULL, 0} }, 0, 0 },
    { "clean-fp-traps", WEFT_LINT_LANG_C,  SRC_CLEAN_FP_TRAPS,
      { {NULL, 0} }, 0, 0 },
    { "clean-rust",   WEFT_LINT_LANG_RUST, SRC_CLEAN_RUST,
      { {NULL, 0} }, 0, 0 },
    { "clean-ts",     WEFT_LINT_LANG_TYPESCRIPT, SRC_CLEAN_TS,
      { {NULL, 0} }, 0, 0 },
};
/* clang-format on */

#define MAX_DIAGS 256

static int g_failures = 0;

static int count_rule(const weft_lint_diag_t *d, uint32_t n,
                      const char *rule, int sev)
{
    int c = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(d[i].rule, rule) == 0 && (int)d[i].severity == sev) {
            c++;
        }
    }
    return c;
}

static int total_sev(const weft_lint_diag_t *d, uint32_t n, int sev)
{
    int c = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((int)d[i].severity == sev) {
            c++;
        }
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* layer 1: corpus recall + precision                                  */
/* ------------------------------------------------------------------ */

static unsigned long long g_planted = 0u;
static unsigned long long g_found = 0u;

static void layer1_corpus(void)
{
    weft_lint_diag_t *d1 = malloc(sizeof(weft_lint_diag_t) * MAX_DIAGS);
    weft_lint_diag_t *d2 = malloc(sizeof(weft_lint_diag_t) * MAX_DIAGS);
    if (d1 == NULL || d2 == NULL) {
        fprintf(stderr, "FAIL: corpus diag buffer alloc\n");
        g_failures++;
        free(d1);
        free(d2);
        return;
    }

    const size_t nc = sizeof(k_cases) / sizeof(k_cases[0]);
    for (size_t ci = 0; ci < nc; ci++) {
        const corpus_case_t *cs = &k_cases[ci];
        weft_lint_ast_t *ast = NULL;
        int prc = weftc_lint_parse(cs->source, strlen(cs->source), cs->name,
                                   cs->lang, weft_lint_default_alloc, NULL,
                                   &ast);
        if (prc != 0 || ast == NULL) {
            fprintf(stderr, "FAIL[%s]: parse rc=%d\n", cs->name, prc);
            g_failures++;
            continue;
        }
        uint32_t w1 = 0u, t1 = 0u, w2 = 0u, t2 = 0u;
        int rc1 = weftc_lint_scan_ast(ast, d1, MAX_DIAGS, &w1, &t1);
        int rc2 = weftc_lint_scan_ast(ast, d2, MAX_DIAGS, &w2, &t2);

        if (rc1 != 0 || rc2 != 0) {
            fprintf(stderr, "FAIL[%s]: scan rc=%d/%d\n", cs->name, rc1, rc2);
            g_failures++;
        }
        if (w1 != w2 || t1 != t2 ||
            memcmp(d1, d2, sizeof(weft_lint_diag_t) * w1) != 0) {
            fprintf(stderr, "FAIL[%s]: non-deterministic scan\n", cs->name);
            g_failures++;
        }

        int errs = total_sev(d1, w1, WEFT_LINT_ERROR);
        int warns = total_sev(d1, w1, WEFT_LINT_WARNING);
        if (errs != cs->expect_errors || warns != cs->expect_warnings) {
            fprintf(stderr,
                    "FAIL[%s]: errors %d (want %d), warnings %d (want %d)\n",
                    cs->name, errs, cs->expect_errors, warns,
                    cs->expect_warnings);
            g_failures++;
        }
        for (int e = 0; cs->expect[e].rule != NULL; e++) {
            int got = count_rule(d1, w1, cs->expect[e].rule,
                                 (cs->expect[e].count > 0 &&
                                  strcmp(cs->expect[e].rule, "alloc-rust-std")
                                      == 0)
                                     ? WEFT_LINT_WARNING
                                     : WEFT_LINT_ERROR);
            if (got != cs->expect[e].count) {
                fprintf(stderr,
                        "FAIL[%s]: rule %s count %d (want %d)\n",
                        cs->name, cs->expect[e].rule, got,
                        cs->expect[e].count);
                g_failures++;
            }
        }
        g_planted += (unsigned long long)(cs->expect_errors +
                                          cs->expect_warnings);
        g_found += (unsigned long long)(errs + warns);

        printf("  [%-14s] nodes=%-6u funcs=%-3u hot=%d diags=%u/%u "
               "(E=%d W=%d) %s\n",
               cs->name, (unsigned)ast->node_count,
               (unsigned)ast->func_count,
               (int)k_cases[ci].expect_errors > 0 ? 1 : 0, (unsigned)w1,
               (unsigned)t1, errs, warns,
               (errs == cs->expect_errors && warns == cs->expect_warnings)
                   ? "ok"
                   : "MISMATCH");
        weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
    }

    free(d1);
    free(d2);

    if (g_planted != g_found) {
        fprintf(stderr,
                "FAIL: recall %llu/%llu (100%% required by G3)\n", g_found,
                g_planted);
        g_failures++;
    }
    printf("L1 corpus: %zu cases, planted=%llu found=%llu "
           "(recall %.2f%%)\n",
           nc, g_planted, g_found,
           g_planted ? (100.0 * (double)g_found / (double)g_planted) : 100.0);
}

/* ------------------------------------------------------------------ */
/* layer 3: Law 1 zero-allocation probe on the SCAN pass               */
/* ------------------------------------------------------------------ */

static void layer3_scan_zero_alloc(void)
{
    weft_lint_diag_t *d = malloc(sizeof(weft_lint_diag_t) * MAX_DIAGS);
    weft_lint_ast_t *ast = NULL;
    if (d == NULL) {
        fprintf(stderr, "FAIL: probe diag buffer\n");
        g_failures++;
        return;
    }
    int prc = weftc_lint_parse(SRC_C_POISON, strlen(SRC_C_POISON),
                               "probe", WEFT_LINT_LANG_C,
                               weft_lint_default_alloc, NULL, &ast);
    if (prc != 0 || ast == NULL) {
        fprintf(stderr, "FAIL: probe parse\n");
        g_failures++;
        free(d);
        return;
    }
    unsigned long long p0 = weft_probe_calls();
    uint32_t w = 0u, t = 0u;
    for (int i = 0; i < 100; i++) {
        (void)weftc_lint_scan_ast(ast, d, MAX_DIAGS, &w, &t);
    }
    unsigned long long p1 = weft_probe_calls();
    printf("L3 Law1: 100 scans x %u nodes, heap calls during scan: %llu\n",
           (unsigned)ast->node_count, p1 - p0);
    if (p1 != p0) {
        fprintf(stderr,
                "FAIL(Law1): scan pass performed %llu heap call(s)\n",
                p1 - p0);
        g_failures++;
    }
    weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
    free(d);
}

/* ------------------------------------------------------------------ */
/* layer 4: SLA benchmark                                              */
/* ------------------------------------------------------------------ */

static void layer4_sla(int reduced)
{
    uint32_t target = reduced ? 20000u : 150000u;
    double best = 0.0, avg = 0.0;
    uint32_t nodes = 0u, diags = 0u;
    int rc = weftc_lint_benchmark(target, 5u, &best, &avg, &nodes, &diags);
    if (rc != 0) {
        fprintf(stderr, "FAIL: benchmark rc=%d\n", rc);
        g_failures++;
        return;
    }
    printf("L4 SLA: nodes=%u (>100k? %s) best=%.3f ms avg=%.3f ms "
           "(%.1f ns/node) diags=%u — limit 15 ms: %s\n",
           (unsigned)nodes, nodes >= 100000u ? "yes" : "no (san mode)",
           best, avg, (nodes > 0u) ? (best * 1e6 / (double)nodes) : 0.0,
           (unsigned)diags, best < 15.0 ? "PASS" : "FAIL");
    if (!reduced) {
        if (nodes < 100000u) {
            fprintf(stderr, "FAIL: benchmark nodes %u < 100000\n",
                    (unsigned)nodes);
            g_failures++;
        }
        if (best >= 15.0) {
            fprintf(stderr, "FAIL: benchmark best %.3f ms >= 15 ms\n", best);
            g_failures++;
        }
    }
}

int main(int argc, char **argv)
{
    int reduced = (argc > 1 && strcmp(argv[1], "--san") == 0);
    static char obuf[1 << 16];
    setvbuf(stdout, obuf, _IOFBF, sizeof(obuf));

    printf("weftc --lint-alloc corpus suite — %s\n",
           reduced ? "sanitizer budget" : "full");
    printf("engine: %s (ABI 0x%04x)\n", weftc_lint_version(),
           (unsigned)weftc_lint_abi_version());

    layer1_corpus();
    layer3_scan_zero_alloc();
    layer4_sla(reduced);

    printf("LINT-ALLOC %s (%d failures; recall %llu/%llu)\n",
           (g_failures == 0) ? "PASS" : "FAIL", g_failures, g_found,
           g_planted);
    fflush(stdout);
    return (g_failures == 0) ? 0 : 1;
}
