/* ---------------------------------------------------------------------------
 * weftc_lint_alloc.h — Weft Pillar 8: `weftc --lint-alloc` engine ABI
 *
 * TERRITORY: tools/weftc/lint/ (Pillar 8 directive, Engineer 1).
 * ROLE:      Static-analysis engine that proves Law 1 (zero heap allocation
 *            on the hot plane) at COMPILE TIME. It parses C, C++, Rust,
 *            TypeScript, Swift and Dart sources into a flat token-level AST
 *            (arena-allocated at SETUP time), then runs a ZERO-ALLOCATION
 *            scan pass that flags, inside functions tagged @hot /
 *            [[clang::annotate("weft_hot")]] / __attribute__((weft_hot)) /
 *            #[weft_hot] / @WeftHot:
 *
 *              - malloc / calloc / realloc / free / strdup / strndup /
 *                asprintf / vasprintf / posix_memalign / aligned_alloc /
 *                memalign / valloc / pvalloc (rule "alloc-heap",
 *                "alloc-string", "alloc-fmt")
 *              - mmap / mmap64 / sbrk (rule "alloc-mmap")
 *              - new / delete, incl. TS `new X()` / `delete x`
 *                (rule "alloc-new")
 *              - dynamic string concatenation: `+`/`+=` against string
 *                literals, template `${...}`, Dart `$var` interpolation,
 *                Rust `format!`/`String::from` (rule "alloc-concat"/
 *                "alloc-fmt")
 *              - unbounded recursion: self-recursion and mutually
 *                recursive cycles touching a @hot function
 *                (rule "alloc-recursion")
 *              - Rust std allocation idioms (Box::new, vec!, .to_string(),
 *                .clone(), Vec::new ...) — WARNING severity
 *                (rule "alloc-rust-std")
 *
 * PERFORMANCE SLA (mandate §2.B): the SCAN pass processes > 100,000 AST
 * nodes in < 15 ms. The scan is single-pass over the token arena with
 * static const rule tables; it performs ZERO heap allocations (Law 1,
 * proven by the G4 --wrap probe on the shipped test binaries).
 *
 * ABI: frozen at version 1.0. struct layouts are pinned with static asserts
 * here and mirrored from C++17 by tests/verify/core/test_abi_cpp17.cpp.
 * ------------------------------------------------------------------------- */
#ifndef WEFTC_LINT_ALLOC_H
#define WEFTC_LINT_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFTC_LINT_ABI_MAJOR 1u
#define WEFTC_LINT_ABI_MINOR 0u
#define WEFTC_LINT_ABI_VERSION ((WEFTC_LINT_ABI_MAJOR << 8u) | WEFTC_LINT_ABI_MINOR)

/* ------------------------------------------------------------------ */
/* Enums (frozen numeric values)                                       */
/* ------------------------------------------------------------------ */

typedef enum weft_lint_severity {
    WEFT_LINT_NOTE    = 0,
    WEFT_LINT_WARNING = 1,
    WEFT_LINT_ERROR   = 2
} weft_lint_severity_t;

typedef enum weft_lint_lang {
    WEFT_LINT_LANG_AUTO       = 0,
    WEFT_LINT_LANG_C          = 1,
    WEFT_LINT_LANG_CPP        = 2,
    WEFT_LINT_LANG_RUST       = 3,
    WEFT_LINT_LANG_TYPESCRIPT = 4,
    WEFT_LINT_LANG_SWIFT      = 5,
    WEFT_LINT_LANG_DART       = 6
} weft_lint_lang_t;

/* Token/AST node kinds. */
enum {
    WEFT_LINT_TK_EOF     = 0,
    WEFT_LINT_TK_IDENT   = 1,
    WEFT_LINT_TK_NUMBER  = 2,
    WEFT_LINT_TK_STRING  = 3,  /* extra bit0: interpolation; bit1: backtick */
    WEFT_LINT_TK_CHAR    = 4,
    WEFT_LINT_TK_PUNCT   = 5,
    WEFT_LINT_TK_COMMENT = 6,  /* extra bit0: contains @hot marker */
    WEFT_LINT_TK_PREPROC = 7
};

/* Node extra flags. */
#define WEFT_LINT_X_INTERP     0x01u  /* string carries interpolation   */
#define WEFT_LINT_X_BACKTICK   0x02u  /* template literal (TS)          */
#define WEFT_LINT_X_HOTATTR    0x04u  /* token begins a hot annotation  */
#define WEFT_LINT_X_HOTCOMMENT 0x08u  /* comment mentions @hot          */

/* ------------------------------------------------------------------ */
/* Frozen structs (layouts pinned; mirrored by the C++17 ABI test)     */
/* ------------------------------------------------------------------ */

typedef struct weft_lint_diag {
    char     file[64];         /* source file (truncated at 63)          */
    uint32_t line;             /* 1-based                                */
    uint32_t col;              /* 1-based                                */
    uint8_t  severity;         /* weft_lint_severity_t                   */
    char     rule[16];         /* e.g. "alloc-heap"                      */
    char     message[160];     /* human text, GCC-style, no prefix       */
    char     remediation[192]; /* remediation advice                     */
} weft_lint_diag_t;

typedef struct weft_lint_node {
    uint32_t kind;             /* WEFT_LINT_TK_*                         */
    uint32_t line;
    uint32_t col;
    uint32_t text_off;         /* offset into ast->text                  */
    uint32_t text_len;
    uint32_t extra;            /* kind-specific flags                    */
} weft_lint_node_t;

