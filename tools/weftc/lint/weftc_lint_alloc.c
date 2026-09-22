/* ---------------------------------------------------------------------------
 * weftc_lint_alloc.c — Weft Pillar 8: `weftc --lint-alloc` engine
 *
 * TERRITORY: tools/weftc/lint/ (Pillar 8 directive, Engineer 1).
 *
 * Architecture (two phases, one Law):
 *
 *   Phase 1  PARSE (setup, allocation allowed through the caller's arena
 *            hook): tokenizer -> token arena (flat weft_lint_node_t array,
 *            text interned as a single copy of the source) -> hot-attribute
 *            marking -> function table -> name lookup table -> capacity
 *            reservation for the scan pass (edges + Tarjan scratch).
 *
 *   Phase 2  SCAN (weftc_lint_scan_ast — ZERO heap allocations, Law 1):
 *            one linear walk over each function body applying the rule
 *            tables, call-edge collection for recursion analysis, then an
 *            iterative Tarjan SCC pass over the intra-file call graph.
 *            Diagnostics are written into the caller's buffer; messages are
 *            assembled with bounded copies only.
 *
 * Languages: C, C++, Rust, TypeScript, Swift, Dart (auto-sniffed or forced
 * with --lang). Every heuristic and its precision/recall ledger is documented
 * in docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md §5.
 * ------------------------------------------------------------------------- */

/* feature macros FIRST: clock_gettime (POSIX.1-2008) for the SLA bench */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "weftc_lint_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WEFT_LINT_UNREACHABLE_UINT32 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static uint32_t weft_lint_next_pow2_u32(uint32_t v)
{
    uint32_t p = 1u;
    while (p < v && p != 0u) {
        p <<= 1u;
    }
    return (p == 0u) ? 0xFFFFFFFFu : p;
}

static uint64_t weft_lint_fnv1a_64(const char *s, uint32_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Grow an arena through the caller's allocator. Returns 0 on success. */
static int weft_lint_grow(weft_lint_alloc_fn alloc, void *actx, void **ptr,
                          uint32_t *cap, size_t elem_size, uint32_t need)
{
    if (need < *cap) {
        return 0;
    }
    uint32_t ncap = (*cap == 0u) ? 64u : *cap;
    while (ncap < need && ncap != 0xFFFFFFFFu) {
        ncap *= 2u;
    }
    if (ncap == 0xFFFFFFFFu && need < 0xFFFFFFFFu) {
        ncap = need;
    }
    void *np = alloc(actx, *ptr, (size_t)ncap * elem_size);
    if (np == NULL && ncap != 0u) {
        return -1;
    }
    *ptr = np;
    *cap = ncap;
    return 0;
}

void *weft_lint_default_alloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    if (size == 0u) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

/* ------------------------------------------------------------------ */
/* Language sniffing (AUTO only)                                       */
/* ------------------------------------------------------------------ */

static int weft_lint_contains(const char *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    if (m == 0u || n < m) {
        return 0;
    }
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) {
            return 1;
        }
    }
    return 0;
}

static weft_lint_lang_t weft_lint_sniff(const char *s, size_t n)
{
    if (weft_lint_contains(s, n, "fn ") &&
        (weft_lint_contains(s, n, "pub ") ||
         weft_lint_contains(s, n, "let ") ||
         weft_lint_contains(s, n, "impl "))) {
        return WEFT_LINT_LANG_RUST;
    }
    if (weft_lint_contains(s, n, "function ") ||
        (weft_lint_contains(s, n, "=>") &&
         (weft_lint_contains(s, n, "const ") ||
          weft_lint_contains(s, n, "interface ")))) {
        return WEFT_LINT_LANG_TYPESCRIPT;
    }
    if (weft_lint_contains(s, n, "func ")) {
        if (weft_lint_contains(s, n, "package:") ||
            weft_lint_contains(s, n, "List<") ||
            weft_lint_contains(s, n, "async ")) {
            return WEFT_LINT_LANG_DART;
        }
        return WEFT_LINT_LANG_SWIFT;
    }
    if (weft_lint_contains(s, n, "::") ||
        weft_lint_contains(s, n, "template<") ||
        weft_lint_contains(s, n, "public:")) {
        return WEFT_LINT_LANG_CPP;
    }
    return WEFT_LINT_LANG_C;
}

static int weft_lint_lang_is_managed(weft_lint_lang_t l)
{
    return (l == WEFT_LINT_LANG_TYPESCRIPT || l == WEFT_LINT_LANG_SWIFT ||
            l == WEFT_LINT_LANG_DART);
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char          *src;
    size_t               n;
    size_t               pos;
    uint32_t             line;
    uint32_t             col;
    weft_lint_ast_t     *ast;
    weft_lint_alloc_fn   alloc;
    void                *actx;
} weft_tok_ctx_t;

static int weft_tok_push(weft_tok_ctx_t *t, uint32_t kind, size_t off,
                         uint32_t len, uint32_t extra)
{
    if (weft_lint_grow(t->alloc, t->actx, (void **)&t->ast->nodes,
                       &t->ast->node_cap, sizeof(weft_lint_node_t),
                       t->ast->node_count + 1u) != 0) {
        return -1;
    }
    weft_lint_node_t *nd = &t->ast->nodes[t->ast->node_count];
    nd->kind = kind;
    nd->line = t->line;
    nd->col = t->col;
    nd->text_off = (uint32_t)off;
    nd->text_len = len;
    nd->extra = extra;
    t->ast->node_count++;
    return 0;
}

static int weft_is_ident_start(int c, weft_lint_lang_t lang)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        return 1;
    }
    if (c == '$' && lang == WEFT_LINT_LANG_TYPESCRIPT) {
        return 1;
    }
    return 0;
}

static int weft_is_ident_char(int c, weft_lint_lang_t lang)
{
    return weft_is_ident_start(c, lang) || (c >= '0' && c <= '9');
}

/* Scan a quoted region starting at s[begin] (the quote). Returns end index
 * of the closing quote via *out_end, interpolation flag via *out_interp. */
static size_t weft_scan_quoted(const char *s, size_t n, size_t begin,
                               char quote, weft_lint_lang_t lang,
                               int backtick, int *out_interp)
{
    size_t i = begin + 1u;
    int interp = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && !backtick) {
            /* Swift string interpolation "\(expr)" is an escape we must
             * still observe before skipping the escape pair */
            if (lang == WEFT_LINT_LANG_SWIFT && i + 1u < n &&
                s[i + 1u] == '(') {
                interp = 1;
            }
            i += 2u;
            continue;
        }
        if (c == '\\' && backtick) {
            i += 2u;
            continue;
        }
        if (c == quote) {
            i++;
            break;
        }
        /* interpolation detectors */
        if (backtick && c == '$' && i + 1u < n && s[i + 1u] == '{') {
            interp = 1;
        }
        if ((lang == WEFT_LINT_LANG_DART) && c == '$' &&
            i + 1u < n &&
            ((s[i + 1u] >= 'a' && s[i + 1u] <= 'z') ||
             (s[i + 1u] >= 'A' && s[i + 1u] <= 'Z') || s[i + 1u] == '_' ||
             s[i + 1u] == '{')) {
            interp = 1;
        }
        i++;
    }
    if (i > n) {
        i = n;
    }
    *out_interp = interp;
    return i;
}