typedef struct weft_lint_func {
    uint32_t name_off;         /* offset into ast->text                  */
    uint32_t name_len;
    uint32_t line;
    uint32_t col;
    uint32_t body_first;       /* node index of '{' (or '=>')            */
    uint32_t body_last;        /* node index of '}' (or ';') inclusive   */
    uint32_t hot_attr_pos;     /* node idx of annotation, or UINT32_MAX  */
    uint8_t  is_hot;
    uint8_t  lang;             /* weft_lint_lang_t                       */
    uint16_t flags;
} weft_lint_func_t;

typedef struct weft_lint_ast {
    /* token arena */
    weft_lint_node_t *nodes;
    uint32_t          node_count;
    uint32_t          node_cap;
    /* interning text arena */
    char             *text;
    uint32_t          text_len;
    uint32_t          text_cap;
    /* detected functions */
    weft_lint_func_t *funcs;
    uint32_t          func_count;
    uint32_t          func_cap;
    /* call-graph edges (caller,callee) pairs, grouped by caller in
     * function order; filled by the SCAN pass, capacity reserved at parse */
    uint32_t         *edges;
    uint32_t          edge_count;
    uint32_t          edge_cap;
    uint32_t         *edge_start;     /* func_count+1 offsets, scan-filled */
    /* name lookup: open-addressed table hash -> func_idx+1 (0 = empty) */
    uint32_t         *name_table;
    uint32_t          name_table_mask;
    /* scratch for the scan pass (Tarjan SCC etc.), zero-alloc at scan */
    uint32_t         *scan_scratch;
    uint32_t          scan_scratch_cap;
    /* metadata */
    char              file[64];
    uint32_t          source_bytes;
    uint32_t          parse_flags;    /* reserved, 0 */
} weft_lint_ast_t;

/* Arena allocator with realloc() semantics: (NULL, n) -> malloc,
 * (p, 0) -> free, (p, n) -> realloc. The ENGINE never allocates outside
 * this hook, and the SCAN pass never calls it at all (Law 1). */
typedef void *(*weft_lint_alloc_fn)(void *ctx, void *ptr, size_t size);

/* ------------------------------------------------------------------ */
/* Engine API                                                          */
/* ------------------------------------------------------------------ */

/* Parse `source` into an AST. Returns 0 on success, negative on error
 * (bad arguments / arena exhaustion). Allocation happens ONLY here. */
int weftc_lint_parse(const char *source, size_t source_len,
                     const char *filename, weft_lint_lang_t lang,
                     weft_lint_alloc_fn alloc, void *alloc_ctx,
                     weft_lint_ast_t **out_ast);

/* Release an AST through the same allocator hook. */
void weftc_lint_free_ast(weft_lint_ast_t *ast, weft_lint_alloc_fn alloc,
                         void *alloc_ctx);

/* Scan a parsed AST and emit diagnostics into `diags` (caller-owned).
 * ZERO heap allocations. The token/function/name-table arenas are treated
 * as read-only; only the preallocated call-edge array, edge offsets and
 * Tarjan scratch (all reserved at parse time) are written.
 * Returns:
 *    0  scan complete, all diagnostics written
 *    1  scan complete but the diagnostic buffer was too small
 *   -1  invalid arguments
 *   -2  internal capacity exhausted (fail-closed: caller must re-parse
 *        with a larger arena)
 * *out_written = diagnostics stored; *out_total = diagnostics found. */
int weftc_lint_scan_ast(weft_lint_ast_t *ast,
                        weft_lint_diag_t *diags, uint32_t max_diags,
                        uint32_t *out_written, uint32_t *out_total);

/* Default allocator (stdlib realloc/free) for CLI convenience. */
void *weft_lint_default_alloc(void *ctx, void *ptr, size_t size);

/* SLA benchmark: synthesizes a deterministic AST of >= target_nodes nodes
 * (setup allocations allowed), then times `rounds` scan passes.
 * Returns 0 on success; fills the measured values. */
int weftc_lint_benchmark(uint32_t target_nodes, uint32_t rounds,
                         double *out_best_ms, double *out_avg_ms,
                         uint32_t *out_nodes, uint32_t *out_diags);

/* Engine identity. */
const char *weftc_lint_version(void);
uint32_t    weftc_lint_abi_version(void);

/* ------------------------------------------------------------------ */
/* Frozen layout proofs                                                */
/* ------------------------------------------------------------------ */

#if defined(__cplusplus)
#  if __cplusplus >= 201103L
#    define WEFTC_LINT_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#  else
#    error "weftc_lint_alloc.h requires C++11 or newer"
#  endif
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define WEFTC_LINT_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
#  define WEFTC_LINT_SA_GLUE_(a, b) a##b
#  define WEFTC_LINT_SA_GLUE(a, b)  WEFTC_LINT_SA_GLUE_(a, b)
#  define WEFTC_LINT_STATIC_ASSERT(cond, msg) \
      typedef char WEFTC_LINT_SA_GLUE(weft_lint_sa_line_, __LINE__)[(cond) ? 1 : -1]
#endif

WEFTC_LINT_STATIC_ASSERT(sizeof(weft_lint_node_t) == 24,
                         "frozen ABI: weft_lint_node_t must be 24 bytes");
WEFTC_LINT_STATIC_ASSERT(sizeof(weft_lint_diag_t) == 444,
                         "frozen ABI: weft_lint_diag_t must be 444 bytes");
WEFTC_LINT_STATIC_ASSERT(sizeof(weft_lint_func_t) == 32,
                         "frozen ABI: weft_lint_func_t must be 32 bytes");

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFTC_LINT_ALLOC_H */