static int weft_tok_run(weft_tok_ctx_t *t, weft_lint_lang_t lang)
{
    const char *s = t->src;
    const size_t n = t->n;
    while (t->pos < n) {
        char c = s[t->pos];
        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') {
            t->pos++;
            t->col++;
            continue;
        }
        if (c == '\n') {
            t->pos++;
            t->line++;
            t->col = 1u;
            continue;
        }
        /* comments */
        if (c == '/' && t->pos + 1u < n && s[t->pos + 1u] == '/') {
            size_t off = t->pos;
            while (t->pos < n && s[t->pos] != '\n') {
                t->pos++;
            }
            uint32_t len = (uint32_t)(t->pos - off);
            int hot = weft_lint_contains(s + off, len, "@hot") ||
                      weft_lint_contains(s + off, len, "weft_hot") ||
                      weft_lint_contains(s + off, len, "@WeftHot");
            if (weft_tok_push(t, WEFT_LINT_TK_COMMENT, off, len,
                              hot ? WEFT_LINT_X_HOTCOMMENT : 0u) != 0) {
                return -1;
            }
            t->col += len;
            continue;
        }
        if (c == '/' && t->pos + 1u < n && s[t->pos + 1u] == '*') {
            size_t off = t->pos;
            t->pos += 2u;
            while (t->pos + 1u < n &&
                   !(s[t->pos] == '*' && s[t->pos + 1u] == '/')) {
                if (s[t->pos] == '\n') {
                    t->line++;
                    t->col = 1u;
                } else {
                    t->col++;
                }
                t->pos++;
            }
            if (t->pos + 1u < n) {
                t->pos += 2u; /* skip closing */
            } else {
                t->pos = n;
            }
            uint32_t len = (uint32_t)(t->pos - off);
            int hot = weft_lint_contains(s + off, len, "@hot") ||
                      weft_lint_contains(s + off, len, "weft_hot") ||
                      weft_lint_contains(s + off, len, "@WeftHot");
            if (weft_tok_push(t, WEFT_LINT_TK_COMMENT, off, len,
                              hot ? WEFT_LINT_X_HOTCOMMENT : 0u) != 0) {
                return -1;
            }
            t->col += len;
            continue;
        }
        /* preprocessor line (C family) or Rust attribute opener */
        if (c == '#') {
            if (t->pos + 1u < n && s[t->pos + 1u] == '[') {
                /* '#' then '[' — Rust attribute; emit '#' as punct */
                if (weft_tok_push(t, WEFT_LINT_TK_PUNCT, t->pos, 1u, 0u) != 0) {
                    return -1;
                }
                t->pos++;
                t->col++;
                continue;
            }
            size_t off = t->pos;
            while (t->pos < n) {
                if (s[t->pos] == '\\' && t->pos + 1u < n &&
                    s[t->pos + 1u] == '\n') {
                    t->pos += 2u;
                    t->line++;
                    t->col = 1u;
                    continue;
                }
                if (s[t->pos] == '\n') {
                    break;
                }
                t->pos++;
                t->col++;
            }
            uint32_t len = (uint32_t)(t->pos - off);
            if (weft_tok_push(t, WEFT_LINT_TK_PREPROC, off, len, 0u) != 0) {
                return -1;
            }
            continue;
        }
        /* strings / chars (Rust lifetimes like 'a / 'static are handled
         * first: quote immediately followed by an identifier start) */
        if (c == '\'' && lang == WEFT_LINT_LANG_RUST && t->pos + 1u < n &&
            weft_is_ident_start((int)(unsigned char)s[t->pos + 1u], lang) &&
            !(t->pos + 2u < n && s[t->pos + 2u] == '\'')) {
            size_t off = t->pos;
            t->pos++;
            while (t->pos < n &&
                   weft_is_ident_char((int)(unsigned char)s[t->pos], lang)) {
                t->pos++;
            }
            uint32_t llen = (uint32_t)(t->pos - off);
            if (weft_tok_push(t, WEFT_LINT_TK_CHAR, off, llen, 0u) != 0) {
                return -1;
            }
            t->col += llen;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            int interp = 0;
            int backtick = (c == '`');
            size_t off = t->pos;
            size_t end = weft_scan_quoted(s, n, t->pos, c, lang, backtick,
                                          &interp);
            uint32_t len = (uint32_t)(end - off);
            uint32_t kind = (c == '"') ? WEFT_LINT_TK_STRING : WEFT_LINT_TK_CHAR;
            uint32_t extra = 0u;
            if (backtick) {
                kind = WEFT_LINT_TK_STRING;
                extra |= WEFT_LINT_X_BACKTICK;
            }
            if (interp) {
                extra |= WEFT_LINT_X_INTERP;
            }
            if (weft_tok_push(t, kind, off, len, extra) != 0) {
                return -1;
            }
            /* advance line/col over the literal */
            for (size_t k = off; k < end; k++) {
                if (s[k] == '\n') {
                    t->line++;
                    t->col = 1u;
                } else {
                    t->col++;
                }
            }
            t->pos = end;
            continue;
        }
        /* numbers */
        if (c >= '0' && c <= '9') {
            size_t off = t->pos;
            while (t->pos < n) {
                char d = s[t->pos];
                if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') ||
                    (d >= 'A' && d <= 'F') || d == '_') {
                    t->pos++;
                    continue;
                }
                if (d == '.' && t->pos + 1u < n && s[t->pos + 1u] != '.') {
                    t->pos++;
                    continue;
                }
                if ((d == '+' || d == '-') && t->pos > off &&
                    (s[t->pos - 1u] == 'e' || s[t->pos - 1u] == 'E')) {
                    t->pos++;
                    continue;
                }
                break;
            }
            uint32_t len = (uint32_t)(t->pos - off);
            if (weft_tok_push(t, WEFT_LINT_TK_NUMBER, off, len, 0u) != 0) {
                return -1;
            }
            t->col += len;
            continue;
        }
        /* identifiers */
        if (weft_is_ident_start((int)(unsigned char)c, lang)) {
            size_t off = t->pos;
            while (t->pos < n &&
                   weft_is_ident_char((int)(unsigned char)s[t->pos], lang)) {
                t->pos++;
            }
            uint32_t len = (uint32_t)(t->pos - off);
            if (weft_tok_push(t, WEFT_LINT_TK_IDENT, off, len, 0u) != 0) {
                return -1;
            }
            t->col += len;
            continue;
        }
        /* multi-char punctuators */
        {
            static const char *const multi[] = {
                "::", "->", "=>", "==", "!=", "<=", ">=", "&&", "||",
                "++", "--", "+=", "-=", "*=", "/=", "%=", "<<", ">>",
                "<<=", ">>=", "|=", "&=", "^=", "?.", "??", "...", NULL
            };
            uint32_t matched = 0;
            for (int mi = 0; multi[mi] != NULL; mi++) {
                size_t ml = strlen(multi[mi]);
                if (t->pos + ml <= n &&
                    memcmp(s + t->pos, multi[mi], ml) == 0) {
                    if (weft_tok_push(t, WEFT_LINT_TK_PUNCT, t->pos,
                                      (uint32_t)ml, 0u) != 0) {
                        return -1;
                    }
                    t->pos += ml;
                    t->col += (uint32_t)ml;
                    matched = 1;
                    break;
                }
            }
            if (matched) {
                continue;
            }
        }
        /* single-char punctuator */
        if (weft_tok_push(t, WEFT_LINT_TK_PUNCT, t->pos, 1u, 0u) != 0) {
            return -1;
        }
        t->pos++;
        t->col++;
    }
    /* EOF sentinel */
    if (weft_tok_push(t, WEFT_LINT_TK_EOF, (uint32_t)(n > 0u ? n - 1u : 0u),
                      0u, 0u) != 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Token helpers                                                       */
/* ------------------------------------------------------------------ */

static int weft_tok_text_eq(const weft_lint_ast_t *a, uint32_t i,
                            const char *lit)
{
    size_t m = strlen(lit);
    if (a->nodes[i].text_len != (uint32_t)m) {
        return 0;
    }
    return memcmp(a->text + a->nodes[i].text_off, lit, m) == 0;
}

static int weft_tok_streq_lit(const char *tok_text, uint32_t len,
                              const char *lit)
{
    size_t m = strlen(lit);
    return ((uint32_t)m == len) && (memcmp(tok_text, lit, m) == 0);
}

static int weft_tok_is_punct(const weft_lint_ast_t *a, uint32_t i,
                             const char *lit)
{
    return (a->nodes[i].kind == WEFT_LINT_TK_PUNCT) &&
           weft_tok_text_eq(a, i, lit);
}

static int weft_tok_is_ident_txt(const weft_lint_ast_t *a, uint32_t i,
                                 const char *lit)
{
    return (a->nodes[i].kind == WEFT_LINT_TK_IDENT) &&
           weft_tok_text_eq(a, i, lit);
}

static int weft_tok_is_hot_name(const weft_lint_ast_t *a, uint32_t i)
{
    return weft_tok_is_ident_txt(a, i, "hot") ||
           weft_tok_is_ident_txt(a, i, "weft_hot") ||
           weft_tok_is_ident_txt(a, i, "WeftHot") ||
           weft_tok_is_ident_txt(a, i, "weftHot");
}

/* ------------------------------------------------------------------ */
/* Hot-attribute pre-pass: marks the FIRST token of every recognized   */
/* annotation with WEFT_LINT_X_HOTATTR.                                */
/* ------------------------------------------------------------------ */

static void weft_mark_hot_attrs(weft_lint_ast_t *a)
{
    uint32_t n = a->node_count;
    for (uint32_t i = 0; i + 1u < n; i++) {
        /* @hot / @weft_hot / @WeftHot / @weftHot (decorators) */
        if (weft_tok_is_punct(a, i, "@") && weft_tok_is_hot_name(a, i + 1u)) {
            a->nodes[i].extra |= WEFT_LINT_X_HOTATTR;
            continue;
        }
        /* #[weft_hot] (Rust) */
        if (weft_tok_is_punct(a, i, "#") && weft_tok_is_punct(a, i + 1u, "[") &&
            i + 2u < n && weft_tok_is_hot_name(a, i + 2u)) {
            a->nodes[i].extra |= WEFT_LINT_X_HOTATTR;
            continue;
        }
        /* [[weft_hot]] (C++11 attribute) */
        if (weft_tok_is_punct(a, i, "[") && weft_tok_is_punct(a, i + 1u, "[") &&
            i + 2u < n && weft_tok_is_hot_name(a, i + 2u)) {
            a->nodes[i].extra |= WEFT_LINT_X_HOTATTR;
            continue;
        }
        /* [[clang::annotate("weft_hot")]] */
        if (weft_tok_is_punct(a, i, "[") && weft_tok_is_punct(a, i + 1u, "[") &&
            i + 7u < n && weft_tok_is_ident_txt(a, i + 2u, "clang") &&
            weft_tok_is_punct(a, i + 3u, "::") &&
            weft_tok_is_ident_txt(a, i + 4u, "annotate") &&
            weft_tok_is_punct(a, i + 5u, "(") &&
            a->nodes[i + 6u].kind == WEFT_LINT_TK_STRING &&
            weft_lint_contains(a->text + a->nodes[i + 6u].text_off,
                               a->nodes[i + 6u].text_len, "weft_hot") &&
            weft_tok_is_punct(a, i + 7u, ")")) {
            a->nodes[i].extra |= WEFT_LINT_X_HOTATTR;
            continue;
        }
        /* __attribute__((weft_hot)) */
        if (weft_tok_is_ident_txt(a, i, "__attribute__") &&
            weft_tok_is_punct(a, i + 1u, "(") &&
            weft_tok_is_punct(a, i + 2u, "(") &&
            i + 3u < n && weft_tok_is_hot_name(a, i + 3u)) {
            a->nodes[i].extra |= WEFT_LINT_X_HOTATTR;
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Function detection                                                  */
/* ------------------------------------------------------------------ */

enum {
    WEFT_CTX_FILE = 0,
    WEFT_CTX_NAMESPACE = 1,
    WEFT_CTX_CLASS = 2,
    WEFT_CTX_OTHER = 3,
    WEFT_CTX_FN = 4
};

#define WEFT_LINT_CTX_MAX 128u

static int weft_fn_is_container_kw(const weft_lint_ast_t *a, uint32_t i)
{
    static const char *const kws[] = {
        "namespace", "class", "struct", "interface", "impl", "enum",
        "module", "extension", "trait", "object", "category", "package",
        NULL
    };
    if (a->nodes[i].kind != WEFT_LINT_TK_IDENT) {
        return 0;
    }
    for (int k = 0; kws[k] != NULL; k++) {
        if (weft_tok_text_eq(a, i, kws[k])) {
            return 1;
        }
    }
    return 0;
}

static int weft_fn_is_control_kw(const weft_lint_ast_t *a, uint32_t i)
{
    static const char *const kws[] = {
        "if", "elseif", "elif", "else", "for", "while", "switch", "case",
        "do", "return", "sizeof", "catch", "finally", "try", "defined",
        "alignof", "typeof", "static_assert", "assert", "while", NULL
    };
    if (a->nodes[i].kind != WEFT_LINT_TK_IDENT) {
        return 0;
    }
    for (int k = 0; kws[k] != NULL; k++) {
        if (weft_tok_text_eq(a, i, kws[k])) {
            return 1;
        }
    }
    return 0;
}

/* Find the opening '(' matching the ')' at index j (searching backwards).
 * Returns the index of '(' or UINT32_MAX. */
static uint32_t weft_fn_match_paren_back(const weft_lint_ast_t *a, uint32_t j)
{
    uint32_t depth = 0u;
    uint32_t steps = 0u;
    for (uint32_t i = j; i-- > 0u;) {
        if (steps++ > 4000u) {
            return WEFT_LINT_UNREACHABLE_UINT32;
        }
        if (weft_tok_is_punct(a, i, ")")) {
            depth++;
            continue;
        }
        if (weft_tok_is_punct(a, i, "(")) {
            if (depth == 0u) {
                return i;
            }
            depth--;
            continue;
        }
        if (weft_tok_is_punct(a, i, ";") || weft_tok_is_punct(a, i, "{") ||
            weft_tok_is_punct(a, i, "}")) {
            return WEFT_LINT_UNREACHABLE_UINT32;
        }
    }
    return WEFT_LINT_UNREACHABLE_UINT32;
}


/* Walk back from `start` (inclusive) over an optional trailing return type
 * ("-> u32", ": string", "const", "noexcept", ...) to the ')' that closes
 * the parameter list. Returns the ')' index or UINT32_MAX. */
static uint32_t weft_fn_find_params_close(const weft_lint_ast_t *a,
                                          uint32_t start)
{
    uint32_t steps = 0u;
    for (uint32_t i = start + 1u; i-- > 0u;) {
        if (steps++ > 64u) {
            return WEFT_LINT_UNREACHABLE_UINT32;
        }
        const weft_lint_node_t *nd = &a->nodes[i];
        if (weft_tok_is_punct(a, i, ")")) {
            return i;
        }
        if (nd->kind == WEFT_LINT_TK_IDENT ||
            nd->kind == WEFT_LINT_TK_NUMBER ||
            nd->kind == WEFT_LINT_TK_CHAR ||   /* Rust lifetimes */
            weft_tok_is_punct(a, i, "::") || weft_tok_is_punct(a, i, "*") ||
            weft_tok_is_punct(a, i, "&") || weft_tok_is_punct(a, i, "->") ||
            weft_tok_is_punct(a, i, "<") || weft_tok_is_punct(a, i, ">") ||
            weft_tok_is_punct(a, i, "[") || weft_tok_is_punct(a, i, "]") ||
            weft_tok_is_punct(a, i, ".") || weft_tok_is_punct(a, i, ",") ||
            weft_tok_is_punct(a, i, ":")) {
            continue;
        }
        return WEFT_LINT_UNREACHABLE_UINT32;
    }
    return WEFT_LINT_UNREACHABLE_UINT32;
}

/* Walk back from token `from` (inclusive) up to 64 tokens looking for a
 * hot-attribute marker. Stops at ';' or '}'. */
static int weft_fn_hot_in_window(const weft_lint_ast_t *a, uint32_t from,
                                 uint32_t *out_attr_pos)
{
    uint32_t steps = 0u;
    for (uint32_t i = from + 1u; i-- > 0u;) {
        if (steps++ > 64u) {
            break;
        }
        if (weft_tok_is_punct(a, i, ";") || weft_tok_is_punct(a, i, "}")) {
            break;
        }
        if ((a->nodes[i].extra & WEFT_LINT_X_HOTATTR) != 0u ||
            (a->nodes[i].extra & WEFT_LINT_X_HOTCOMMENT) != 0u) {
            *out_attr_pos = i;
            return 1;
        }
    }
    return 0;
}

static int weft_lint_reserve_fn(weft_lint_ast_t *a, weft_lint_alloc_fn alloc,
                                void *actx)
{
    return weft_lint_grow(alloc, actx, (void **)&a->funcs, &a->func_cap,
                          sizeof(weft_lint_func_t), a->func_count + 1u);
}

static void weft_detect_functions(weft_lint_ast_t *a, weft_lint_lang_t lang,
                                  weft_lint_alloc_fn alloc, void *actx,
                                  int *out_overflow)
{
    uint8_t ctx[WEFT_LINT_CTX_MAX];
    uint32_t ctx_fn[WEFT_LINT_CTX_MAX]; /* fn idx for CTX_FN frames */
    int ctxd = 0;
    ctx[0] = WEFT_CTX_FILE;
    ctx_fn[0] = WEFT_LINT_UNREACHABLE_UINT32;
    *out_overflow = 0;

    const uint32_t n = a->node_count;
    for (uint32_t i = 0; i < n; i++) {
        if (weft_tok_is_punct(a, i, "{")) {
            uint8_t kind = WEFT_CTX_OTHER;
            uint32_t fnrec = WEFT_LINT_UNREACHABLE_UINT32;
            const int allowed = (ctx[ctxd] == WEFT_CTX_FILE ||
                                 ctx[ctxd] == WEFT_CTX_NAMESPACE ||
                                 ctx[ctxd] == WEFT_CTX_CLASS);
            if (allowed && i > 0u) {
                /* arrow form: ') =>' '{' or 'IDENT =>' '{' */
                if (weft_tok_is_punct(a, i - 1u, "=>")) {
                    uint32_t name = WEFT_LINT_UNREACHABLE_UINT32;
                    if (i >= 2u && weft_tok_is_punct(a, i - 2u, ")")) {
                        uint32_t op =
                            weft_fn_match_paren_back(a, i - 2u);
                        if (op != WEFT_LINT_UNREACHABLE_UINT32 && op > 1u &&
                            weft_tok_is_punct(a, op - 1u, "=") &&
                            a->nodes[op - 2u].kind == WEFT_LINT_TK_IDENT) {
                            name = op - 2u;
                        }
                    } else if (i >= 4u &&
                               a->nodes[i - 2u].kind == WEFT_LINT_TK_IDENT &&
                               weft_tok_is_punct(a, i - 3u, "=") &&
                               a->nodes[i - 4u].kind == WEFT_LINT_TK_IDENT) {
                        name = i - 4u;
                    }
                    if (name != WEFT_LINT_UNREACHABLE_UINT32) {
                        fnrec = name;
                    }
                } else {
                    /* allow an optional trailing return type between the
                     * parameter list ')' and the '{' */
                    uint32_t pc = weft_fn_find_params_close(a, i - 1u);
                    if (pc != WEFT_LINT_UNREACHABLE_UINT32) {
                        uint32_t op = weft_fn_match_paren_back(a, pc);
                        if (op != WEFT_LINT_UNREACHABLE_UINT32 && op > 0u &&
                            a->nodes[op - 1u].kind == WEFT_LINT_TK_IDENT &&
                            !weft_fn_is_control_kw(a, op - 1u) &&
                            !weft_tok_is_punct(a, op - 2u, ".") &&
                            !weft_tok_is_punct(a, op - 2u, "->")) {
                            fnrec = op - 1u;
                        }
                    }
                }
            }
            if (fnrec != WEFT_LINT_UNREACHABLE_UINT32) {
                uint32_t attr_pos = WEFT_LINT_UNREACHABLE_UINT32;
                int hot = weft_fn_hot_in_window(a, fnrec, &attr_pos);
                if (weft_lint_reserve_fn(a, alloc, actx) == 0) {
                    weft_lint_func_t *f = &a->funcs[a->func_count];
                    f->name_off = a->nodes[fnrec].text_off;
                    f->name_len = a->nodes[fnrec].text_len;
                    f->line = a->nodes[fnrec].line;
                    f->col = a->nodes[fnrec].col;
                    f->body_first = i + 1u;
                    f->body_last = WEFT_LINT_UNREACHABLE_UINT32; /* fixed at pop */
                    f->hot_attr_pos = attr_pos;
                    f->is_hot = (uint8_t)(hot ? 1 : 0);
                    f->lang = (uint8_t)lang;
                    f->flags = 0u;
                    kind = WEFT_CTX_FN;
                    ctx_fn[ctxd + 1] = a->func_count;
                    a->func_count++;
                } else {
                    *out_overflow = 1;
                }
            } else if (allowed && i > 0u) {
                int kw_at = -1;
                if (weft_fn_is_container_kw(a, i - 1u)) {
                    kw_at = (int)(i - 1u);
                } else if (i > 1u &&
                           a->nodes[i - 1u].kind == WEFT_LINT_TK_IDENT &&
                           weft_fn_is_container_kw(a, i - 2u)) {
                    kw_at = (int)(i - 2u);
                }
                if (kw_at >= 0) {
                    kind = weft_tok_text_eq(a, (uint32_t)kw_at,
                                            "namespace") ||
                                   weft_tok_text_eq(a, (uint32_t)kw_at,
                                                    "module") ||
                                   weft_tok_text_eq(a, (uint32_t)kw_at,
                                                    "package")
                               ? WEFT_CTX_NAMESPACE
                               : WEFT_CTX_CLASS;
                }
            }
            if (ctxd + 1 < (int)WEFT_LINT_CTX_MAX) {
                ctx[++ctxd] = kind;
            } else {
                ctx[ctxd] = WEFT_CTX_OTHER;
                *out_overflow = 1;
            }
            continue;
        }
        if (weft_tok_is_punct(a, i, "}")) {
            if (ctxd > 0) {
                if (ctx[ctxd] == WEFT_CTX_FN &&
                    ctx_fn[ctxd] != WEFT_LINT_UNREACHABLE_UINT32 &&
                    ctx_fn[ctxd] < a->func_count) {
                    weft_lint_func_t *f = &a->funcs[ctx_fn[ctxd]];
                    f->body_last = (i > 0u) ? i - 1u : 0u;
                }
                ctxd--;
            }
            continue;
        }
        /* arrow-body functions without braces (TS / Dart):
         * '=>' followed by a non-'{' token at an allowed context. */
        if (weft_tok_is_punct(a, i, "=>") && i + 1u < n &&
            !weft_tok_is_punct(a, i + 1u, "{") &&
            (lang == WEFT_LINT_LANG_TYPESCRIPT ||
             lang == WEFT_LINT_LANG_DART)) {
            const int allowed = (ctx[ctxd] == WEFT_CTX_FILE ||
                                 ctx[ctxd] == WEFT_CTX_NAMESPACE ||
                                 ctx[ctxd] == WEFT_CTX_CLASS);
            if (!allowed || i < 2u) {
                continue;
            }
            uint32_t name = WEFT_LINT_UNREACHABLE_UINT32;
            if (weft_tok_is_punct(a, i - 1u, ")")) {
                uint32_t op = weft_fn_match_paren_back(a, i - 1u);
                if (op != WEFT_LINT_UNREACHABLE_UINT32 && op > 1u &&
                    weft_tok_is_punct(a, op - 1u, "=") &&
                    a->nodes[op - 2u].kind == WEFT_LINT_TK_IDENT) {
                    name = op - 2u;
                }
            } else if (a->nodes[i - 1u].kind == WEFT_LINT_TK_IDENT &&
                       i >= 3u && weft_tok_is_punct(a, i - 2u, "=") &&
                       a->nodes[i - 3u].kind == WEFT_LINT_TK_IDENT) {
                name = i - 3u;
            }
            if (name == WEFT_LINT_UNREACHABLE_UINT32) {
                continue;
            }
            /* body: from i+1 to the next ';' at depth 0 */
            uint32_t depth = 0u;
            uint32_t end = n - 1u;
            for (uint32_t j = i + 1u; j + 1u < n; j++) {
                if (weft_tok_is_punct(a, j, "(") ||
                    weft_tok_is_punct(a, j, "[")) {
                    depth++;
                } else if (weft_tok_is_punct(a, j, ")") ||
                           weft_tok_is_punct(a, j, "]")) {
                    depth--;
                } else if (depth == 0u &&
                           (weft_tok_is_punct(a, j, ";") ||
                            weft_tok_is_punct(a, j, "}"))) {
                    end = j;
                    break;
                }
            }
            uint32_t attr_pos = WEFT_LINT_UNREACHABLE_UINT32;
            int hot = weft_fn_hot_in_window(a, name, &attr_pos);
            if (weft_lint_reserve_fn(a, alloc, actx) == 0) {
                weft_lint_func_t *f = &a->funcs[a->func_count];
                f->name_off = a->nodes[name].text_off;
                f->name_len = a->nodes[name].text_len;
                f->line = a->nodes[name].line;
                f->col = a->nodes[name].col;
                f->body_first = i + 1u;
                f->body_last = (end > i + 1u) ? end - 1u : i + 1u;
                f->hot_attr_pos = attr_pos;
                f->is_hot = (uint8_t)(hot ? 1 : 0);
                f->lang = (uint8_t)lang;
                f->flags = 0u;
                a->func_count++;
            } else {
                *out_overflow = 1;
            }
        }
    }
    /* any unclosed CTX_FN frames get clamped bodies */
    for (uint32_t k = 0; k < a->func_count; k++) {
        if (a->funcs[k].body_last == WEFT_LINT_UNREACHABLE_UINT32) {
            a->funcs[k].body_last = (n > 1u) ? n - 2u : 0u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rule tables (static const; matched only inside @hot bodies)        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *rule;
    uint8_t     sev;
} weft_lint_rule_ent_t;

#define WEFT_LINT_SEV_ERROR_ ((uint8_t)WEFT_LINT_ERROR)
#define WEFT_LINT_SEV_WARN_  ((uint8_t)WEFT_LINT_WARNING)

static const weft_lint_rule_ent_t k_rules_heap[] = {
    { "malloc", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "calloc", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "realloc", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "free", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "posix_memalign", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "aligned_alloc", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "memalign", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "valloc", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "pvalloc", "alloc-heap", WEFT_LINT_SEV_ERROR_ },
    { "strdup", "alloc-string", WEFT_LINT_SEV_ERROR_ },
    { "strndup", "alloc-string", WEFT_LINT_SEV_ERROR_ },
    { "asprintf", "alloc-fmt", WEFT_LINT_SEV_ERROR_ },
    { "vasprintf", "alloc-fmt", WEFT_LINT_SEV_ERROR_ },
    { "mmap", "alloc-mmap", WEFT_LINT_SEV_ERROR_ },
    { "mmap64", "alloc-mmap", WEFT_LINT_SEV_ERROR_ },
    { "sbrk", "alloc-mmap", WEFT_LINT_SEV_ERROR_ },
    { "Box", "alloc-heap", WEFT_LINT_SEV_ERROR_ }, /* Box::new, gated below */
    { "String", "alloc-string", WEFT_LINT_SEV_ERROR_ }, /* String::from */
    { "vec", "alloc-heap", WEFT_LINT_SEV_ERROR_ },     /* vec! macro */
    { "format", "alloc-fmt", WEFT_LINT_SEV_ERROR_ },   /* format! macro */
};

static const weft_lint_rule_ent_t k_rules_rust_std[] = {
    { "to_string", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
    { "to_owned", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
    { "to_vec", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
    { "clone", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
    { "collect", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
    { "push", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
    { "Vec", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },   /* Vec::new */
    { "println", "alloc-rust-std", WEFT_LINT_SEV_WARN_ },
};

static const weft_lint_rule_ent_t *weft_rule_match(
    const weft_lint_rule_ent_t *tab, size_t tab_n, const char *s,
    uint32_t len)
{
    for (size_t k = 0; k < tab_n; k++) {
        if ((uint32_t)strlen(tab[k].name) == len &&
            memcmp(tab[k].name, s, len) == 0) {
            return &tab[k];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Diagnostic emission (bounded copies only; zero allocation)          */
/* ------------------------------------------------------------------ */

typedef struct {
    weft_lint_ast_t    *ast;
    weft_lint_diag_t   *diags;
    uint32_t            max;
    uint32_t            written;
    uint32_t            total;
    int                 truncated;
} weft_emit_t;

static void weft_copy_bounded(char *dst, size_t cap, const char *src,
                              size_t n)
{
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void weft_name_buf(const weft_lint_ast_t *a, uint32_t off,
                          uint32_t len, char *buf /* >= 52 bytes */)
{
    weft_copy_bounded(buf, 49u, a->text + off, len);
}

static void weft_emit(weft_emit_t *e, const weft_lint_node_t *at,
                      uint8_t sev, const char *rule, const char *msg,
                      const char *rem)
{
    e->total++;
    if (e->written >= e->max) {
        e->truncated = 1;
        return;
    }
    weft_lint_diag_t *d = &e->diags[e->written++];
    weft_copy_bounded(d->file, sizeof(d->file), e->ast->file,
                      strlen(e->ast->file));
    d->line = at->line;
    d->col = at->col;
    d->severity = sev;
    weft_copy_bounded(d->rule, sizeof(d->rule), rule, strlen(rule));
    weft_copy_bounded(d->message, sizeof(d->message), msg, strlen(msg));
    weft_copy_bounded(d->remediation, sizeof(d->remediation), rem,
                      strlen(rem));
}

static const char *k_rem_heap =
    "Law 1: preallocate from a stack buffer or an arena slot; the hot "
    "plane never touches the heap allocator";
static const char *k_rem_string =
    "use a fixed-size buffer or an arena copy; dynamic string duplication "
    "is a heap allocation";
static const char *k_rem_fmt =
    "format into a caller-provided fixed buffer (snprintf); allocating "
    "format constructors are forbidden on the hot plane";
static const char *k_rem_mmap =
    "map memory at startup; hot paths must not touch the page allocator";
static const char *k_rem_new =
    "reuse pooled or preallocated objects; new/delete are heap operations";
static const char *k_rem_concat =
    "write into a preallocated buffer; dynamic string concatenation "
    "allocates on every call";
static const char *k_rem_recursion =
    "hoist to an iterative loop with a fixed-depth stack; recursion depth "
    "is unbounded on the hot plane";
static const char *k_rem_rust_std =
    "Rust std allocation idiom; use arrays, slices or an arena on the "
    "hot plane";

/* ------------------------------------------------------------------ */
/* Name lookup                                                         */
/* ------------------------------------------------------------------ */

static uint32_t weft_lookup_func(const weft_lint_ast_t *a, const char *s,
                                 uint32_t len)
{
    if (a->name_table == NULL || a->func_count == 0u) {
        return WEFT_LINT_UNREACHABLE_UINT32;
    }
    uint64_t h = weft_lint_fnv1a_64(s, len);
    uint32_t j = (uint32_t)(h & (uint64_t)a->name_table_mask);
    for (uint32_t probes = 0; probes <= a->name_table_mask; probes++) {
        uint32_t fi = a->name_table[j];
        if (fi == 0u) {
            return WEFT_LINT_UNREACHABLE_UINT32;
        }
        fi--;
        if (a->funcs[fi].name_len == len &&
            memcmp(a->text + a->funcs[fi].name_off, s, len) == 0) {
            return fi;
        }
        j = (j + 1u) & a->name_table_mask;
    }
    return WEFT_LINT_UNREACHABLE_UINT32;
}

/* ------------------------------------------------------------------ */
/* SCAN pass — zero heap allocation                                    */
/* ------------------------------------------------------------------ */

static int weft_punct_is(const weft_lint_node_t *nd, const char *a_text,
                         const char *lit)
{
    size_t m = strlen(lit);
    return (nd->kind == WEFT_LINT_TK_PUNCT) && (nd->text_len == (uint32_t)m) &&
           (memcmp(a_text + nd->text_off, lit, m) == 0);
}

int weftc_lint_scan_ast(weft_lint_ast_t *ast, weft_lint_diag_t *diags,
                        uint32_t max_diags, uint32_t *out_written,
                        uint32_t *out_total)
{
    if (ast == NULL || ast->nodes == NULL ||
        (max_diags > 0u && diags == NULL)) {
        return -1;
    }
    weft_emit_t em;
    em.ast = ast;
    em.diags = diags;
    em.max = max_diags;
    em.written = 0u;
    em.total = 0u;
    em.truncated = 0;

    const uint32_t n = ast->node_count;
    const char *tx = ast->text;
    const uint32_t F = ast->func_count;

    ast->edge_count = 0u;
    if (F == 0u) {
        if (out_written) {
            *out_written = 0u;
        }
        if (out_total) {
            *out_total = 0u;
        }
        return 0;
    }

    /* ---------------- phase A: per-function walk ---------------- */
    for (uint32_t f = 0; f < F; f++) {
        weft_lint_func_t *fn = &ast->funcs[f];
        ast->edge_start[f] = ast->edge_count;
        if (fn->body_last == WEFT_LINT_UNREACHABLE_UINT32 ||
            fn->body_first > fn->body_last || fn->body_last >= n) {
            continue;
        }
        char fname[52];
        weft_name_buf(ast, fn->name_off, fn->name_len, fname);
        const weft_lint_lang_t flang = (weft_lint_lang_t)fn->lang;

        for (uint32_t i = fn->body_first; i <= fn->body_last; i++) {
            const weft_lint_node_t *nd = &ast->nodes[i];
            const weft_lint_node_t *prev =
                (i > 0u) ? &ast->nodes[i - 1u] : NULL;
            const weft_lint_node_t *next =
                (i + 1u < n) ? &ast->nodes[i + 1u] : NULL;
            const char *t = tx + nd->text_off;

            if (nd->kind == WEFT_LINT_TK_IDENT) {
                const int call_shape =
                    (next != NULL &&
                     weft_punct_is(next, tx, "("));
                const int prev_dot =
                    (prev != NULL &&
                     (weft_punct_is(prev, tx, ".") ||
                      weft_punct_is(prev, tx, "->") ||
                      weft_punct_is(prev, tx, "::")));
                /* call-edge collection (all functions, for recursion) */
                if (call_shape && !prev_dot) {
                    uint32_t callee =
                        weft_lookup_func(ast, t, nd->text_len);
                    if (callee != WEFT_LINT_UNREACHABLE_UINT32) {
                        if (ast->edge_count + 2u > ast->edge_cap) {
                            return -2;
                        }
                        ast->edges[ast->edge_count * 2u] = f;
                        ast->edges[ast->edge_count * 2u + 1u] = callee;
                        ast->edge_count++;
                    }
                }
                if (!fn->is_hot) {
                    continue;
                }
                /* ---- rule matching inside @hot bodies ---- */
                const weft_lint_rule_ent_t *hit = weft_rule_match(
                    k_rules_heap,
                    sizeof(k_rules_heap) / sizeof(k_rules_heap[0]), t,
                    nd->text_len);
                if (hit != NULL) {
                    /* Rust namespaced/macro forms need qualification:
                     * only the ALLOCATING constructors count (Box::new,
                     * String::from) — Box::leak and friends consume. */
                    int rust_ok = 1;
                    if (flang == WEFT_LINT_LANG_RUST &&
                        weft_tok_streq_lit(t, nd->text_len, "Box")) {
                        const weft_lint_node_t *n2 =
                            (i + 2u < n) ? &ast->nodes[i + 2u] : NULL;
                        rust_ok = (next != NULL &&
                                   weft_punct_is(next, tx, "::") && n2 != NULL &&
                                   n2->kind == WEFT_LINT_TK_IDENT &&
                                   weft_tok_streq_lit(tx + n2->text_off,
                                                      n2->text_len, "new"));
                    }
                    if (flang == WEFT_LINT_LANG_RUST &&
                        weft_tok_streq_lit(t, nd->text_len, "String")) {
                        const weft_lint_node_t *n2 =
                            (i + 2u < n) ? &ast->nodes[i + 2u] : NULL;
                        rust_ok = (next != NULL &&
                                   weft_punct_is(next, tx, "::") && n2 != NULL &&
                                   n2->kind == WEFT_LINT_TK_IDENT &&
                                   weft_tok_streq_lit(tx + n2->text_off,
                                                      n2->text_len, "from"));
                    }
                    if (flang == WEFT_LINT_LANG_RUST &&
                        (weft_tok_streq_lit(t, nd->text_len, "vec") ||
                         weft_tok_streq_lit(t, nd->text_len, "format"))) {
                        rust_ok = (next != NULL &&
                                   weft_punct_is(next, tx, "!"));
                    }
                    if (rust_ok) {
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "call to '%.48s' in @hot function '%.48s'",
                                 hit->name, fname);
                        const char *rem = k_rem_heap;
                        if (strcmp(hit->rule, "alloc-string") == 0) {
                            rem = k_rem_string;
                        } else if (strcmp(hit->rule, "alloc-fmt") == 0) {
                            rem = k_rem_fmt;
                        } else if (strcmp(hit->rule, "alloc-mmap") == 0) {
                            rem = k_rem_mmap;
                        }
                        weft_emit(&em, nd, hit->sev, hit->rule, msg, rem);
                    }
                }
                if ((flang == WEFT_LINT_LANG_CPP ||
                     flang == WEFT_LINT_LANG_TYPESCRIPT)) {
                    if (weft_tok_streq_lit(t, nd->text_len, "new") &&
                        next != NULL &&
                        (next->kind == WEFT_LINT_TK_IDENT ||
                         weft_punct_is(next, tx, "(") ||
                         weft_punct_is(next, tx, "["))) {
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "'new' expression in @hot function '%.48s'",
                                 fname);
                        weft_emit(&em, nd, WEFT_LINT_SEV_ERROR_,
                                  "alloc-new", msg, k_rem_new);
                    } else if (weft_tok_streq_lit(t, nd->text_len,
                                                  "delete")) {
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "'delete' expression in @hot function "
                                 "'%.48s'",
                                 fname);
                        weft_emit(&em, nd, WEFT_LINT_SEV_ERROR_,
                                  "alloc-new", msg, k_rem_new);
                    }
                }
                if (flang == WEFT_LINT_LANG_RUST && prev_dot &&
                    prev != NULL && weft_punct_is(prev, tx, ".")) {
                    const weft_lint_rule_ent_t *rs = weft_rule_match(
                        k_rules_rust_std,
                        sizeof(k_rules_rust_std) / sizeof(k_rules_rust_std[0]),
                        t, nd->text_len);
                    if (rs != NULL) {
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "Rust std allocation '.%.48s()' in @hot "
                                 "function '%.48s'",
                                 rs->name, fname);
                        weft_emit(&em, nd, rs->sev, rs->rule, msg,
                                  k_rem_rust_std);
                    }
                }
                continue;
            }
            if ((nd->kind == WEFT_LINT_TK_STRING ||
                 nd->kind == WEFT_LINT_TK_CHAR) &&
                (nd->extra & WEFT_LINT_X_INTERP) != 0u && fn->is_hot &&
                weft_lint_lang_is_managed(flang)) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "interpolated string allocation in @hot function "
                         "'%.48s'",
                         fname);
                weft_emit(&em, nd, WEFT_LINT_SEV_ERROR_, "alloc-concat", msg,
                          k_rem_concat);
                continue;
            }
            if (nd->kind == WEFT_LINT_TK_PUNCT && fn->is_hot &&
                (tx[nd->text_off] == '+') &&
                (nd->text_len == 1u || nd->text_len == 2u)) {
                int adjacent_string = 0;
                if (prev != NULL &&
                    (prev->kind == WEFT_LINT_TK_STRING ||
                     prev->kind == WEFT_LINT_TK_CHAR)) {
                    adjacent_string = 1;
                }
                if (next != NULL &&
                    (next->kind == WEFT_LINT_TK_STRING ||
                     next->kind == WEFT_LINT_TK_CHAR)) {
                    adjacent_string = 1;
                }
                if (adjacent_string && weft_lint_lang_is_managed(flang)) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "dynamic string concatenation in @hot function "
                             "'%.48s'",
                             fname);
                    weft_emit(&em, nd, WEFT_LINT_SEV_ERROR_, "alloc-concat",
                              msg, k_rem_concat);
                } else if (adjacent_string && flang == WEFT_LINT_LANG_RUST) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "string concatenation in @hot function '%.48s'",
                             fname);
                    weft_emit(&em, nd, WEFT_LINT_SEV_WARN_, "alloc-rust-std",
                              msg, k_rem_rust_std);
                }
            }
        }
    }
    ast->edge_start[F] = ast->edge_count;

    /* ------------- phase B: recursion analysis (Tarjan SCC) -------- */
    {
        uint32_t *scr = ast->scan_scratch;
        const uint32_t cap_units = ast->scan_scratch_cap / 7u;
        if (cap_units >= F + 2u) {
            uint32_t *idx = scr;
            uint32_t *low = scr + cap_units;
            uint32_t *onstk = scr + 2u * cap_units;
            uint32_t *tstack = scr + 3u * cap_units;
            uint32_t *csv = scr + 4u * cap_units;
            uint32_t *cse = scr + 5u * cap_units;
            uint32_t *members = scr + 6u * cap_units;
            for (uint32_t k = 0; k < F; k++) {
                idx[k] = WEFT_LINT_UNREACHABLE_UINT32;
                onstk[k] = 0u;
                members[k] = 0u;
            }
            /* self-recursion (dedup per function) */
            for (uint32_t e = 0; e < ast->edge_count; e++) {
                uint32_t a = ast->edges[e * 2u];
                uint32_t b = ast->edges[e * 2u + 1u];
                if (a == b && ast->funcs[a].is_hot && members[a] == 0u) {
                    members[a] = 1u;
                    char fname[52];
                    weft_name_buf(ast, ast->funcs[a].name_off,
                                  ast->funcs[a].name_len, fname);
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "self-recursive call in @hot function '%.48s'",
                             fname);
                    weft_emit(&em, &ast->nodes[
                             (ast->funcs[a].body_first > 0u)
                                 ? ast->funcs[a].body_first - 1u
                                 : 0],
                             WEFT_LINT_SEV_ERROR_, "alloc-recursion", msg,
                             k_rem_recursion);
                }
            }
            for (uint32_t k = 0; k < F; k++) {
                members[k] = 0u;
            }
            /* iterative Tarjan */
            uint32_t next_id = 0u;
            uint32_t tsp = 0u;
            uint32_t csp = 0u;
            for (uint32_t root = 0; root < F; root++) {
                if (idx[root] != WEFT_LINT_UNREACHABLE_UINT32) {
                    continue;
                }
                idx[root] = low[root] = next_id++;
                onstk[root] = 1u;
                tstack[tsp++] = root;
                csv[csp] = root;
                cse[csp] = ast->edge_start[root];
                csp++;
                while (csp > 0u) {
                    uint32_t v = csv[csp - 1u];
                    int descended = 0;
                    while (cse[csp - 1u] < ast->edge_start[v + 1u]) {
                        uint32_t e = cse[csp - 1u]++;
                        uint32_t w = ast->edges[e * 2u + 1u];
                        if (idx[w] == WEFT_LINT_UNREACHABLE_UINT32) {
                            idx[w] = low[w] = next_id++;
                            onstk[w] = 1u;
                            tstack[tsp++] = w;
                            csv[csp] = w;
                            cse[csp] = ast->edge_start[w];
                            csp++;
                            descended = 1;
                            break;
                        }
                        if (onstk[w] != 0u && idx[w] < low[v]) {
                            low[v] = idx[w];
                        }
                    }
                    if (descended) {
                        continue;
                    }
                    csp--;
                    if (low[v] == idx[v]) {
                        uint32_t mcnt = 0u;
                        while (tsp > 0u) {
                            uint32_t w = tstack[--tsp];
                            onstk[w] = 0u;
                            members[mcnt++] = w;
                            if (w == v) {
                                break;
                            }
                        }
                        if (mcnt >= 2u) {
                            for (uint32_t k = 0; k < mcnt; k++) {
                                uint32_t m = members[k];
                                if (!ast->funcs[m].is_hot) {
                                    continue;
                                }
                                char fname[52];
                                weft_name_buf(ast, ast->funcs[m].name_off,
                                              ast->funcs[m].name_len, fname);
                                char msg[160];
                                snprintf(msg, sizeof(msg),
                                         "unbounded recursion: cycle of %u "
                                         "functions includes @hot '%.48s'",
                                         (unsigned)mcnt, fname);
                                weft_emit(&em, &ast->nodes[
                                             (ast->funcs[m].body_first > 0u)
                                                 ? ast->funcs[m].body_first -
                                                       1u
                                                 : 0],
                                         WEFT_LINT_SEV_ERROR_,
                                         "alloc-recursion", msg,
                                         k_rem_recursion);
                            }
                        }
                        for (uint32_t k = 0; k < mcnt; k++) {
                            members[k] = 0u;
                        }
                    }
                    if (csp > 0u) {
                        uint32_t p = csv[csp - 1u];
                        if (low[v] < low[p]) {
                            low[p] = low[v];
                        }
                    }
                }
            }
        }
    }

    if (out_written) {
        *out_written = em.written;
    }
    if (out_total) {
        *out_total = em.total;
    }
    return em.truncated ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public API: parse / free / version                                  */
/* ------------------------------------------------------------------ */

int weftc_lint_parse(const char *source, size_t source_len,
                     const char *filename, weft_lint_lang_t lang,
                     weft_lint_alloc_fn alloc, void *alloc_ctx,
                     weft_lint_ast_t **out_ast)
{
    if (source == NULL || out_ast == NULL || alloc == NULL) {
        return -1;
    }
    if (source_len > 0x7FFFFFFFu) {
        return -1; /* uint32 token offsets; refuse > 2 GiB sources */
    }
    *out_ast = NULL;
    weft_lint_ast_t *ast = alloc(alloc_ctx, NULL, sizeof(weft_lint_ast_t));
    if (ast == NULL) {
        return -2;
    }
    memset(ast, 0, sizeof(*ast));
    if (filename != NULL) {
        weft_copy_bounded(ast->file, sizeof(ast->file), filename,
                          strlen(filename));
    } else {
        weft_copy_bounded(ast->file, sizeof(ast->file), "<input>", 7u);
    }
    ast->source_bytes = (uint32_t)source_len;
    if (lang == WEFT_LINT_LANG_AUTO) {
        lang = weft_lint_sniff(source, source_len);
    }

    /* intern the source as the text arena */
    size_t tcap = (source_len > 0u) ? source_len : 1u;
    ast->text = alloc(alloc_ctx, NULL, tcap);
    if (ast->text == NULL) {
        alloc(alloc_ctx, ast, 0u);
        return -2;
    }
    if (source_len > 0u) {
        memcpy(ast->text, source, source_len);
    }
    ast->text_cap = (uint32_t)tcap;
    ast->text_len = (uint32_t)source_len;

    /* tokenize */
    weft_tok_ctx_t tc;
    tc.src = source;
    tc.n = source_len;
    tc.pos = 0u;
    tc.line = 1u;
    tc.col = 1u;
    tc.ast = ast;
    tc.alloc = alloc;
    tc.actx = alloc_ctx;
    if (weft_tok_run(&tc, lang) != 0) {
        weftc_lint_free_ast(ast, alloc, alloc_ctx);
        return -2;
    }

    /* hot-attribute pre-pass + function detection */
    weft_mark_hot_attrs(ast);
    int overflow = 0;
    weft_detect_functions(ast, lang, alloc, alloc_ctx, &overflow);

    /* name lookup table */
    uint32_t fcap = (ast->func_cap > 0u) ? ast->func_cap : 1u;
    uint32_t tsize = weft_lint_next_pow2_u32(
        (fcap * 2u > 16u) ? fcap * 2u : 16u);
    ast->name_table = alloc(alloc_ctx, NULL, (size_t)tsize * sizeof(uint32_t));
    if (ast->name_table == NULL) {
        weftc_lint_free_ast(ast, alloc, alloc_ctx);
        return -2;
    }
    memset(ast->name_table, 0, (size_t)tsize * sizeof(uint32_t));
    ast->name_table_mask = tsize - 1u;
    for (uint32_t f = 0; f < ast->func_count; f++) {
        uint64_t h = weft_lint_fnv1a_64(ast->text + ast->funcs[f].name_off,
                                        ast->funcs[f].name_len);
        uint32_t j = (uint32_t)(h & (uint64_t)ast->name_table_mask);
        while (ast->name_table[j] != 0u) {
            j = (j + 1u) & ast->name_table_mask;
        }
        ast->name_table[j] = f + 1u;
    }

    /* scan-phase capacity reservation (all filled with ZERO allocation
     * during weftc_lint_scan_ast) */
    ast->edge_cap = ast->node_count / 2u + 8u;
    ast->edges = alloc(alloc_ctx, NULL,
                       (size_t)ast->edge_cap * 2u * sizeof(uint32_t));
    ast->edge_start = alloc(alloc_ctx, NULL,
                            (size_t)(ast->func_count + 1u) * sizeof(uint32_t));
    {
        uint32_t units = (ast->func_cap > 16u) ? ast->func_cap : 16u;
        ast->scan_scratch_cap = 7u * (units + 4u);
        ast->scan_scratch =
            alloc(alloc_ctx, NULL,
                  (size_t)ast->scan_scratch_cap * sizeof(uint32_t));
    }
    if (ast->edges == NULL || ast->edge_start == NULL ||
        ast->scan_scratch == NULL) {
        weftc_lint_free_ast(ast, alloc, alloc_ctx);
        return -2;
    }

    *out_ast = ast;
    return overflow ? 1 : 0;
}

void weftc_lint_free_ast(weft_lint_ast_t *ast, weft_lint_alloc_fn alloc,
                         void *alloc_ctx)
{
    if (ast == NULL || alloc == NULL) {
        return;
    }
    alloc(alloc_ctx, ast->nodes, 0u);
    alloc(alloc_ctx, ast->funcs, 0u);
    alloc(alloc_ctx, ast->text, 0u);
    alloc(alloc_ctx, ast->edges, 0u);
    alloc(alloc_ctx, ast->edge_start, 0u);
    alloc(alloc_ctx, ast->name_table, 0u);
    alloc(alloc_ctx, ast->scan_scratch, 0u);
    alloc(alloc_ctx, ast, 0u);
}

const char *weftc_lint_version(void)
{
    return "weftc-lint 1.0.0 (Weft Pillar 8, --lint-alloc)";
}

uint32_t weftc_lint_abi_version(void)
{
    return WEFTC_LINT_ABI_VERSION;
}

/* ------------------------------------------------------------------ */
/* SLA benchmark: deterministic synthetic AST + timed zero-alloc scans */
/* ------------------------------------------------------------------ */

static double weft_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    ok;
} weft_bench_buf_t;

static void weft_bench_append(weft_bench_buf_t *b, const char *s)
{
    size_t m = strlen(s);
    if (b->len + m + 1u > b->cap) {
        size_t ncap = (b->cap == 0u) ? 65536u : b->cap;
        while (b->len + m + 1u > ncap) {
            ncap *= 2u;
        }
        char *np = realloc(b->buf, ncap);
        if (np == NULL) {
            b->ok = 0;
            return;
        }
        b->buf = np;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, s, m);
    b->len += m;
    b->buf[b->len] = '\0';
}

static void weft_bench_gen(weft_bench_buf_t *b, uint32_t fns)
{
    b->len = 0u;
    if (b->buf != NULL) {
        b->buf[0] = '\0';
    }
    char line[256];
    for (uint32_t i = 0; i < fns; i++) {
        if (i % 13u == 0u) {
            int m = snprintf(line, sizeof(line),
                             "/* @hot */ static unsigned r%u(unsigned x)"
                             "{if(x<2u)return x;"
                             "return r%u(x-1u)+r%u(x-2u);}\n",
                             (unsigned)i, (unsigned)i, (unsigned)i);
            if (m > 0) {
                line[(size_t)m >= sizeof(line) ? sizeof(line) - 1u
                                               : (size_t)m] = '\0';
                weft_bench_append(b, line);
            }
        } else if (i % 7u == 0u) {
            int m = snprintf(line, sizeof(line),
                             "static unsigned g%u(unsigned x)"
                             "{unsigned t[4];"
                             "t[0]=(unsigned)(size_t)malloc(16u);"
                             "t[1]=x+t[0];"
                             "free((void*)(size_t)t[1]);"
                             "return t[1];}\n",
                             (unsigned)i);
            if (m > 0) {
                line[(size_t)m >= sizeof(line) ? sizeof(line) - 1u
                                               : (size_t)m] = '\0';
                weft_bench_append(b, line);
            }
        } else {
            int m = snprintf(line, sizeof(line),
                             "static unsigned f%u(unsigned x)"
                             "{unsigned t[4];t[0]=x+%uu;"
                             "t[1]=t[0]*3u;t[2]=t[1]^%uu;"
                             "t[3]=t[2]+t[1];return t[3];}\n",
                             (unsigned)i, (unsigned)i, (unsigned)i);
            if (m > 0) {
                line[(size_t)m >= sizeof(line) ? sizeof(line) - 1u
                                               : (size_t)m] = '\0';
                weft_bench_append(b, line);
            }
        }
        if (!b->ok) {
            return;
        }
    }
}

int weftc_lint_benchmark(uint32_t target_nodes, uint32_t rounds,
                         double *out_best_ms, double *out_avg_ms,
                         uint32_t *out_nodes, uint32_t *out_diags)
{
    if (out_best_ms == NULL || out_avg_ms == NULL || out_nodes == NULL ||
        out_diags == NULL) {
        return -1;
    }
    if (rounds == 0u) {
        rounds = 1u;
    }
    if (rounds > 64u) {
        rounds = 64u;
    }
    if (target_nodes < 1000u) {
        target_nodes = 1000u;
    }
    if (target_nodes > 2000000u) {
        target_nodes = 2000000u;
    }

    weft_bench_buf_t b;
    b.buf = NULL;
    b.len = 0u;
    b.cap = 0u;
    b.ok = 1;

    weft_lint_ast_t *ast = NULL;
    uint32_t fns = target_nodes / 24u + 64u;
    for (int attempt = 0; attempt < 5; attempt++) {
        weft_bench_gen(&b, fns);
        if (!b.ok) {
            free(b.buf);
            return -2;
        }
        int rc = weftc_lint_parse(b.buf, b.len, "<bench-synth>",
                                  WEFT_LINT_LANG_C, weft_lint_default_alloc,
                                  NULL, &ast);
        if (rc != 0 || ast == NULL) {
            free(b.buf);
            return -2;
        }
        if (ast->node_count >= target_nodes) {
            break;
        }
        weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
        ast = NULL;
        fns = (uint32_t)(((uint64_t)fns * 5u) / 4u) + 64u;
        if (ast != NULL) {
            break;
        }
    }
    if (ast == NULL) {
        free(b.buf);
        return -2;
    }

    weft_lint_diag_t *diags = malloc(sizeof(weft_lint_diag_t) * 16384u);
    if (diags == NULL) {
        weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
        free(b.buf);
        return -2;
    }

    double best = 1e30;
    double sum = 0.0;
    uint32_t written = 0u;
    uint32_t total = 0u;
    for (uint32_t r = 0; r < rounds; r++) {
        double t0 = weft_now_ms();
        (void)weftc_lint_scan_ast(ast, diags, 16384u, &written, &total);
        double dt = weft_now_ms() - t0;
        sum += dt;
        if (dt < best) {
            best = dt;
        }
    }

    *out_best_ms = best;
    *out_avg_ms = sum / (double)rounds;
    *out_nodes = ast->node_count;
    *out_diags = total;

    free(diags);
    weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
    free(b.buf);
    return 0;
}



