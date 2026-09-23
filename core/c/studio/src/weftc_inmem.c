/* weftc_inmem.c — the Weft Studio in-memory .weft schema compiler.
 *
 * Compiles a raw string buffer of RFC-0017 schema text into:
 *   - an AST reflection tree (breadcrumb view for the editor),
 *   - bit-exact struct layout tables (offsets, padding, cache lines),
 *   - WH64 schema fingerprints over the canonical WAB1/WID1/WDC1
 *     manifests — byte-identical to the weftc reference compiler of
 *     RFC-0017 (golden-pinned in tests/studio/core),
 *   - 7-target code preview strings (C, C++, Rust, TypeScript, Python,
 *     Dart, Swift).
 *
 * Engine discipline (Law 1 / WASM portability): this translation unit
 * uses <string.h> (memcpy/memset/memmove/memcmp) and nothing else —
 * no stdio, no malloc, no syscalls, no threads, no atomics. It compiles
 * unchanged for native hosts and wasm32-unknown-emscripten /
 * wasm32-unknown-unknown. All pools are fixed arrays inside the
 * caller-provided context; exhaustion is an EBOUNDS refusal.
 *
 * Parse errors are DIAGNOSTICS (the studio is live: partial layouts
 * while typing are the product), never crashes: panic-mode recovery
 * synchronizes on ',' '}' ';' and declaration keywords.
 */
#include "weft_studio_internal.h"

/* ------------------------------------------------------------------ */
/* Primitive registry (frozen order: RFC-0017 PrimKind 1..12)           */
/* ------------------------------------------------------------------ */
const char *const weft_prim_names[13] = {
    NULL, "u8", "i8", "u16", "i16", "u32", "i32",
    "u64", "i64", "f16", "f32", "f64", "bool"
};
const uint8_t weft_prim_sizes[13]  = { 0, 1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8, 1 };
const uint8_t weft_prim_aligns[13] = { 0, 1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8, 1 };

/* Signed/unsigned split used by enum/bitflags backing validation. */
static const uint8_t prim_is_int[13] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0
};
static const uint8_t prim_is_uint[13] = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0
};

const char *weft_prim_name(uint32_t tag)
{
    if (tag == 0 || tag > 12u) return NULL;
    return weft_prim_names[tag];
}
uint64_t weft_prim_size(uint32_t tag)
{
    if (tag == 0 || tag > 12u) return 0;
    return weft_prim_sizes[tag];
}
uint64_t weft_prim_align(uint32_t tag)
{
    if (tag == 0 || tag > 12u) return 0;
    return weft_prim_aligns[tag];
}

/* ------------------------------------------------------------------ */
/* Status / diagnostic code naming                                     */
/* ------------------------------------------------------------------ */
const char *weft_studio_strerror(int code)
{
    switch (code) {
    case WEFT_STUDIO_OK:      return "ok";
    case WEFT_STUDIO_EPARSE:  return "EPARSE: malformed input";
    case WEFT_STUDIO_EALIGN:  return "EALIGN: alignment contract violated";
    case WEFT_STUDIO_ETRUNC:  return "ETRUNC: truncated stream / end of trace";
    case WEFT_STUDIO_ECRC:    return "ECRC: CRC-32C integrity failure";
    case WEFT_STUDIO_EBOUNDS: return "EBOUNDS: capacity/bounds refusal";
    default:                  return "unknown status code";
    }
}

const char *weft_diag_code_name(uint32_t code)
{
    static char box[24];
    if (code >= WEFT_D_STUDIO_BASE) {
        /* "STUDIO-2101" (16 bytes max incl. NUL) */
        uint32_t n = code, i = 0;
        char tmp[12];
        uint32_t j = 0;
        if (n == 0) return "STUDIO-?";
        while (n && j < 10) { tmp[j++] = (char)('0' + (n % 10)); n /= 10; }
        box[i++] = 'S'; box[i++] = 'T'; box[i++] = 'U'; box[i++] = 'D';
        box[i++] = 'I'; box[i++] = 'O'; box[i++] = '-';
        while (j) box[i++] = tmp[--j];
        box[i] = '\0';
        return box;
    }
    if (code >= WEFT_D_WW_BASE) {
        uint32_t n = code - WEFT_D_WW_BASE + 1; /* WW001.. */
        uint32_t i = 0;
        if (n == 0 || n > 999) return "WW?";
        box[i++] = 'W'; box[i++] = 'W';
        box[i++] = (char)('0' + (n / 100) % 10);
        box[i++] = (char)('0' + (n / 10) % 10);
        box[i++] = (char)('0' + n % 10);
        box[i] = '\0';
        return box;
    }
    {
        uint32_t n = code, i = 0;
        if (n == 0 || n > 999) return "WE?";
        box[i++] = 'W'; box[i++] = 'E';
        box[i++] = (char)('0' + (n / 100) % 10);
        box[i++] = (char)('0' + (n / 10) % 10);
        box[i++] = (char)('0' + n % 10);
        box[i] = '\0';
        return box;
    }
}

/* ------------------------------------------------------------------ */
/* WH64 + FNV-1a-64 (frozen at IR version 1 — RFC-0017 §5)             */
/* ------------------------------------------------------------------ */
static uint64_t wh_mix64(uint64_t z)
{
    z ^= z >> 30; z *= UINT64_C(0xbf58476d1ce4e5b9);
    z ^= z >> 27; z *= UINT64_C(0x94d049bb133111eb);
    z ^= z >> 31;
    return z;
}

uint64_t weft_wh64(const uint8_t *data, size_t len)
{
    uint64_t h = UINT64_C(0x9E3779B97F4A7C15) ^ wh_mix64((uint64_t)len);
    size_t i = 0;
    if (len == 0) return wh_mix64(h);
    for (; len - i >= 8; i += 8) {
        uint64_t w = (uint64_t)data[i]
                   | ((uint64_t)data[i + 1] << 8)
                   | ((uint64_t)data[i + 2] << 16)
                   | ((uint64_t)data[i + 3] << 24)
                   | ((uint64_t)data[i + 4] << 32)
                   | ((uint64_t)data[i + 5] << 40)
                   | ((uint64_t)data[i + 6] << 48)
                   | ((uint64_t)data[i + 7] << 56);
        h = wh_mix64(h + w + UINT64_C(0x165667B19E3779F9));
    }
    if (i < len) { /* tail: 1..7 bytes, little-endian fold */
        uint64_t t = 0;
        size_t j = i;
        for (; j < len; j++)
            t |= (uint64_t)data[j] << (8 * (j - i));
        h = wh_mix64(h ^ (t + UINT64_C(0xC2B2AE3D27D4EB4F)));
    }
    return wh_mix64(h);
}

uint64_t weft_fnv1a64(const uint8_t *data, size_t len)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

void weft_mix_u64le(uint8_t *out, uint64_t v)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    out[3] = (uint8_t)((v >> 24) & 0xFF);
    out[4] = (uint8_t)((v >> 32) & 0xFF);
    out[5] = (uint8_t)((v >> 40) & 0xFF);
    out[6] = (uint8_t)((v >> 48) & 0xFF);
    out[7] = (uint8_t)((v >> 56) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* String pools                                                        */
/* ------------------------------------------------------------------ */
int weft_strpool_add(weft_strpool_t *p, const char *s, uint32_t len)
{
    uint32_t need = len + 1u;
    if ((uint64_t)p->n + (uint64_t)need > (uint64_t)p->cap) return -1;
    if (len) memcpy(p->base + p->n, s, len);
    p->base[p->n + len] = '\0';
    {
        uint32_t at = p->n;
        p->n += need;
        return (int)at;
    }
}

/* ctx-scoped name pool add; returns pool offset or -1 (capacity). */
static int32_t name_pool_add(weftc_ctx_t *c, const char *s, uint32_t len)
{
    weft_strpool_t p;
    int at;
    p.base = c->name_pool; p.cap = WEFTC_MAX_NAME_POOL; p.n = c->name_pool_n;
    at = weft_strpool_add(&p, s, len);
    if (at < 0) return -1;
    c->name_pool_n = p.n;
    return (int32_t)at;
}

/* ------------------------------------------------------------------ */
/* Diagnostic staging (messages composed into the ctx pool)            */
/* ------------------------------------------------------------------ */
typedef struct {
    char    *base;
    uint32_t cap;
    uint32_t n;
    int      overflow;
} msgbuf_t;

static void mb_raw(msgbuf_t *b, const char *s, uint32_t len)
{
    if ((uint64_t)b->n + (uint64_t)len > (uint64_t)b->cap) {
        b->overflow = 1;
        return;
    }
    if (len) memcpy(b->base + b->n, s, len);
    b->n += len;
}

static void mbx_str(msgbuf_t *b, const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    mb_raw(b, s, n);
}

/* Stage a diagnostic. msg built by caller into a scratch then copied:
 * simpler variant — caller passes composed parts. */
static void diag_stage(weftc_ctx_t *c, uint32_t code, uint16_t sev,
                       const char *msg, const char *fix,
                       uint32_t start_off, uint32_t end_off)
{
    weftc_diagst_t *d;
    msgbuf_t mb;
    if (c->nstaging >= WEFTC_MAX_DIAGS) {
        c->diags_dropped++;
        return;
    }
    d = &c->staging[c->nstaging];
    d->code = code;
    d->sev = sev;
    d->reserved = 0;
    d->start_off = start_off;
    d->end_off = end_off > start_off ? end_off : start_off + 1u;
    d->msg_off = c->msg_pool_n;
    d->fix_off = UINT32_MAX;
    mb.base = c->msg_pool + c->msg_pool_n;
    mb.cap = WEFTC_MAX_MSG_POOL - c->msg_pool_n;
    mb.n = 0;
    mb.overflow = 0;
    mbx_str(&mb, msg);
    mb_raw(&mb, "\0", 1);          /* NUL terminator */
    if (mb.overflow) {
        c->msg_pool_n = WEFTC_MAX_MSG_POOL;
        d->msg_off = c->msg_pool_n; /* empty */
    } else {
        c->msg_pool_n += mb.n;
    }
    if (fix) {
        d->fix_off = c->msg_pool_n;
        mb.base = c->msg_pool + c->msg_pool_n;
        mb.cap = WEFTC_MAX_MSG_POOL - c->msg_pool_n;
        mb.n = 0;
        mb.overflow = 0;
        mbx_str(&mb, fix);
        mb_raw(&mb, "\0", 1);      /* NUL terminator */
        if (mb.overflow) {
            c->msg_pool_n = WEFTC_MAX_MSG_POOL;
            d->fix_off = UINT32_MAX;
        } else {
            c->msg_pool_n += mb.n;
        }
    }
    c->nstaging++;
}

/* Formatted staging: printf-flavored composition with a tiny formatter
 * supporting %s (string), %u (u64), %d (i64), %x (u64 hex). Used for the
 * rich WE messages. fmt literals are engine-owned constants. */
static void diag_fmsg(weftc_ctx_t *c, char *scratch, uint32_t scratch_cap,
                      const char *fmt, const char *str_a, const char *str_b,
                      uint64_t num_a, uint64_t num_b, int64_t inum_a)
{
    uint32_t i = 0, o = 0;
    (void)c;
    while (fmt[i] && o + 32u < scratch_cap) {
        if (fmt[i] == '%' && fmt[i + 1]) {
            char f = fmt[i + 1];
            i += 2;
            if (f == 's') {
                const char *s = str_a;
                str_a = str_b; str_b = NULL;
                if (s) {
                    while (*s && o + 32u < scratch_cap)
                        scratch[o++] = *s++;
                }
            } else if (f == 'u') {
                uint64_t v = num_a; num_a = num_b; num_b = 0;
                {
                    char tmp[24];
                    uint32_t k = 0, j;
                    if (v == 0) tmp[k++] = '0';
                    while (v) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
                    for (j = k; j > 0; j--) scratch[o++] = tmp[j - 1];
                }
            } else if (f == 'd') {
                int64_t v = inum_a;
                uint64_t m;
                char tmp[24];
                uint32_t k = 0, j;
                if (v < 0) { scratch[o++] = '-'; m = (uint64_t)(-(v + 1)) + 1u; }
                else m = (uint64_t)v;
                if (m == 0) tmp[k++] = '0';
                while (m) { tmp[k++] = (char)('0' + (m % 10)); m /= 10; }
                for (j = k; j > 0; j--) scratch[o++] = tmp[j - 1];
            } else if (f == 'x') {
                uint64_t v = num_a;
                char tmp[20];
                uint32_t k = 0, j;
                const char hexd[] = "0123456789abcdef";
                if (v == 0) tmp[k++] = '0';
                while (v) { tmp[k++] = hexd[v & 0xF]; v >>= 4; }
                for (j = k; j > 0; j--) scratch[o++] = tmp[j - 1];
            } else {
                scratch[o++] = fmt[i - 1];
                scratch[o++] = f;
            }
        } else {
            scratch[o++] = fmt[i++];
        }
    }
    scratch[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    weftc_ctx_t *c;
    const char *src;
    size_t      len;
    uint32_t    pos;
    uint32_t    line;         /* 1-based                        */
    uint32_t    line_start;   /* byte offset of line start      */
} Lexer;

static int lx_is_ident0(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}
static int lx_is_ident(uint8_t ch)
{
    return lx_is_ident0(ch) || (ch >= '0' && ch <= '9');
}

static void lx_line_start(Lexer *lx)
{
    /* records lx->line_start (the byte AFTER the consumed newline), NOT
     * lx->pos — pos only advances on token emission, so it would record
     * stale positions and shift every diagnostic range. */
    if (lx->c->nlines < WEFTC_MAX_LINES)
        lx->c->line_starts[lx->c->nlines++] = lx->line_start;
    else
        lx->c->capacity_hit = 1;
}

/* Emit one token into *t; never fails (TOK_EOF terminates). */
static void lex_one(Lexer *lx, weft_tok_t *t)
{
    const char *s = lx->src;
    size_t len = lx->len;
    uint32_t i = lx->pos;
    weftc_ctx_t *c = lx->c;

    for (;;) {
        if (i >= (uint32_t)len) {
            t->kind = TOK_EOF; t->off = i; t->len = 0;
            t->line = lx->line; t->col = 1; t->ch = 0;
            return;
        }
        {
            uint8_t ch = (uint8_t)s[i];
            if (ch == '\n') {
                i++; lx->line++; lx->line_start = i; lx_line_start(lx);
                continue;
            }
            if (ch == '\r') {
                i++;
                if (i < (uint32_t)len && (uint8_t)s[i] == '\n') i++;
                lx->line++; lx->line_start = i; lx_line_start(lx);
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v') {
                i++;
                continue;
            }
            if (ch == '/' && i + 1 < (uint32_t)len) {
                if ((uint8_t)s[i + 1] == '/') { /* line comment */
                    uint32_t st = i;
                    while (i < (uint32_t)len && (uint8_t)s[i] != '\n') i++;
                    t->kind = TOK_COMMENT_LINE; t->off = st;
                    t->len = i - st;
                    t->line = lx->line;
                    t->col = st - lx->line_start + 1u;
                    lx->pos = i;
                    return;
                }
                if ((uint8_t)s[i + 1] == '*') { /* block comment */
                    uint32_t st = i;
                    uint32_t cl = lx->line, cls = lx->line_start;
                    i += 2;
                    for (;;) {
                        if (i + 1 >= (uint32_t)len) {
                            /* unterminated */
                            char scratch[96];
                            diag_fmsg(c, scratch, sizeof scratch,
                                      "unterminated block comment", NULL,
                                      NULL, 0, 0, 0);
                            diag_stage(c, 22, 1, scratch, NULL,
                                       st, (uint32_t)len);
                            t->kind = TOK_COMMENT_BLOCK; t->off = st;
                            t->len = (uint32_t)len - st;
                            t->line = cl; t->col = st - cls + 1u;
                            lx->pos = (uint32_t)len;
                            return;
                        }
                        if ((uint8_t)s[i] == '*' && (uint8_t)s[i + 1] == '/') {
                            i += 2;
                            break;
                        }
                        if ((uint8_t)s[i] == '\n') {
                            lx->line++; i++;
                            lx->line_start = i; lx_line_start(lx);
                        } else {
                            i++;
                        }
                    }
                    t->kind = TOK_COMMENT_BLOCK; t->off = st;
                    t->len = i - st;
                    t->line = cl; t->col = st - cls + 1u;
                    lx->pos = i;
                    return;
                }
            }
            break;
        }
    }

    {
        uint8_t ch = (uint8_t)s[i];
        uint32_t st = i;
        if (lx_is_ident0(ch)) {
            while (i < (uint32_t)len && lx_is_ident((uint8_t)s[i])) i++;
            t->kind = TOK_IDENT; t->off = st; t->len = i - st;
            t->line = lx->line; t->col = st - lx->line_start + 1u;
            if (t->len > WEFTC_MAX_IDENT) {
                char scratch[160];
                diag_fmsg(c, scratch, sizeof scratch,
                          "identifier exceeds the 255-byte limit", NULL, NULL,
                          0, 0, 0);
                diag_stage(c, 36, 1, scratch, NULL, st, i);
            }
            lx->pos = i;
            return;
        }
        if ((ch >= '0' && ch <= '9')) {
            uint64_t acc = 0;
            int overflow = 0, any = 0;
            int base = 10;
            if (ch == '0' && i + 1 < (uint32_t)len) {
                if ((uint8_t)s[i + 1] == 'x' || (uint8_t)s[i + 1] == 'X') {
                    base = 16; i += 2;
                } else if ((uint8_t)s[i + 1] == 'b' || (uint8_t)s[i + 1] == 'B') {
                    base = 2;  i += 2;
                }
            }
            while (i < (uint32_t)len) {
                uint8_t d = (uint8_t)s[i];
                int dv;
                if (d == '_') { i++; continue; }
                if (d >= '0' && d <= '9') dv = d - '0';
                else if (base == 16 && d >= 'a' && d <= 'f') dv = 10 + d - 'a';
                else if (base == 16 && d >= 'A' && d <= 'F') dv = 10 + d - 'A';
                else break;
                if (dv >= base) break;
                any = 1;
                if (acc > (UINT64_MAX - (uint64_t)dv) / (uint64_t)base)
                    overflow = 1;          /* saturate, keep consuming */
                else
                    acc = acc * (uint64_t)base + (uint64_t)dv;
                i++;
            }
            t->num = overflow ? UINT64_MAX : acc;
            if (overflow) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "integer literal out of range", NULL, NULL, 0, 0, 0);
                diag_stage(c, 21, 1, scratch, NULL, st, i);
            }
            if (!any) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "base prefix with no digits", NULL, NULL, 0, 0, 0);
                diag_stage(c, 41, 1, scratch, NULL, st, i);
            }
            t->overflow = (uint8_t)(overflow ? 1 : 0);
            t->kind = TOK_INT; t->off = st; t->len = i - st;
            t->line = lx->line; t->col = st - lx->line_start + 1u;
            lx->pos = i;
            return;
        }
        /* punctuation */
        {
            const char *puncts = "{}[]()<>::;=,@-";
            const char *q = puncts;
            while (*q) {
                if ((uint8_t)*q == ch) break;
                q++;
            }
            if (*q) {
                t->kind = TOK_PUNCT; t->off = st; t->len = 1;
                t->line = lx->line; t->col = st - lx->line_start + 1u;
                t->ch = ch;
                lx->pos = i + 1u;
                return;
            }
        }
        /* invalid character */
        {
            char scratch[96];
            diag_fmsg(c, scratch, sizeof scratch,
                      "invalid character (byte %u)", NULL, NULL, ch, 0, 0);
            diag_stage(c, 23, 1, scratch, NULL, st, st + 1u);
            t->kind = TOK_INVALID; t->off = st; t->len = 1;
            t->line = lx->line; t->col = st - lx->line_start + 1u;
            t->ch = 0;
            lx->pos = i + 1u;
            return;
        }
    }
}

static void lex_all(weftc_ctx_t *c, const char *src, size_t src_len)
{
    Lexer lx;
    weft_tok_t t;
    uint32_t overflow_reported = 0;
    lx.c = c; lx.src = src; lx.len = src_len;
    lx.pos = 0; lx.line = 1; lx.line_start = 0;
    lx_line_start(&lx);
    for (;;) {
        uint32_t prev_len;
        lex_one(&lx, &t);
        if (c->ntoks >= WEFTC_MAX_TOKENS) {
            c->capacity_hit = 1;
            if (!overflow_reported) {
                char scratch[96];
                diag_fmsg(c, scratch, sizeof scratch,
                          "token stream exceeds the %u-token limit", NULL,
                          NULL, WEFTC_MAX_TOKENS, 0, 0);
                diag_stage(c, 0, 1, scratch, NULL, t.off, t.off + 1u);
                overflow_reported = 1;
            }
            c->toks[c->ntoks > 0 ? c->ntoks - 1u : 0].kind = TOK_EOF;
            return;
        }
        prev_len = t.len;
        c->toks[c->ntoks] = t;
        c->ntoks++;
        if (t.kind == TOK_EOF) return;
        (void)prev_len;
    }
}

/* ------------------------------------------------------------------ */
/* Parser (recursive descent, panic-mode recovery)                     */
/* ------------------------------------------------------------------ */
typedef struct {
    weftc_ctx_t *c;
    uint32_t    ti;         /* token cursor                       */
    uint32_t    root;       /* AST schema node                   */
} Parser;

static const weft_tok_t *tk_raw(Parser *p)
{
    return &p->c->toks[p->ti < p->c->ntoks ? p->ti : p->c->ntoks - 1u];
}

/* Token access with comment skipping: comments are tokens (the semantic
 * tokenizer needs them), but the grammar never admits them — every access
 * through tk() advances past comment tokens transparently. */
static const weft_tok_t *tk(Parser *p)
{
    while (p->ti < p->c->ntoks) {
        uint16_t k = p->c->toks[p->ti].kind;
        if (k != TOK_COMMENT_LINE && k != TOK_COMMENT_BLOCK) break;
        p->ti++;
    }
    return tk_raw(p);
}
static void tk_next(Parser *p)
{
    if (p->ti + 1u < p->c->ntoks) p->ti++;
}

static int tk_is_punct(const weft_tok_t *t, char ch)
{
    return t->kind == TOK_PUNCT && t->ch == (uint8_t)ch;
}

/* Compares an IDENT token's text against a keyword (source-bounded). */
static int tk_is_ident(Parser *p, const weft_tok_t *t, const char *name)
{
    uint32_t i;
    if (t->kind != TOK_IDENT) return 0;
    if (t->off + t->len > p->c->src_len) return 0;
    for (i = 0; name[i]; i++) {
        if (i >= t->len) return 0;
        if (name[i] != p->c->src[t->off + i]) return 0;
    }
    return i == t->len;
}

static uint32_t node_new(Parser *p, uint32_t kind, const weft_tok_t *t,
                         uint32_t parent, uint32_t prev_sibling)
{
    weftc_ctx_t *c = p->c;
    weft_ast_node_t *n;
    uint32_t idx;
    if (c->nnodes >= WEFTC_MAX_NODES) {
        c->capacity_hit = 1;
        return WEFT_AST_NONE;
    }
    idx = c->nnodes++;
    n = &c->nodes[idx];
    n->kind = kind;
    n->parent = parent;
    n->first_child = WEFT_AST_NONE;
    n->next_sibling = WEFT_AST_NONE;
    n->name_off = t ? t->off : 0;
    n->name_len = t ? t->len : 0;
    n->line = t ? t->line : 1;
    n->col = t ? t->col : 1;
    n->value = 0;
    if (prev_sibling != WEFT_AST_NONE && prev_sibling < c->nnodes)
        c->nodes[prev_sibling].next_sibling = idx;
    else if (parent != WEFT_AST_NONE && parent < c->nnodes)
        c->nodes[parent].first_child = idx;
    return idx;
}

/* ------------------------------------------------------------------ */
/* Attribute plumbing                                                  */
/* ------------------------------------------------------------------ */
enum { ATTR_ALIGN = 0, ATTR_SIMD = 1, ATTR_PACKED = 2, ATTR_OPTIMIZE = 3 };

typedef struct {
    uint32_t val[4];        /* align/simd values; presence flags below  */
    uint8_t present[4];
    uint32_t tok_off[4];
    uint32_t tok_len[4];
    uint32_t line[4];
    uint32_t col[4];
} AttrSet;

enum { ATTRSUB_STRUCT = 0, ATTRSUB_ENUM = 1, ATTRSUB_FIELD = 2 };

static void attr_report_dup(Parser *p, const AttrSet *a, int k)
{
    char scratch[128];
    const char *names[4] = { "align", "simd", "packed", "optimize" };
    diag_fmsg(p->c, scratch, sizeof scratch,
              "attribute `@%s` appears more than once on this subject",
              names[k], NULL, 0, 0, 0);
    diag_stage(p->c, 40, 1, scratch, NULL, a->tok_off[k],
               a->tok_off[k] + a->tok_len[k]);
}

static void attr_report_badpos(Parser *p, const weft_tok_t *t, const char *name)
{
    char scratch[128];
    diag_fmsg(p->c, scratch, sizeof scratch,
              "attribute `@%s` is not valid in this position", name, NULL,
              0, 0, 0);
    diag_stage(p->c, 6, 1, scratch, NULL, t->off, t->off + t->len);
}

/* Parses zero or more attributes into *a (subject validates positions). */
static void parse_attrs(Parser *p, AttrSet *a, int subject)
{
    weftc_ctx_t *c = p->c;
    memset(a, 0, sizeof *a);
    while (tk_is_punct(tk(p), '@')) {
        const weft_tok_t *at = tk(p);
        const weft_tok_t *nm;
        int k = -1;
        tk_next(p);
        nm = tk(p);
        if (nm->kind != TOK_IDENT) {
            char scratch[96];
            diag_fmsg(c, scratch, sizeof scratch,
                      "attribute name expected after `@`", NULL, NULL, 0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, at->off, at->off + 1u);
            return;
        }
        if (tk_is_ident(p, nm, "align")) k = ATTR_ALIGN;
        else if (tk_is_ident(p, nm, "simd")) k = ATTR_SIMD;
        else if (tk_is_ident(p, nm, "packed")) k = ATTR_PACKED;
        else if (tk_is_ident(p, nm, "optimize")) k = ATTR_OPTIMIZE;
        if (k < 0) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "unknown attribute `%s`", NULL, NULL, 0, 0, 0);
            /* name text is inside the message via manual copy below */
            {
                uint32_t i;
                char full[192];
                const char *pre = "unknown attribute `";
                uint32_t o = 0;
                while (pre[o]) { full[o] = pre[o]; o++; }
                for (i = 0; i < nm->len && o < 180u; i++)
                    full[o++] = c->src[nm->off + i];
                full[o++] = '`';
                full[o] = 0;
                diag_stage(c, 1, 1, full, NULL, nm->off, nm->off + nm->len);
            }
            tk_next(p);
            /* skip a possible argument list */
            if (tk_is_punct(tk(p), '(')) {
                int depth = 0;
                do {
                    if (tk_is_punct(tk(p), '(')) depth++;
                    else if (tk_is_punct(tk(p), ')')) depth--;
                    tk_next(p);
                } while (depth > 0 && tk(p)->kind != TOK_EOF);
            }
            continue;
        }
        /* position validity */
        if (subject == ATTRSUB_ENUM && k != ATTR_ALIGN) {
            attr_report_badpos(p, nm, k == ATTR_SIMD ? "simd"
                             : k == ATTR_PACKED ? "packed" : "optimize");
        }
        if (subject == ATTRSUB_FIELD && k == ATTR_OPTIMIZE)
            attr_report_badpos(p, nm, "optimize");

        if (a->present[k]) attr_report_dup(p, a, k);
        a->present[k] = 1;
        a->tok_off[k] = nm->off; a->tok_len[k] = nm->len;
        a->line[k] = nm->line; a->col[k] = nm->col;
        tk_next(p);

        if (k == ATTR_ALIGN || k == ATTR_SIMD) {
            const weft_tok_t *v;
            if (!tk_is_punct(tk(p), '(')) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "attribute `@%s` requires an argument",
                          k == ATTR_ALIGN ? "align" : "simd", NULL, 0, 0, 0);
                diag_stage(c, 2, 1, scratch, NULL, nm->off, nm->off + nm->len);
                continue;
            }
            tk_next(p);
            v = tk(p);
            if (v->kind != TOK_INT) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "bad attribute argument: integer expected",
                          NULL, NULL, 0, 0, 0);
                diag_stage(c, 32, 1, scratch, NULL, v->off, v->off + 1u);
                /* resync: skip to ')' */
                while (!tk_is_punct(tk(p), ')') && tk(p)->kind != TOK_EOF)
                    tk_next(p);
                if (tk_is_punct(tk(p), ')')) tk_next(p);
                continue;
            }
            {
                uint64_t av = v->num;
                if (av == 0 || (av & (av - 1u)) != 0u) {
                    char scratch[128];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "attribute `@%s` requires a power-of-two value, "
                              "got %u", k == ATTR_ALIGN ? "align" : "simd",
                              NULL, av, 0, 0);
                    diag_stage(c, 25, 1, scratch, NULL, v->off,
                               v->off + v->len);
                } else if (av > WEFTC_MAX_ALIGN) {
                    char scratch[128];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "attribute `@%s(%u)` exceeds the 4096-byte "
                              "alignment limit",
                              k == ATTR_ALIGN ? "align" : "simd", NULL, av,
                              0, 0);
                    diag_stage(c, 26, 1, scratch, NULL, v->off, v->off + v->len);
                } else {
                    a->val[k] = (uint32_t)av;
                }
            }
            tk_next(p);
            if (tk_is_punct(tk(p), ')')) tk_next(p);
        } else if (k == ATTR_OPTIMIZE) {
            if (!tk_is_punct(tk(p), '(')) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "attribute `@optimize` requires an argument",
                          NULL, NULL, 0, 0, 0);
                diag_stage(c, 2, 1, scratch, NULL, nm->off, nm->off + nm->len);
            } else {
                tk_next(p);
                if (!tk_is_ident(p, tk(p), "packing")) {
                    char scratch[128];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "bad attribute argument: `packing` expected",
                              NULL, NULL, 0, 0, 0);
                    diag_stage(c, 32, 1, scratch, NULL, tk(p)->off,
                               tk(p)->off + tk(p)->len);
                } else {
                    tk_next(p);
                }
                if (tk_is_punct(tk(p), ')')) tk_next(p);
            }
        } else { /* packed: takes no argument */
            if (tk_is_punct(tk(p), '(')) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "attribute `@packed` takes no argument", NULL, NULL,
                          0, 0, 0);
                diag_stage(c, 3, 1, scratch, NULL, nm->off, nm->off + nm->len);
                tk_next(p);
                while (!tk_is_punct(tk(p), ')') && tk(p)->kind != TOK_EOF)
                    tk_next(p);
                if (tk_is_punct(tk(p), ')')) tk_next(p);
            }
        }
    }
}

/* Emits ATTR AST nodes under `parent` (after the parent exists). */
static uint32_t attrs_emit_ast(Parser *p, const AttrSet *a, uint32_t parent,
                               uint32_t prev)
{
    static const uint32_t kinds[4] = {
        WEFT_AST_ATTR_ALIGN, WEFT_AST_ATTR_SIMD,
        WEFT_AST_ATTR_PACKED, WEFT_AST_ATTR_OPTIMIZE
    };
    int k;
    for (k = 0; k < 4; k++) {
        if (!a->present[k]) continue;
        {
            weft_tok_t fake;
            fake.kind = TOK_IDENT;
            fake.off = a->tok_off[k];
            fake.len = a->tok_len[k];
            fake.line = a->line[k];
            fake.col = a->col[k];
            fake.num = 0; fake.neg = 0; fake.overflow = 0;
            fake.ch = 0; fake.reserved0 = 0; fake.reserved1 = 0;
            prev = node_new(p, kinds[k], &fake, parent, prev);
            if (prev != WEFT_AST_NONE)
                p->c->nodes[prev].value =
                    (k == ATTR_ALIGN || k == ATTR_SIMD) ? a->val[k] : 0;
        }
    }
    return prev;
}

/* ------------------------------------------------------------------ */
/* Type expressions                                                    */
/* ------------------------------------------------------------------ */
static uint32_t parse_type(Parser *p, uint32_t parent, uint32_t prev,
                           uint32_t depth)
{
    weftc_ctx_t *c = p->c;
    const weft_tok_t *t = tk(p);
    uint32_t node;

    if (depth > WEFTC_MAX_TYPE_DEPTH) {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "type expression nesting exceeds 64 levels", NULL, NULL,
                  0, 0, 0);
        diag_stage(c, 27, 1, scratch, NULL, t->off, t->off + (t->len ? t->len : 1));
        return WEFT_AST_NONE;
    }
    if (t->kind == TOK_IDENT) {
        int prim = -1, k;
        for (k = 1; k <= 12; k++) {
            if (tk_is_ident(p, t, weft_prim_names[k])) { prim = k; break; }
        }
        if (tk_is_ident(p, t, "str")) {
            tk_next(p);
            if (!tk_is_punct(tk(p), '[')) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "expected `[` after `str`", NULL, NULL, 0, 0, 0);
                diag_stage(c, 19, 1, scratch, NULL, t->off, t->off + t->len);
                return WEFT_AST_NONE;
            }
            tk_next(p);
            if (tk(p)->kind != TOK_INT) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "expected an integer length for `str[N]`", NULL,
                          NULL, 0, 0, 0);
                diag_stage(c, 19, 1, scratch, NULL, tk(p)->off,
                           tk(p)->off + 1u);
                return WEFT_AST_NONE;
            }
            {
                weft_tok_t num = *tk(p);
                tk_next(p);
                if (tk_is_punct(tk(p), ']')) tk_next(p);
                if (num.num == 0) {
                    char scratch[128];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "fixed length must be >= 1", NULL, NULL, 0, 0, 0);
                    diag_stage(c, 12, 1, scratch, NULL, num.off,
                               num.off + num.len);
                } else if (num.num > 0xFFFFFFFFull) {
                    char scratch[128];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "fixed length exceeds the u32 limit", NULL,
                              NULL, 0, 0, 0);
                    diag_stage(c, 29, 1, scratch, NULL, num.off,
                               num.off + num.len);
                }
                node = node_new(p, WEFT_AST_TY_STR, t, parent, prev);
                if (node != WEFT_AST_NONE) c->nodes[node].value = num.num;
                return node;
            }
        }
        if (tk_is_ident(p, t, "span")) {
            uint32_t elem;
            tk_next(p);
            if (!tk_is_punct(tk(p), '<')) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "expected `<` after `span`", NULL, NULL, 0, 0, 0);
                diag_stage(c, 19, 1, scratch, NULL, t->off, t->off + t->len);
                return WEFT_AST_NONE;
            }
            tk_next(p);
            elem = parse_type(p, parent, prev, depth + 1u);
            if (tk_is_punct(tk(p), '>')) tk_next(p);
            node = node_new(p, WEFT_AST_TY_SPAN, t, parent, prev);
            if (node != WEFT_AST_NONE) {
                weft_ast_node_t *n = &c->nodes[node];
                n->first_child = (elem != WEFT_AST_NONE && c->nodes[elem].parent == parent)
                                 ? elem : WEFT_AST_NONE;
                if (elem != WEFT_AST_NONE &&
                    c->nodes[elem].kind == WEFT_AST_TY_SPAN) {
                    char scratch[128];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "span element must not be span", NULL, NULL,
                              0, 0, 0);
                    diag_stage(c, 24, 1, scratch, NULL, t->off, t->off + t->len);
                }
            }
            return node;
        }
        if (prim > 0) {
            tk_next(p);
            node = node_new(p, WEFT_AST_TY_PRIM, t, parent, prev);
            if (node != WEFT_AST_NONE) c->nodes[node].value = (uint64_t)prim;
            return node;
        }
        /* named reference */
        tk_next(p);
        return node_new(p, WEFT_AST_TY_NAMED, t, parent, prev);
    }
    if (tk_is_punct(t, '[')) {
        uint32_t elem;
        tk_next(p);
        elem = parse_type(p, parent, prev, depth + 1u);
        if (!tk_is_punct(tk(p), ';')) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "expected `;` in array type", NULL, NULL, 0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
        } else {
            tk_next(p);
        }
        if (tk(p)->kind != TOK_INT) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "expected an integer length for `[T; N]`", NULL, NULL,
                      0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
            return WEFT_AST_NONE;
        }
        {
            weft_tok_t num = *tk(p);
            tk_next(p);
            if (tk_is_punct(tk(p), ']')) tk_next(p);
            if (num.num == 0) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "fixed length must be >= 1", NULL, NULL, 0, 0, 0);
                diag_stage(c, 12, 1, scratch, NULL, num.off, num.off + num.len);
            } else if (num.num > 0xFFFFFFFFull) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "fixed length exceeds the u32 limit", NULL, NULL,
                          0, 0, 0);
                diag_stage(c, 29, 1, scratch, NULL, num.off, num.off + num.len);
            }
            node = node_new(p, WEFT_AST_TY_ARRAY, t, parent, prev);
            if (node != WEFT_AST_NONE) {
                c->nodes[node].value = num.num;
                c->nodes[node].first_child =
                    (elem != WEFT_AST_NONE && c->nodes[elem].parent == parent)
                    ? elem : WEFT_AST_NONE;
            }
            return node;
        }
    }
    {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "type expected", NULL, NULL, 0, 0, 0);
        diag_stage(c, 19, 1, scratch, NULL, t->off,
                   t->off + (t->len ? t->len : 1));
        return WEFT_AST_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* Declarations                                                        */
/* ------------------------------------------------------------------ */
static int prim_tag_of(Parser *p, const weft_tok_t *t)
{
    int k;
    if (t->kind != TOK_IDENT) return -1;
    for (k = 1; k <= 12; k++)
        if (tk_is_ident(p, t, weft_prim_names[k])) return k;
    return -1;
}

static void parse_struct_decl(Parser *p, const AttrSet *da, uint32_t decl_node,
                              weftc_decl_t *d)
{
    weftc_ctx_t *c = p->c;
    uint32_t prev_child = WEFT_AST_NONE;
    attrs_emit_ast(p, da, decl_node, prev_child);
    /* attrs become the first children; keep cursor at last attr node */
    {
        uint32_t ch = c->nodes[decl_node].first_child;
        while (ch != WEFT_AST_NONE && c->nodes[ch].next_sibling != WEFT_AST_NONE)
            ch = c->nodes[ch].next_sibling;
        prev_child = ch;
    }
    if (!tk_is_punct(tk(p), '{')) {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "expected `{` to open the struct body", NULL, NULL, 0, 0, 0);
        diag_stage(c, 19, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
        return;
    }
    tk_next(p);
    for (;;) {
        AttrSet fa;
        const weft_tok_t *nm;
        weftc_field_t *f;
        uint32_t fnode, tynode;
        if (tk_is_punct(tk(p), '}')) { tk_next(p); break; }
        if (tk(p)->kind == TOK_EOF) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "unterminated struct body: expected `}`", NULL, NULL,
                      0, 0, 0);
            diag_stage(c, 20, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
            return;
        }
        parse_attrs(p, &fa, ATTRSUB_FIELD);
        nm = tk(p);
        if (nm->kind != TOK_IDENT) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "field name expected", NULL, NULL, 0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, nm->off, nm->off + 1u);
            /* panic: skip to ',' or '}' */
            while (!tk_is_punct(tk(p), ',') && !tk_is_punct(tk(p), '}') &&
                   tk(p)->kind != TOK_EOF)
                tk_next(p);
            if (tk_is_punct(tk(p), ',')) tk_next(p);
            else if (tk_is_punct(tk(p), '}')) { tk_next(p); return; }
            continue;
        }
        tk_next(p);
        if (!tk_is_punct(tk(p), ':')) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "expected `:` after field name `%s`", NULL, NULL, 0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, nm->off, nm->off + nm->len);
        } else {
            tk_next(p);
        }
        if (c->nfields >= WEFTC_MAX_FIELDS) { c->capacity_hit = 1; return; }
        if (d->nfields >= WEFTC_MAX_FIELDS_PER_DECL) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "struct exceeds the per-decl field limit (2048)",
                      NULL, NULL, 0, 0, 0);
            diag_stage(c, 0, 1, scratch, NULL, nm->off, nm->off + nm->len);
            return;
        }
        if (d->nfields == 0) d->first_field = c->nfields;
        f = &c->fields[c->nfields];
        memset(f, 0, sizeof *f);
        f->name_off = nm->off; f->name_len = nm->len;
        f->name_pool = (uint32_t)name_pool_add(c, c->src + nm->off, nm->len);
        if ((int32_t)f->name_pool < 0) { c->capacity_hit = 1; return; }
        f->line = nm->line; f->col = nm->col;
        f->span_start = nm->off;
        f->orig_index = d->nfields;
        f->packed = fa.present[ATTR_PACKED] && !fa.present[ATTR_ALIGN] &&
                    !fa.present[ATTR_SIMD] ? 1 : 0;
        if (fa.present[ATTR_ALIGN]) {
            f->has_attr_align = 1;
            f->attr_align = fa.val[ATTR_ALIGN];
        }
        if (fa.present[ATTR_SIMD]) {
            f->has_attr_simd = 1;
            f->attr_simd = fa.val[ATTR_SIMD];
        }
        fnode = node_new(p, WEFT_AST_FIELD, nm, decl_node, prev_child);
        if (fnode != WEFT_AST_NONE) prev_child = fnode;
        attrs_emit_ast(p, &fa,
                       fnode != WEFT_AST_NONE ? fnode : decl_node,
                       WEFT_AST_NONE);
        tynode = parse_type(p, fnode != WEFT_AST_NONE ? fnode : decl_node,
                            WEFT_AST_NONE, 0);
        f->ty_node = tynode;
        {
            const weft_tok_t *after = tk(p);
            f->span_end = (after->off > nm->off) ? after->off
                          : nm->off + nm->len;
        }
        c->nfields++;
        d->nfields++;
        if (tk_is_punct(tk(p), ',')) tk_next(p);
    }
    if (tk_is_punct(tk(p), ';')) tk_next(p);
}

static void parse_enum_decl(Parser *p, const AttrSet *da, uint32_t decl_node,
                            weftc_decl_t *d, int is_bitflags)
{
    weftc_ctx_t *c = p->c;
    uint32_t prev_child = WEFT_AST_NONE;
    attrs_emit_ast(p, da, decl_node, prev_child);
    {
        uint32_t ch = c->nodes[decl_node].first_child;
        while (ch != WEFT_AST_NONE && c->nodes[ch].next_sibling != WEFT_AST_NONE)
            ch = c->nodes[ch].next_sibling;
        prev_child = ch;
    }
    if (!tk_is_punct(tk(p), ':')) {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "expected `:` and a backing type", NULL, NULL, 0, 0, 0);
        diag_stage(c, 19, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
        return;
    }
    tk_next(p);
    {
        int prim = prim_tag_of(p, tk(p));
        if (prim < 0) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "backing must be an integer primitive", NULL, NULL,
                      0, 0, 0);
            diag_stage(c, 17, 1, scratch, NULL, tk(p)->off,
                       tk(p)->off + (tk(p)->len ? tk(p)->len : 1));
        } else {
            tk_next(p);
            d->backing = (uint32_t)prim;
            if (!prim_is_int[prim]) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "backing must be an integer primitive", NULL, NULL,
                          0, 0, 0);
                diag_stage(c, 17, 1, scratch, NULL, tk(p)->off,
                           tk(p)->off + 1u);
                d->backing = 1; /* u8 default keeps layout computable */
            } else if (is_bitflags && !prim_is_uint[prim]) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "bitflags backing must be unsigned", NULL, NULL,
                          0, 0, 0);
                diag_stage(c, 16, 1, scratch, NULL, tk(p)->off,
                           tk(p)->off + 1u);
                d->backing = 1;
            }
        }
    }
    if (!tk_is_punct(tk(p), '{')) {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "expected `{` to open the body", NULL, NULL, 0, 0, 0);
        diag_stage(c, 19, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
        return;
    }
    tk_next(p);
    for (;;) {
        const weft_tok_t *nm;
        weftc_variant_t *v;
        if (tk_is_punct(tk(p), '}')) { tk_next(p); break; }
        if (tk(p)->kind == TOK_EOF) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "unterminated body: expected `}`", NULL, NULL, 0, 0, 0);
            diag_stage(c, 20, 1, scratch, NULL, tk(p)->off, tk(p)->off + 1u);
            return;
        }
        nm = tk(p);
        if (nm->kind != TOK_IDENT) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "variant name expected", NULL, NULL, 0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, nm->off, nm->off + 1u);
            while (!tk_is_punct(tk(p), ',') && !tk_is_punct(tk(p), '}') &&
                   tk(p)->kind != TOK_EOF)
                tk_next(p);
            if (tk_is_punct(tk(p), ',')) tk_next(p);
            else if (tk_is_punct(tk(p), '}')) { tk_next(p); return; }
            continue;
        }
        tk_next(p);
        if (!tk_is_punct(tk(p), '=')) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "expected `=` and a value for variant `%s`", NULL, NULL,
                      0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, nm->off, nm->off + nm->len);
            continue;
        }
        tk_next(p);
        {
            int neg = 0;
            uint64_t raw;
            int64_t value;
            if (tk_is_punct(tk(p), '-')) { neg = 1; tk_next(p); }
            if (tk(p)->kind != TOK_INT) {
                char scratch[128];
                diag_fmsg(c, scratch, sizeof scratch,
                          "variant value expected", NULL, NULL, 0, 0, 0);
                diag_stage(c, 19, 1, scratch, NULL, tk(p)->off,
                           tk(p)->off + 1u);
                continue;
            }
            raw = tk(p)->num;
            tk_next(p);
            if (neg) {
                if (raw > (uint64_t)INT64_MAX + 1u)
                    value = INT64_MIN;
                else if (raw == (uint64_t)INT64_MAX + 1u)
                    value = INT64_MIN;
                else
                    value = -(int64_t)raw;
            } else {
                value = (raw > (uint64_t)INT64_MAX)
                        ? (int64_t)(raw - (uint64_t)INT64_MAX - 1u) - INT64_MAX - 1
                        : (int64_t)raw;
            }
            if (c->nvariants >= WEFTC_MAX_VARIANTS) { c->capacity_hit = 1; return; }
            if (d->nvariants == 0) d->first_variant = c->nvariants;
            v = &c->variants[c->nvariants];
            memset(v, 0, sizeof *v);
            v->name_off = nm->off; v->name_len = nm->len;
            v->name_pool = (uint32_t)name_pool_add(c, c->src + nm->off,
                                                   nm->len);
            if ((int32_t)v->name_pool < 0) { c->capacity_hit = 1; return; }
            v->line = nm->line; v->col = nm->col;
            v->span_start = nm->off;
            v->span_end = tk(p)->off > nm->off ? tk(p)->off
                          : nm->off + nm->len;
            v->value = value;
            {
                uint32_t vnode = node_new(p, WEFT_AST_VARIANT, nm, decl_node,
                                          prev_child);
                if (vnode != WEFT_AST_NONE) {
                    prev_child = vnode;
                    c->nodes[vnode].value = (uint64_t)value;
                }
            }
            c->nvariants++;
            d->nvariants++;
            if (tk_is_punct(tk(p), ',')) tk_next(p);
        }
    }
    if (d->nvariants == 0) {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "enum/bitflags with no variants", NULL, NULL, 0, 0, 0);
        diag_stage(c, 14, 1, scratch, NULL, d->span_start,
                   d->span_start + 1u);
    }
    if (tk_is_punct(tk(p), ';')) tk_next(p);
}

static void parse_decl(Parser *p, const AttrSet *da, int has_attrs)
{
    weftc_ctx_t *c = p->c;
    const weft_tok_t *kw = tk(p);
    int is_enum = 0, is_bitflags = 0;
    uint32_t decl_node;
    weftc_decl_t *d;
    uint32_t kind;
    (void)has_attrs;

    if (tk_is_ident(p, kw, "struct")) kind = WEFT_AST_DECL_STRUCT;
    else if (tk_is_ident(p, kw, "enum")) { kind = WEFT_AST_DECL_ENUM; is_enum = 1; }
    else if (tk_is_ident(p, kw, "bitflags")) {
        kind = WEFT_AST_DECL_BITFLAGS; is_bitflags = 1;
    } else {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "declaration keyword expected", NULL, NULL, 0, 0, 0);
        diag_stage(c, 19, 1, scratch, NULL, kw->off,
                   kw->off + (kw->len ? kw->len : 1));
        return;
    }
    tk_next(p);
    if (c->ndecls >= WEFTC_MAX_DECLS) {
        c->capacity_hit = 1;
        return;
    }
    {
        const weft_tok_t *nm = tk(p);
        if (nm->kind != TOK_IDENT) {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "declaration name expected", NULL, NULL, 0, 0, 0);
            diag_stage(c, 19, 1, scratch, NULL, nm->off, nm->off + 1u);
            return;
        }
        tk_next(p);
        d = &c->decls[c->ndecls];
        memset(d, 0, sizeof *d);
        d->kind = (uint8_t)(is_enum ? DCL_ENUM
                          : is_bitflags ? DCL_BITFLAGS : DCL_STRUCT);
        d->name_off = nm->off; d->name_len = nm->len;
        d->name_pool = (uint32_t)name_pool_add(c, c->src + nm->off, nm->len);
        if ((int32_t)d->name_pool < 0) { c->capacity_hit = 1; return; }
        d->line = nm->line; d->col = nm->col;
        d->kw_line = kw->line; d->kw_col = kw->col;
        d->span_start = kw->off;
        d->backing = 1; /* u8 default when the backing is malformed */
        if (da->present[ATTR_ALIGN]) {
            d->has_align_attr = 1;
            d->align_attr = (uint16_t)da->val[ATTR_ALIGN];
        }
        if (da->present[ATTR_SIMD]) {
            d->has_simd_attr = 1;
            d->simd_attr = (uint16_t)da->val[ATTR_SIMD];
        }
        if (da->present[ATTR_PACKED]) d->packed = 1;
        decl_node = node_new(p, kind, nm, p->root, WEFT_AST_NONE);
        if (decl_node == WEFT_AST_NONE) {
            c->ndecls++; /* decl table still registered */
            if (d->kind == DCL_STRUCT) parse_struct_decl(p, da, 0, d);
            else parse_enum_decl(p, da, 0, d, is_bitflags);
            d->span_end = tk(p)->off > kw->off ? tk(p)->off : kw->off + kw->len;
            return;
        }
        c->ndecls++;
        if (d->kind == DCL_STRUCT) parse_struct_decl(p, da, decl_node, d);
        else parse_enum_decl(p, da, decl_node, d, is_bitflags);
        d->span_end = tk(p)->off > kw->off ? tk(p)->off : kw->off + kw->len;
        if (da->present[ATTR_OPTIMIZE] && d->kind == DCL_STRUCT) {
            if (d->packed) {
                char scratch[160];
                diag_fmsg(c, scratch, sizeof scratch,
                          "@packed + @optimize(packing) is the identity",
                          NULL, NULL, 0, 0, 0);
                diag_stage(c, 901, 2, scratch,
                           "remove one of the two attributes",
                           d->span_start, d->span_start + 6u);
            } else {
                d->reordered = 1;
            }
        }
    }
}

/* Panic-mode resync: advance to ';' (consumed), a declaration keyword,
 * or EOF. */
static void skip_to_sync(Parser *p)
{
    for (;;) {
        const weft_tok_t *t = tk(p);
        if (t->kind == TOK_EOF) return;
        if (tk_is_punct(t, ';')) { tk_next(p); return; }
        if (t->kind == TOK_IDENT &&
            (tk_is_ident(p, t, "struct") || tk_is_ident(p, t, "enum") ||
             tk_is_ident(p, t, "bitflags") || tk_is_ident(p, t, "endianness")))
            return;
        tk_next(p);
    }
}

static void parse_endianness(Parser *p)
{
    weftc_ctx_t *c = p->c;
    const weft_tok_t *kw = tk(p);
    uint32_t node;
    tk_next(p);
    if (c->endianness_declared) {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "endianness redeclared", NULL, NULL, 0, 0, 0);
        diag_stage(c, 18, 1, scratch, NULL, kw->off, kw->off + kw->len);
    } else {
        c->endianness_declared = 1;
    }
    if (tk_is_ident(p, tk(p), "little")) {
        tk_next(p);
    } else if (tk_is_ident(p, tk(p), "big")) {
        c->endian_big = 1;
        tk_next(p);
    } else {
        char scratch[128];
        diag_fmsg(c, scratch, sizeof scratch,
                  "endianness must be little or big", NULL, NULL, 0, 0, 0);
        diag_stage(c, 28, 1, scratch, NULL, tk(p)->off,
                   tk(p)->off + (tk(p)->len ? tk(p)->len : 1));
    }
    if (tk_is_punct(tk(p), ';')) tk_next(p);
    node = node_new(p, WEFT_AST_ENDIANNESS, kw, p->root, WEFT_AST_NONE);
    if (node != WEFT_AST_NONE) c->nodes[node].value = c->endian_big;
}

static void parse_schema(Parser *p)
{
    weftc_ctx_t *c = p->c;
    weft_tok_t root_tok;
    root_tok.kind = TOK_EOF;
    root_tok.off = 0; root_tok.len = 0;
    root_tok.line = 1; root_tok.col = 1;
    root_tok.num = 0; root_tok.neg = 0; root_tok.overflow = 0;
    root_tok.ch = 0; root_tok.reserved0 = 0; root_tok.reserved1 = 0;
    p->root = node_new(p, WEFT_AST_SCHEMA, &root_tok, WEFT_AST_NONE,
                       WEFT_AST_NONE);
    for (;;) {
        const weft_tok_t *t = tk(p);
        AttrSet da;
        int had_attrs = 0;
        if (t->kind == TOK_EOF) return;
        if (t->kind == TOK_IDENT && tk_is_ident(p, t, "endianness")) {
            parse_endianness(p);
            continue;
        }
        if (tk_is_punct(t, '@')) {
            parse_attrs(p, &da, ATTRSUB_STRUCT);
            had_attrs = 1;
        } else {
            memset(&da, 0, sizeof da);
        }
        t = tk(p);
        if (t->kind == TOK_EOF) return;
        if (t->kind == TOK_IDENT &&
            (tk_is_ident(p, t, "struct") || tk_is_ident(p, t, "enum") ||
             tk_is_ident(p, t, "bitflags"))) {
            parse_decl(p, &da, had_attrs);
            continue;
        }
        {
            char scratch[128];
            diag_fmsg(c, scratch, sizeof scratch,
                      "unexpected token", NULL, NULL, 0, 0, 0);
            diag_stage(c, 20, 1, scratch, NULL, t->off,
                       t->off + (t->len ? t->len : 1));
        }
        skip_to_sync(p);
    }
}

/* ------------------------------------------------------------------ */
/* Semantic analysis + the deterministic layout engine (RFC-0017 §4)   */
/* ------------------------------------------------------------------ */
static const char *decl_name_ptr(const weftc_ctx_t *c, const weftc_decl_t *d)
{
    return c->name_pool + d->name_pool;
}
static const char *field_name_ptr(const weftc_ctx_t *c, const weftc_field_t *f)
{
    return c->name_pool + f->name_pool;
}

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    if (a <= 1) return v;
    return (v + a - 1u) & ~(a - 1u);
}

/* Levenshtein distance, hard-capped (suggestions only, 64-byte names). */
static size_t edit_distance(const char *a, size_t la, const char *b, size_t lb)
{
    size_t prev[65], cur[65];
    size_t i, j;
    if (la > 64 || lb > 64) return SIZE_MAX;
    if (la == 0) return lb;
    if (lb == 0) return la;
    for (j = 0; j <= lb; j++) prev[j] = j;
    for (i = 1; i <= la; i++) {
        cur[0] = i;
        for (j = 1; j <= lb; j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            size_t m = prev[j - 1] + cost;
            if (prev[j] + 1 < m) m = prev[j] + 1;
            if (cur[j - 1] + 1 < m) m = cur[j - 1] + 1;
            cur[j] = m;
        }
        memcpy(prev, cur, (lb + 1) * sizeof(size_t));
    }
    return prev[lb];
}

/* "did you mean" over declared names then primitive names. */
static void suggest_type(weftc_ctx_t *c, const char *name, size_t len)
{
    size_t best = 3; /* only suggest distance <= 2 */
    const char *best_name = NULL;
    size_t best_len = 0;
    uint32_t i;
    int k;
    for (i = 0; i < c->ndecls; i++) {
        const char *cand = decl_name_ptr(c, &c->decls[i]);
        size_t cl = 0;
        while (cand[cl]) cl++;
        {
            size_t d = edit_distance(name, len, cand, cl);
            if (d < best) { best = d; best_name = cand; best_len = cl; }
        }
    }
    for (k = 1; k <= 12; k++) {
        size_t d = edit_distance(name, len, weft_prim_names[k], 2);
        if (d < best) {
            best = d;
            best_name = weft_prim_names[k];
            best_len = 2;
            while (best_name[best_len]) best_len++;
        }
    }
    if (best_name && best_len < 150u) {
        char *fix = c->pending_fix;
        const char *fp = "did you mean `";
        uint32_t fo = 0;
        uint32_t fn = 0;
        while (fp[fo]) { fix[fo] = fp[fo]; fo++; }
        while (fn < best_len && fo < 186u) fix[fo++] = best_name[fn++];
        fix[fo++] = '`';
        fix[fo++] = '?';
        fix[fo] = 0;
        c->pending_fix_valid = 1;
    }
}

/* Sorted name index (insertion sort — deterministic, small n). */
static void build_name_index(weftc_ctx_t *c)
{
    uint32_t i;
    for (i = 0; i < c->ndecls; i++) c->by_name[i] = i;
    {
        uint32_t j;
        for (i = 1; i < c->ndecls; i++) {
            uint32_t v = c->by_name[i];
            j = i;
            while (j > 0) {
                const char *a = decl_name_ptr(c, &c->decls[c->by_name[j - 1]]);
                const char *b = decl_name_ptr(c, &c->decls[v]);
                int cmp = 0;
                size_t k = 0;
                for (;;) {
                    char ca = a[k], cb = b[k];
                    if (ca != cb || ca == 0) { cmp = (unsigned char)ca < (unsigned char)cb ? -1
                                                   : (unsigned char)ca > (unsigned char)cb ? 1 : 0;
                        break; }
                    k++;
                }
                if (cmp <= 0) break;
                c->by_name[j] = c->by_name[j - 1];
                j--;
            }
            c->by_name[j] = v;
        }
    }
    for (i = 1; i < c->ndecls; i++) {
        const char *a = decl_name_ptr(c, &c->decls[c->by_name[i - 1]]);
        const char *b = decl_name_ptr(c, &c->decls[c->by_name[i]]);
        int same = 0;
        size_t k = 0;
        for (;;) {
            if (a[k] != b[k]) break;
            if (a[k] == 0) { same = 1; break; }
            k++;
        }
        if (same) {
            weftc_decl_t *dup = &c->decls[c->by_name[i]];
            char scratch[192];
            const char *pre = "duplicate declaration name `";
            uint32_t o = 0, n = 0;
            const char *nm = decl_name_ptr(c, dup);
            while (pre[o]) { scratch[o] = pre[o]; o++; }
            while (nm[n] && o < 180u) scratch[o++] = nm[n++];
            scratch[o++] = '`';
            scratch[o] = 0;
            diag_stage(c, 5, 1, scratch, NULL, dup->span_start,
                       dup->span_start + dup->name_len);
            dup->poisoned = 1;
        }
    }
}

/* Binary search: decl index by name, -1 when absent. */
static int32_t find_decl(weftc_ctx_t *c, const char *name, size_t len)
{
    uint32_t lo = 0, hi = c->ndecls;
    if (len > 255u) return -1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const char *cand = decl_name_ptr(c, &c->decls[c->by_name[mid]]);
        int r = 0;
        size_t k = 0;
        for (;;) {
            char ca = cand[k];
            char cb = (k < len) ? name[k] : 0;
            if (ca != cb) { r = (unsigned char)ca < (unsigned char)cb ? -1 : 1; break; }
            if (ca == 0) { r = 0; break; }
            k++;
        }
        if (r == 0) return (int32_t)c->by_name[mid];
        if (r < 0) lo = mid + 1u;
        else hi = mid;
    }
    return -1;
}

/* ---- type geometry -------------------------------------------------- */
typedef struct {
    uint64_t size, align;
    uint32_t tag;      /* weft_type_tag (reflection)          */
    uint32_t decl_idx; /* for named refs; UINT32_MAX else     */
    int      ok;
} Geom;

static int layout_decl(weftc_ctx_t *c, uint32_t i, uint32_t depth);

static Geom ty_geom(weftc_ctx_t *c, uint32_t node_idx, uint32_t depth)
{
    Geom g;
    g.size = 0; g.align = 1; g.tag = 0; g.decl_idx = UINT32_MAX; g.ok = 0;
    if (node_idx == WEFT_AST_NONE || node_idx >= c->nnodes) return g;
    {
        const weft_ast_node_t *n = &c->nodes[node_idx];
        switch (n->kind) {
        case WEFT_AST_TY_PRIM: {
            uint32_t tag = (uint32_t)n->value;
            if (tag < 1 || tag > 12u) return g;
            g.size = weft_prim_sizes[tag];
            g.align = weft_prim_aligns[tag];
            g.tag = tag;
            g.ok = 1;
            return g;
        }
        case WEFT_AST_TY_STR:
            g.size = n->value; g.align = 1;
            g.tag = WEFT_TY_STR;
            g.ok = (n->value >= 1 && n->value <= 0xFFFFFFFFull);
            return g;
        case WEFT_AST_TY_SPAN:
            g.size = WEFTC_SPAN_SIZE; g.align = WEFTC_SPAN_ALIGN;
            g.tag = WEFT_TY_SPAN;
            g.ok = 1;
            return g;
        case WEFT_AST_TY_ARRAY: {
            Geom el = ty_geom(c, n->first_child, depth);
            if (!el.ok) return g;
            if (el.size != 0 && n->value > WEFTC_MAX_TOTAL_BYTES / el.size) {
                char scratch[160];
                diag_fmsg(c, scratch, sizeof scratch,
                          "array `[...; %u]` exceeds the 2^48-byte total "
                          "size limit", NULL, NULL, n->value, 0, 0);
                diag_stage(c, 33, 1, scratch, NULL, n->name_off,
                           n->name_off + (n->name_len ? n->name_len : 1));
                return g;
            }
            g.size = el.size * n->value;
            g.align = el.align;
            g.tag = WEFT_TY_ARRAY;
            g.ok = 1;
            return g;
        }
        case WEFT_AST_TY_NAMED: {
            int32_t di = find_decl(c, c->src + n->name_off, n->name_len);
            if (di < 0) {
                char full[256];
                const char *pre = "unknown type `";
                uint32_t o = 0, k = 0;
                while (pre[o]) { full[o] = pre[o]; o++; }
                while (k < n->name_len && o < 240u)
                    full[o++] = c->src[n->name_off + k++];
                full[o++] = '`';
                full[o] = 0;
                c->pending_fix_valid = 0;
                suggest_type(c, c->src + n->name_off, n->name_len);
                diag_stage(c, 7, 1, full,
                           c->pending_fix_valid ? c->pending_fix : NULL,
                           n->name_off, n->name_off + n->name_len);
                c->pending_fix_valid = 0;
                return g;
            }
            if (!layout_decl(c, (uint32_t)di, depth + 1u)) return g;
            {
                const weftc_decl_t *d = &c->decls[di];
                g.size = d->size;
                g.align = d->align;
                g.decl_idx = (uint32_t)di;
                g.tag = (d->kind == DCL_STRUCT) ? WEFT_TY_STRUCT
                      : (d->kind == DCL_ENUM) ? WEFT_TY_ENUM : WEFT_TY_BITFLAGS;
                g.ok = d->poisoned ? 0 : 1;
                if (d->poisoned) { g.size = 0; g.align = 1; }
            }
            return g;
        }
        default:
            return g;
        }
    }
}

/* Canonical type string (RFC-0017 §3.3) into the name pool (interned,
 * NUL-terminated). */
static int32_t tystr_build(weftc_ctx_t *c, uint32_t node_idx, int depth)
{
    char num[24];
    uint32_t numlen;
    const weft_ast_node_t *n;
    if (node_idx == WEFT_AST_NONE || node_idx >= c->nnodes) return -1;
    n = &c->nodes[node_idx];
    if (depth > 70) { c->capacity_hit = 1; return -1; }
    switch (n->kind) {
    case WEFT_AST_TY_PRIM: {
        const char *nm = weft_prim_name((uint32_t)n->value);
        uint32_t len = 0;
        if (!nm) return -1;
        while (nm[len]) len++;
        return name_pool_add(c, nm, len);
    }
    case WEFT_AST_TY_NAMED:
        return name_pool_add(c, c->src + n->name_off, n->name_len);
    case WEFT_AST_TY_STR: {
        char out[32];
        uint32_t o = 4, j;
        uint64_t v = n->value;
        out[0] = 's'; out[1] = 't'; out[2] = 'r'; out[3] = '[';
        if (v == 0) out[o++] = '0';
        else {
            numlen = 0;
            while (v) { num[numlen++] = (char)('0' + (v % 10)); v /= 10; }
            for (j = numlen; j > 0 && o < 30u; j--) out[o++] = num[j - 1];
        }
        out[o++] = ']';
        return name_pool_add(c, out, o);
    }
    case WEFT_AST_TY_SPAN: {
        char out[288];
        uint32_t o = 5;
        const char *es;
        uint32_t el = 0;
        int32_t elem = tystr_build(c, n->first_child, depth + 1);
        out[0] = 's'; out[1] = 'p'; out[2] = 'a'; out[3] = 'n'; out[4] = '<';
        if (elem < 0) return -1;
        es = c->name_pool + elem;
        while (es[el] && el < 280u) { out[o++] = es[el]; el++; }
        out[o++] = '>';
        return name_pool_add(c, out, o);
    }
    case WEFT_AST_TY_ARRAY: {
        char out[288];
        uint32_t o = 1, j;
        uint64_t v = n->value;
        const char *es;
        uint32_t el = 0;
        int32_t elem = tystr_build(c, n->first_child, depth + 1);
        if (elem < 0) return -1;
        out[0] = '[';
        es = c->name_pool + elem;
        while (es[el] && el < 250u) { out[o++] = es[el]; el++; }
        out[o++] = ';'; out[o++] = ' ';
        if (v == 0) out[o++] = '0';
        else {
            numlen = 0;
            while (v) { num[numlen++] = (char)('0' + (v % 10)); v /= 10; }
            for (j = numlen; j > 0 && o < 284u; j--) out[o++] = num[j - 1];
        }
        out[o++] = ']';
        return name_pool_add(c, out, o);
    }
    default:
        return -1;
    }
}

/* ---- struct layout --------------------------------------------------- */
static uint64_t plan_walk_size(weftc_ctx_t *c, const uint32_t *plan, uint32_t n,
                                uint64_t *internal_pad)
{
    uint64_t cursor = 0;
    uint64_t pad = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        const weftc_field_t *f = &c->fields[plan[i]];
        uint64_t off = align_up_u64(cursor, f->align ? f->align : 1u);
        if (off > cursor) pad += off - cursor;
        cursor = off + f->size;
    }
    *internal_pad = pad;
    return cursor; /* sizes are checked < 2^48 upstream */
}

static int layout_decl(weftc_ctx_t *c, uint32_t i, uint32_t depth)
{
    weftc_decl_t *d = &c->decls[i];
    if (c->dstate[i] == 2) return d->poisoned ? 0 : 1;
    if (c->dstate[i] == 1) {
        /* containment cycle (WE008) — report on the closing decl */
        char scratch[192];
        const char *pre = "recursive value type: `";
        const char *nm = decl_name_ptr(c, d);
        uint32_t o = 0, k = 0;
        while (pre[o]) { scratch[o] = pre[o]; o++; }
        while (nm[k] && o < 180u) scratch[o++] = nm[k++];
        scratch[o++] = '`';
        scratch[o] = 0;
        diag_stage(c, 8, 1, scratch,
                   "use `span<T>` for references — a span never recurses",
                   d->span_start, d->span_start + d->name_len);
        d->poisoned = 1;
        return 0;
    }
    if (depth > WEFTC_MAX_NEST_DEPTH) {
        char scratch[160];
        diag_fmsg(c, scratch, sizeof scratch,
                  "struct containment exceeds 256 levels", NULL, NULL, 0, 0, 0);
        diag_stage(c, 38, 1, scratch, NULL, d->span_start,
                   d->span_start + d->name_len);
        d->poisoned = 1;
        c->dstate[i] = 2;
        return 0;
    }
    c->dstate[i] = 1;
    if (d->kind != DCL_STRUCT) {
        /* enum / bitflags: geometry from the backing primitive */
        uint64_t sz = weft_prim_sizes[d->backing ? d->backing : 1];
        uint64_t al = weft_prim_aligns[d->backing ? d->backing : 1];
        if (d->has_align_attr && (uint64_t)d->align_attr > al)
            al = d->align_attr;
        d->size = sz; d->align = al;
        d->internal_pad = 0; d->trailing_pad = 0; d->optimize_hint = 0;
        c->dstate[i] = 2;
        return d->poisoned ? 0 : 1;
    }

    /* struct */
    {
        uint32_t nf = d->nfields;
        uint32_t fi;
        uint64_t cursor = 0;
        uint64_t struct_align = 1;
        uint64_t internal_pad = 0;
        int size_error = 0;

        /* pass 1: geometry + effective alignments */
        for (fi = 0; fi < nf && fi < WEFTC_MAX_FIELDS_PER_DECL; fi++) {
            weftc_field_t *f = &c->fields[d->first_field + fi];
            Geom g = ty_geom(c, f->ty_node, depth);
            uint64_t eff;
            f->size = 0; f->align = 1; f->bad = 1;
            if (!g.ok) continue;
            if (d->packed && (f->has_attr_align || f->has_attr_simd)) {
                char scratch[176];
                const char *nm = field_name_ptr(c, f);
                const char *pre = "contradictory alignment request on `";
                uint32_t o = 0, k = 0;
                while (pre[o]) { scratch[o] = pre[o]; o++; }
                while (nm[k] && o < 160u) scratch[o++] = nm[k++];
                scratch[o++] = '`';
                scratch[o] = 0;
                diag_stage(c, 9, 1, scratch,
                           "remove @packed or the field alignment attribute",
                           f->span_start, f->span_start + f->name_len);
                continue; /* field stays bad */
            }
            if (f->packed && (f->has_attr_align || f->has_attr_simd)) {
                char scratch[176];
                const char *nm = field_name_ptr(c, f);
                const char *pre = "contradictory alignment request on `";
                uint32_t o = 0, k = 0;
                while (pre[o]) { scratch[o] = pre[o]; o++; }
                while (nm[k] && o < 160u) scratch[o++] = nm[k++];
                scratch[o++] = '`';
                scratch[o] = 0;
                diag_stage(c, 9, 1, scratch,
                           "remove @packed or the field alignment attribute",
                           f->span_start, f->span_start + f->name_len);
                continue;
            }
            if (d->packed) {
                eff = 1;
            } else if (f->packed) {
                eff = 1;
            } else {
                eff = g.align;
                if (f->has_attr_align) {
                    if ((uint64_t)f->attr_align < g.align) {
                        char scratch[192];
                        const char *nm = field_name_ptr(c, f);
                        const char *pre = "field `";
                        uint32_t o = 0, k = 0;
                        while (pre[o]) { scratch[o] = pre[o]; o++; }
                        while (nm[k] && o < 160u) scratch[o++] = nm[k++];
                        {
                            const char *mid = "`: @align(";
                            uint32_t m = 0;
                            while (mid[m] && o < 176u) scratch[o++] = mid[m++];
                        }
                        {
                            uint64_t v = f->attr_align;
                            char tmp[8];
                            uint32_t tn = 0, j;
                            if (v == 0) tmp[tn++] = '0';
                            else while (v) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
                            for (j = tn; j > 0 && o < 184u; j--)
                                scratch[o++] = tmp[j - 1];
                        }
                        {
                            const char *mid = ") is below its natural alignment";
                            uint32_t m = 0;
                            while (mid[m] && o < 188u) scratch[o++] = mid[m++];
                        }
                        scratch[o] = 0;
                        diag_stage(c, 30, 1, scratch, NULL, f->span_start,
                                   f->span_start + f->name_len);
                    } else {
                        eff = f->attr_align;
                    }
                }
                if (f->has_attr_simd && g.size >= f->attr_simd) {
                    if ((uint64_t)f->attr_simd > eff) eff = f->attr_simd;
                }
                if (d->has_simd_attr && g.size >= d->simd_attr) {
                    if ((uint64_t)d->simd_attr > eff) eff = d->simd_attr;
                }
            }
            if (eff > struct_align) struct_align = eff;
            f->size = g.size;
            f->align = eff;
            f->align = f->align ? f->align : 1u;
            f->ty_tag = g.tag;
            f->decl_ref = (g.decl_idx == UINT32_MAX) ? UINT32_MAX : g.decl_idx;
            {
                int32_t ts = tystr_build(c, f->ty_node, 0);
                f->tystr_pool = (ts < 0) ? UINT32_MAX : (uint32_t)ts;
                if (ts < 0) c->capacity_hit = 1;
            }
            f->bad = 0;
        }
        if (d->has_align_attr && (uint64_t)d->align_attr > struct_align)
            struct_align = d->align_attr;
        if (d->has_simd_attr && (uint64_t)d->simd_attr > struct_align)
            struct_align = d->simd_attr;
        if (d->packed) struct_align = 1;
        if (d->packed && d->has_align_attr && d->align_attr > 1) {
            char scratch[176];
            diag_fmsg(c, scratch, sizeof scratch,
                      "contradictory alignment request: @packed struct with "
                      "@align(N>1)", NULL, NULL, 0, 0, 0);
            diag_stage(c, 9, 1, scratch, "remove one of the two attributes",
                       d->span_start, d->span_start + 6u);
            struct_align = 1;
        }

        /* plan in declaration order */
        {
            uint32_t j = 0;
            for (fi = 0; fi < nf && fi < WEFTC_MAX_FIELDS_PER_DECL; fi++) {
                if (!c->fields[d->first_field + fi].bad) {
                    c->plan_field[j] = d->first_field + fi;
                    c->plan_align[j] = c->fields[d->first_field + fi].align;
                    j++;
                }
            }
            /* @optimize(packing): stable insertion sort by align desc */
            if (d->reordered && j > 1) {
                uint32_t a;
                for (a = 1; a < j; a++) {
                    uint32_t vf = c->plan_field[a];
                    uint64_t va = c->plan_align[a];
                    uint32_t vo = c->fields[vf].orig_index;
                    uint32_t b = a;
                    while (b > 0) {
                        uint32_t pf = c->plan_field[b - 1];
                        uint64_t pa = c->plan_align[b - 1];
                        uint32_t po = c->fields[pf].orig_index;
                        if (pa > va || (pa == va && po <= vo)) break;
                        c->plan_field[b] = pf;
                        c->plan_align[b] = pa;
                        b--;
                    }
                    c->plan_field[b] = vf;
                    c->plan_align[b] = va;
                }
            }
            /* optimize hint on the unoptimized twin */
            if (!d->reordered && !d->packed && j > 1) {
                uint64_t now, opt_size;
                uint64_t sz_now, sz_opt;
                uint64_t pad_now = 0, pad_opt = 0;
                uint32_t tmp_field[256];
                uint64_t tmp_align[256];
                uint32_t m = (j < 256u) ? j : 256u;
                uint32_t a;
                now = plan_walk_size(c, c->plan_field, j, &pad_now);
                memcpy(tmp_field, c->plan_field, m * sizeof(uint32_t));
                memcpy(tmp_align, c->plan_align, m * sizeof(uint64_t));
                for (a = 1; a < m; a++) {
                    uint32_t vf = tmp_field[a];
                    uint64_t va = tmp_align[a];
                    uint32_t vo = c->fields[vf].orig_index;
                    uint32_t b = a;
                    while (b > 0) {
                        uint32_t pf = tmp_field[b - 1];
                        uint64_t pa = tmp_align[b - 1];
                        uint32_t po = c->fields[pf].orig_index;
                        if (pa > va || (pa == va && po <= vo)) break;
                        tmp_field[b] = pf;
                        tmp_align[b] = pa;
                        b--;
                    }
                    tmp_field[b] = vf;
                    tmp_align[b] = va;
                }
                opt_size = plan_walk_size(c, tmp_field, m, &pad_opt);
                sz_now = align_up_u64(now, struct_align);
                sz_opt = align_up_u64(opt_size, struct_align);
                d->optimize_hint = (sz_opt < sz_now) ? (sz_now - sz_opt) : 0;
                (void)pad_now; (void)pad_opt;
            }

            /* persist the final order for manifests / reflection */
            d->nplanned = j;
            {
                uint32_t a;
                for (a = 0; a < j; a++)
                    c->field_order[d->first_field + a] = c->plan_field[a];
            }

            /* final cursor walk (holes + offsets) */
            d->hole_start = c->nholes;
            d->hole_count = 0;
            for (fi = 0; fi < j; fi++) {
                weftc_field_t *f = &c->fields[c->plan_field[fi]];
                uint64_t off = align_up_u64(cursor, f->align);
                if (off > cursor) {
                    if (c->nholes < WEFTC_MAX_HOLES) {
                        c->holes[c->nholes].offset = cursor;
                        c->holes[c->nholes].size = off - cursor;
                        c->nholes++;
                        d->hole_count++;
                    } else {
                        c->capacity_hit = 1;
                    }
                    internal_pad += off - cursor;
                }
                f->offset = off;
                f->padding_after = 0; /* patched to next hole below */
                cursor = off + f->size;
                if (cursor > WEFTC_MAX_TOTAL_BYTES) {
                    char scratch[160];
                    diag_fmsg(c, scratch, sizeof scratch,
                              "struct exceeds the 2^48-byte total size limit",
                              NULL, NULL, 0, 0, 0);
                    diag_stage(c, 33, 1, scratch, NULL, d->span_start,
                               d->span_start + d->name_len);
                    size_error = 1;
                    break;
                }
            }
            /* padding_after per field = distance to the next field */
            {
                uint32_t a;
                for (a = 0; a + 1 < j; a++) {
                    weftc_field_t *f = &c->fields[c->plan_field[a]];
                    weftc_field_t *nx = &c->fields[c->plan_field[a + 1]];
                    f->padding_after = (nx->offset > f->offset + f->size)
                                       ? nx->offset - f->offset - f->size : 0;
                }
            }
            {
                uint64_t size = size_error ? cursor
                              : align_up_u64(cursor, struct_align);
                d->size = size;
                d->trailing_pad = (cursor < size) ? size - cursor : 0;
                d->internal_pad = internal_pad;
                d->align = struct_align ? struct_align : 1u;
            }
        }
    }
    c->dstate[i] = 2;
    return d->poisoned ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Variant checks (enum/bitflags)                                      */
/* ------------------------------------------------------------------ */
static int variant_fits(uint32_t backing, int64_t v, int is_bitflags)
{
    if (is_bitflags && v < 0) return 0;
    switch (backing) {
    case 1: return v >= 0 && v <= 255;
    case 2: return v >= -128 && v <= 127;
    case 3: return v >= 0 && v <= 65535;
    case 4: return v >= -32768 && v <= 32767;
    case 5: return v >= 0 && v <= 4294967295;
    case 6: return v >= -2147483648 && v <= 2147483647;
    case 7: return 1;  /* u64: full unsigned range via two's complement */
    case 8: return 1;  /* i64: full signed range */
    default: return 0;
    }
}

static void check_variants(weftc_ctx_t *c)
{
    uint32_t di;
    for (di = 0; di < c->ndecls; di++) {
        weftc_decl_t *d = &c->decls[di];
        int is_bitflags = (d->kind == DCL_BITFLAGS);
        uint32_t a, b;
        if (d->kind == DCL_STRUCT) {
            /* duplicate field names within a struct (WE004) */
            for (a = 0; a < d->nfields; a++) {
                weftc_field_t *fa_ = &c->fields[d->first_field + a];
                for (b = 0; b < a; b++) {
                    weftc_field_t *fb_ = &c->fields[d->first_field + b];
                    if (fa_->name_len == fb_->name_len &&
                        memcmp(c->src + fa_->name_off, c->src + fb_->name_off,
                               fa_->name_len) == 0) {
                        char scratch[192];
                        const char *pre = "duplicate field name `";
                        const char *nm = c->name_pool + fa_->name_pool;
                        uint32_t o = 0, k = 0;
                        while (pre[o]) { scratch[o] = pre[o]; o++; }
                        while (nm[k] && o < 180u) scratch[o++] = nm[k++];
                        scratch[o++] = '`';
                        scratch[o] = 0;
                        diag_stage(c, 4, 1, scratch, NULL, fa_->span_start,
                                   fa_->span_start + fa_->name_len);
                    }
                }
            }
            continue;
        }
        for (a = 0; a < d->nvariants; a++) {
            weftc_variant_t *va = &c->variants[d->first_variant + a];
            for (b = 0; b < a; b++) {
                weftc_variant_t *vb = &c->variants[d->first_variant + b];
                int name_eq = (va->name_len == vb->name_len) &&
                    memcmp(c->src + va->name_off, c->src + vb->name_off,
                           va->name_len) == 0;
                if (name_eq) {
                    char scratch[192];
                    const char *pre = "duplicate variant name `";
                    const char *nm = c->name_pool + va->name_pool;
                    uint32_t o = 0, k = 0;
                    while (pre[o]) { scratch[o] = pre[o]; o++; }
                    while (nm[k] && o < 180u) scratch[o++] = nm[k++];
                    scratch[o++] = '`';
                    scratch[o] = 0;
                    diag_stage(c, 10, 1, scratch, NULL, va->span_start,
                               va->span_start + va->name_len);
                }
                if (!is_bitflags && va->value == vb->value) {
                    char scratch[192];
                    const char *pre = "duplicate enum discriminant `";
                    const char *nm = c->name_pool + va->name_pool;
                    uint32_t o = 0, k = 0;
                    while (pre[o]) { scratch[o] = pre[o]; o++; }
                    while (nm[k] && o < 180u) scratch[o++] = nm[k++];
                    scratch[o++] = '`';
                    scratch[o] = 0;
                    diag_stage(c, 11, 1, scratch,
                               "discriminants are control-flow values",
                               va->span_start, va->span_start + va->name_len);
                }
                if (is_bitflags && va->value == vb->value && !name_eq) {
                    char scratch[192];
                    const char *pre = "bitflags value `";
                    const char *nm = c->name_pool + va->name_pool;
                    uint32_t o = 0, k = 0;
                    while (pre[o]) { scratch[o] = pre[o]; o++; }
                    while (nm[k] && o < 180u) scratch[o++] = nm[k++];
                    {
                        const char *mid = "` aliases an earlier variant";
                        uint32_t m = 0;
                        while (mid[m] && o < 186u) scratch[o++] = mid[m++];
                    }
                    scratch[o] = 0;
                    diag_stage(c, 902, 2, scratch,
                               "COMBINED = A | B is legal; verify the alias is "
                               "intended",
                               va->span_start, va->span_start + va->name_len);
                }
            }
            if (is_bitflags && va->value < 0) {
                char scratch[192];
                const char *pre = "bitflags value `";
                const char *nm = c->name_pool + va->name_pool;
                uint32_t o = 0, k = 0;
                while (pre[o]) { scratch[o] = pre[o]; o++; }
                while (nm[k] && o < 180u) scratch[o++] = nm[k++];
                {
                    const char *mid = "` is negative";
                    uint32_t m = 0;
                    while (mid[m] && o < 186u) scratch[o++] = mid[m++];
                }
                scratch[o] = 0;
                diag_stage(c, 31, 1, scratch, NULL, va->span_start,
                           va->span_start + va->name_len);
            } else if (!variant_fits(d->backing, va->value, is_bitflags)) {
                char scratch[192];
                const char *pre = "variant value of `";
                const char *nm = c->name_pool + va->name_pool;
                uint32_t o = 0, k = 0;
                while (pre[o]) { scratch[o] = pre[o]; o++; }
                while (nm[k] && o < 180u) scratch[o++] = nm[k++];
                {
                    const char *mid = "` does not fit the backing type";
                    uint32_t m = 0;
                    while (mid[m] && o < 188u) scratch[o++] = mid[m++];
                }
                scratch[o] = 0;
                diag_stage(c, 13, 1, scratch, NULL, va->span_start,
                           va->span_start + va->name_len);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Canonical manifests (WAB1 / WID1 / WDC1) — frozen at IR version 1   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    uint32_t n, cap;
    int      of;
} MBuf;

static void mb_u8(MBuf *b, uint8_t v)
{
    if (b->n < b->cap) b->buf[b->n] = v; else b->of = 1;
    b->n++;
}
static void mb_bytes(MBuf *b, const void *p, uint32_t len)
{
    if ((uint64_t)b->n + len > b->cap) { b->of = 1; b->n += len; return; }
    if (len) memcpy(b->buf + b->n, p, len);
    b->n += len;
}
static void mb_u16le(MBuf *b, uint16_t v)
{
    uint8_t t[2];
    t[0] = (uint8_t)(v & 0xFF); t[1] = (uint8_t)(v >> 8);
    mb_bytes(b, t, 2);
}
static void mb_u32le(MBuf *b, uint32_t v)
{
    uint8_t t[4];
    t[0] = (uint8_t)(v & 0xFF); t[1] = (uint8_t)((v >> 8) & 0xFF);
    t[2] = (uint8_t)((v >> 16) & 0xFF); t[3] = (uint8_t)((v >> 24) & 0xFF);
    mb_bytes(b, t, 4);
}
static void mb_u64le(MBuf *b, uint64_t v)
{
    uint8_t t[8];
    weft_mix_u64le(t, v);
    mb_bytes(b, t, 8);
}
static void mb_i64le(MBuf *b, int64_t v)
{
    mb_u64le(b, (uint64_t)v);
}
static void mb_str(MBuf *b, const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    mb_u16le(b, (uint16_t)(len > 0xFFFFu ? 0xFFFFu : len));
    mb_bytes(b, s, len > 0xFFFFu ? 0xFFFFu : len);
}

static void emit_manifest_header(MBuf *b, const char *magic,
                                 const weftc_ctx_t *c, uint32_t ndecls)
{
    mb_bytes(b, magic, 4);
    mb_u8(b, (uint8_t)WEFTC_IR_VERSION);
    mb_u8(b, (uint8_t)(c->endian_big ? 1 : 0));
    mb_u32le(b, ndecls);
}

static void emit_decl_record(weftc_ctx_t *c, uint32_t i, MBuf *b,
                             int with_names, int with_attrs)
{
    const weftc_decl_t *d = &c->decls[i];
    mb_u8(b, (uint8_t)(d->kind == DCL_STRUCT ? 0
                     : d->kind == DCL_ENUM ? 1 : 2));
    if (d->kind == DCL_STRUCT) {
        uint8_t fl = 0;
        uint32_t s;
        if (d->packed) fl |= 1u << 0;
        if (d->reordered) fl |= 1u << 1;
        if (with_attrs && d->has_align_attr) fl |= 1u << 2;
        if (with_attrs && d->has_simd_attr) fl |= 1u << 3;
        mb_u8(b, fl);
        mb_u64le(b, d->size);
        mb_u64le(b, d->align);
        if (with_attrs && d->has_align_attr) mb_u64le(b, d->align_attr);
        if (with_attrs && d->has_simd_attr) mb_u64le(b, d->simd_attr);
        mb_u32le(b, d->nplanned);
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            if (with_names) mb_str(b, c->name_pool + f->name_pool);
            mb_u64le(b, f->offset);
            mb_u64le(b, f->size);
            mb_u64le(b, f->align);
            mb_str(b, c->name_pool + f->tystr_pool);
            if (d->reordered) mb_u32le(b, f->orig_index);
        }
        mb_u32le(b, d->hole_count);
        for (s = 0; s < d->hole_count; s++) {
            mb_u64le(b, c->holes[d->hole_start + s].offset);
            mb_u64le(b, c->holes[d->hole_start + s].size);
        }
    } else {
        mb_u8(b, (uint8_t)d->backing);
        mb_u64le(b, d->size);
        mb_u64le(b, d->align);
        if (with_attrs && d->has_align_attr) mb_u64le(b, d->align_attr);
        if (with_attrs && d->has_simd_attr) mb_u64le(b, d->simd_attr);
        mb_u32le(b, d->nvariants);
        {
            uint32_t s;
            for (s = 0; s < d->nvariants; s++) {
                const weftc_variant_t *v =
                    &c->variants[d->first_variant + s];
                if (with_names) mb_str(b, c->name_pool + v->name_pool);
                mb_i64le(b, v->value);
            }
        }
    }
}

static void compute_hashes(weftc_ctx_t *c)
{
    MBuf b;
    uint32_t i;
    b.buf = c->manifest;
    b.cap = WEFTC_MAX_MANIFEST;
    b.n = 0;
    b.of = 0;

    /* WAB1 — whole-schema ABI manifest (effect-only) */
    emit_manifest_header(&b, "WAB1", c, c->ndecls);
    for (i = 0; i < c->ndecls; i++)
        emit_decl_record(c, i, &b, 0, 0);
    c->abi_hash = weft_wh64(c->manifest, b.n);
    c->fnv1a64_abi = weft_fnv1a64(c->manifest, b.n);

    /* WID1 — identity manifest (names + declared attrs) */
    b.n = 0;
    emit_manifest_header(&b, "WID1", c, c->ndecls);
    for (i = 0; i < c->ndecls; i++) {
        mb_str(&b, decl_name_ptr(c, &c->decls[i]));
        emit_decl_record(c, i, &b, 1, 1);
    }
    c->schema_id = weft_wh64(c->manifest, b.n);

    /* WDC1 — per-decl structural manifest (the wire handshake number) */
    for (i = 0; i < c->ndecls; i++) {
        b.n = 0;
        emit_manifest_header(&b, "WDC1", c, 1);
        emit_decl_record(c, i, &b, 0, 0);
        c->decls[i].abi_hash = weft_wh64(c->manifest, b.n);
    }
    if (b.of) c->capacity_hit = 1;
}

/* ------------------------------------------------------------------ */
/* Studio layout diagnostics (Law 2 tripwires)                          */
/* ------------------------------------------------------------------ */
static void studio_layout_diags(weftc_ctx_t *c)
{
    uint32_t di;
    uint32_t cl = c->cache_line ? c->cache_line : 64u;
    for (di = 0; di < c->ndecls; di++) {
        const weftc_decl_t *d = &c->decls[di];
        uint32_t s;
        if (d->kind != DCL_STRUCT || d->poisoned) continue;
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            uint64_t end;
            if (f->size == 0) continue;
            end = f->offset + f->size - 1u;
            if ((f->offset / cl) != (end / cl)) {
                char scratch[224];
                const char *pre = "field `";
                const char *nm = c->name_pool + f->name_pool;
                const char *dn = decl_name_ptr(c, d);
                uint32_t o = 0, k = 0;
                while (pre[o]) { scratch[o] = pre[o]; o++; }
                while (nm[k] && o < 200u) scratch[o++] = nm[k++];
                {
                    const char *mid = "` of `";
                    uint32_t m = 0;
                    while (mid[m] && o < 208u) scratch[o++] = mid[m++];
                }
                k = 0;
                while (dn[k] && o < 216u) scratch[o++] = dn[k++];
                {
                    char tail[96];
                    uint32_t t = 0;
                    const char *w = "` crosses a cache-line boundary";
                    while (w[t]) { tail[t] = w[t]; t++; }
                    {
                        uint32_t m = 0;
                        while (m < t && o < 220u) scratch[o++] = tail[m++];
                    }
                }
                scratch[o] = 0;
                diag_stage(c, WEFT_D_CACHE_LINE_CROSS, WEFT_SEV_WARN,
                           scratch,
                           "reorder fields by descending alignment or raise "
                           "the struct alignment",
                           f->span_start, f->span_start + f->name_len);
            }
            if ((f->offset / 128u) != (end / 128u)) {
                char scratch[224];
                const char *pre = "field `";
                const char *nm = c->name_pool + f->name_pool;
                uint32_t o = 0, k = 0;
                while (pre[o]) { scratch[o] = pre[o]; o++; }
                while (nm[k] && o < 200u) scratch[o++] = nm[k++];
                {
                    const char *mid = "` crosses a 128 B cache-line boundary";
                    uint32_t m = 0;
                    while (mid[m] && o < 220u) scratch[o++] = mid[m++];
                }
                scratch[o] = 0;
                diag_stage(c, WEFT_D_CL128_CROSS, WEFT_SEV_WARN, scratch,
                           "place the field with @align(128) or reorganize "
                           "the struct",
                           f->span_start, f->span_start + f->name_len);
            }
        }
        if (d->trailing_pad > 0) {
            char scratch[224];
            const char *pre = "struct `";
            const char *dn = decl_name_ptr(c, d);
            uint32_t o = 0, k = 0;
            while (pre[o]) { scratch[o] = pre[o]; o++; }
            while (dn[k] && o < 200u) scratch[o++] = dn[k++];
            {
                const char *mid = "` is not fully packed: trailing padding";
                uint32_t m = 0;
                while (mid[m] && o < 218u) scratch[o++] = mid[m++];
            }
            scratch[o] = 0;
            diag_stage(c, WEFT_D_TRAILING_PAD, WEFT_SEV_HINT, scratch,
                       "add @optimize(packing) or reorder fields by "
                       "descending alignment",
                       d->name_off, d->name_off + d->name_len);
        }
        if (d->optimize_hint > 0) {
            char scratch[224];
            const char *pre = "struct `";
            const char *dn = decl_name_ptr(c, d);
            uint32_t o = 0, k = 0;
            while (pre[o]) { scratch[o] = pre[o]; o++; }
            while (dn[k] && o < 200u) scratch[o++] = dn[k++];
            {
                const char *mid = "` can shrink with @optimize(packing)";
                uint32_t m = 0;
                while (mid[m] && o < 218u) scratch[o++] = mid[m++];
            }
            scratch[o] = 0;
            diag_stage(c, WEFT_D_OPTIMIZE_HINT, WEFT_SEV_INFO, scratch,
                       "add @optimize(packing) above the struct",
                       d->name_off, d->name_off + d->name_len);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Finalization: byte spans -> LSP positions; layout tables -> ABI      */
/* ------------------------------------------------------------------ */
static void off_to_pos(const weftc_ctx_t *c, uint32_t off,
                       uint32_t *line, uint32_t *col)
{
    uint32_t lo = 0, hi = c->nlines;
    while (lo + 1u < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (c->line_starts[mid] <= off) lo = mid;
        else hi = mid;
    }
    *line = lo;
    *col = (off >= c->line_starts[lo]) ? off - c->line_starts[lo] : 0;
}

static void finalize_diags(weftc_ctx_t *c)
{
    uint32_t i;
    c->ndiags = 0;
    for (i = 0; i < c->nstaging && c->ndiags < WEFTC_MAX_DIAGS; i++) {
        const weftc_diagst_t *st = &c->staging[i];
        weft_diag_t *dg = &c->diags[c->ndiags];
        uint32_t sl, sc, el, ec;
        if (st->msg_off >= WEFTC_MAX_MSG_POOL) continue;
        off_to_pos(c, st->start_off, &sl, &sc);
        off_to_pos(c, st->end_off, &el, &ec);
        dg->message = c->msg_pool + st->msg_off;
        dg->fix_suggestion = (st->fix_off != UINT32_MAX &&
                              st->fix_off < WEFTC_MAX_MSG_POOL)
                             ? c->msg_pool + st->fix_off : NULL;
        dg->code = st->code;
        dg->severity = st->sev;
        dg->flags = 0;
        dg->start_line = sl; dg->start_col = sc;
        dg->end_line = el; dg->end_col = ec;
        c->ndiags++;
    }
    if (c->diags_dropped > 0 && c->ndiags < WEFTC_MAX_DIAGS) {
        weft_diag_t *dg = &c->diags[c->ndiags];
        char scratch[96];
        diag_fmsg(c, scratch, sizeof scratch,
                  "diagnostic capacity reached; %u further diagnostics "
                  "suppressed", NULL, NULL, c->diags_dropped, 0, 0);
        dg->message = NULL; /* replaced below: pool may be full */
        /* stage through the pool safely */
        {
            msgbuf_t mb;
            uint32_t at = c->msg_pool_n;
            mb.base = c->msg_pool + at;
            mb.cap = WEFTC_MAX_MSG_POOL - at;
            mb.n = 0;
            mb.overflow = 0;
            mbx_str(&mb, scratch);
            if (!mb.overflow) {
                c->msg_pool_n += mb.n;
                dg->message = c->msg_pool + at;
            }
        }
        dg->fix_suggestion = NULL;
        dg->code = 2199;
        dg->severity = WEFT_SEV_INFO;
        dg->flags = 0;
        dg->start_line = 0; dg->start_col = 0;
        dg->end_line = 0; dg->end_col = 0;
        if (dg->message) c->ndiags++;
    }
}

static void finalize_layouts(weftc_ctx_t *c)
{
    uint32_t di;
    uint32_t cl = c->cache_line ? c->cache_line : 64u;
    c->nlayouts = 0;
    for (di = 0; di < c->ndecls; di++) {
        const weftc_decl_t *d = &c->decls[di];
        weft_struct_layout_t *L = &c->layouts[c->nlayouts++];
        uint32_t s;
        L->name = decl_name_ptr(c, d);
        L->fields = (d->kind == DCL_STRUCT)
                    ? &c->flayouts[d->first_field] : NULL;
        L->holes = (d->kind == DCL_STRUCT && d->hole_count)
                   ? &c->holes[d->hole_start] : NULL;
        L->abi_hash = d->abi_hash;
        L->size = d->size;
        L->align = d->align;
        L->internal_pad = d->internal_pad;
        L->trailing_pad = d->trailing_pad;
        L->optimize_hint = d->optimize_hint;
        L->field_count = (d->kind == DCL_STRUCT) ? d->nplanned : 0;
        L->hole_count = d->hole_count;
        L->flags = (uint32_t)((d->packed ? WEFT_SLF_PACKED : 0u)
                   | (d->reordered ? WEFT_SLF_REORDERED : 0u)
                   | (d->has_align_attr ? WEFT_SLF_ALIGN_ATTR : 0u)
                   | (d->has_simd_attr ? WEFT_SLF_SIMD_ATTR : 0u));
        L->cache_line_span = (uint32_t)((d->size + cl - 1u) / cl);
        if (d->kind != DCL_STRUCT) continue;
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            weft_field_layout_t *F = &c->flayouts[d->first_field + s];
            uint64_t end;
            F->name = c->name_pool + f->name_pool;
            F->type_str = c->name_pool + f->tystr_pool;
            F->type_tag = f->ty_tag;
            F->orig_index = f->orig_index;
            F->offset = f->offset;
            F->size = f->size;
            F->align = f->align;
            F->padding_after = f->padding_after;
            F->cache_line_idx = (uint32_t)(f->offset / cl);
            F->flags = 0;
            if (f->size == 0) continue;
            end = f->offset + f->size - 1u;
            if ((f->offset / 64u) != (end / 64u))
                F->flags |= WEFT_FLF_CROSSES_CL64;
            if ((f->offset / 128u) != (end / 128u))
                F->flags |= WEFT_FLF_CROSSES_CL128;
            if (f->packed) F->flags |= WEFT_FLF_PACKED;
            if (f->has_attr_align) F->flags |= WEFT_FLF_ATTR_ALIGN;
            if (f->has_attr_simd) F->flags |= WEFT_FLF_ATTR_SIMD;
            if ((F->flags & WEFT_FLF_CROSSES_CL64) &&
                (f->ty_tag <= 12u || f->ty_tag == WEFT_TY_ENUM ||
                 f->ty_tag == WEFT_TY_BITFLAGS || f->ty_tag == WEFT_TY_SPAN))
                F->flags |= WEFT_FLF_FALSE_SHARING;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Multi-target code previews                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *buf;
    size_t cap;
    size_t n;
    int    of;
} CG;

static void cg_mem(CG *g, const char *s, size_t len)
{
    if ((uint64_t)g->n + len > g->cap) { g->of = 1; return; }
    if (len) memcpy(g->buf + g->n, s, len);
    g->n += len;
}
static void cg_str(CG *g, const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    cg_mem(g, s, len);
}
static void cg_ch(CG *g, char c) { cg_mem(g, &c, 1); }
static void cg_u64(CG *g, uint64_t v)
{
    char tmp[24];
    uint32_t i = 0, j;
    if (v == 0) { cg_ch(g, '0'); return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = i; j > 0; j--) cg_ch(g, tmp[j - 1]);
}
static void cg_i64(CG *g, int64_t v)
{
    uint64_t m;
    if (v < 0) { cg_ch(g, '-'); m = (uint64_t)(-(v + 1)) + 1u; }
    else m = (uint64_t)v;
    cg_u64(g, m);
}
static void cg_hex64(CG *g, uint64_t v)
{
    const char *hexd = "0123456789abcdef";
    char tmp[16];
    uint32_t i = 0, j;
    cg_str(g, "0x");
    if (v == 0) { cg_ch(g, '0'); return; }
    while (v) { tmp[i++] = hexd[v & 0xF]; v >>= 4; }
    for (j = i; j > 0; j--) cg_ch(g, tmp[j - 1]);
}
static void cg_hex32(CG *g, uint64_t v)
{
    const char *hexd = "0123456789abcdef";
    char tmp[16];
    uint32_t i = 0, j;
    cg_str(g, "0x");
    if (v == 0) { cg_ch(g, '0'); return; }
    while (v) { tmp[i++] = hexd[v & 0xF]; v >>= 4; }
    for (j = i; j > 0; j--) cg_ch(g, tmp[j - 1]);
}

static const char *const prim_c[13] = {
    NULL, "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t",
    "int32_t", "uint64_t", "int64_t", "uint16_t", "float", "double",
    "uint8_t"
};
static const char *const prim_rust[13] = {
    NULL, "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "u16",
    "f32", "f64", "u8"
};
static const char *const prim_swift[13] = {
    NULL, "UInt8", "Int8", "UInt16", "Int16", "UInt32", "Int32", "UInt64",
    "Int64", "UInt16", "Float", "Double", "Bool"
};

static int decl_has_span(const weftc_ctx_t *c)
{
    uint32_t di, s;
    for (di = 0; di < c->ndecls; di++) {
        const weftc_decl_t *d = &c->decls[di];
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            if (f->ty_node != WEFT_AST_NONE &&
                f->ty_node < c->nnodes &&
                c->nodes[f->ty_node].kind == WEFT_AST_TY_SPAN)
                return 1;
        }
    }
    return 0;
}

/* C declarator: type + name with array suffixes. */
static void cg_c_decl(CG *g, const weftc_ctx_t *c, uint32_t node,
                      const char *name)
{
    if (node == WEFT_AST_NONE || node >= c->nnodes) {
        cg_str(g, "/*?*/ ");
        cg_str(g, name);
        return;
    }
    {
        const weft_ast_node_t *n = &c->nodes[node];
        switch (n->kind) {
        case WEFT_AST_TY_ARRAY:
            cg_c_decl(g, c, n->first_child, name);
            cg_ch(g, '[');
            cg_u64(g, n->value);
            cg_ch(g, ']');
            return;
        default:
            break;
        }
        if (n->kind == WEFT_AST_TY_PRIM) {
            cg_str(g, prim_c[(n->value >= 1 && n->value <= 12u)
                             ? n->value : 1]);
            if (n->value == 9) cg_str(g, " /* f16 raw bits */");
            if (n->value == 12) cg_str(g, " /* weft bool: 0|1 */");
        } else if (n->kind == WEFT_AST_TY_STR) {
            cg_str(g, "char");
        } else if (n->kind == WEFT_AST_TY_SPAN) {
            cg_str(g, "weft_span");
        } else {
            /* named reference */
            uint32_t k;
            for (k = 0; k < n->name_len; k++)
                cg_ch(g, c->src[n->name_off + k]);
        }
        cg_ch(g, ' ');
        cg_str(g, name);
    }
}

/* Rust type (prefix syntax). */
static void cg_rust_ty(CG *g, const weftc_ctx_t *c, uint32_t node)
{
    if (node == WEFT_AST_NONE || node >= c->nnodes) { cg_str(g, "u8"); return; }
    {
        const weft_ast_node_t *n = &c->nodes[node];
        switch (n->kind) {
        case WEFT_AST_TY_PRIM:
            cg_str(g, prim_rust[(n->value >= 1 && n->value <= 12u)
                                ? n->value : 1]);
            if (n->value == 9) cg_str(g, " /* f16 raw bits */");
            if (n->value == 12) cg_str(g, " /* weft bool: 0|1 */");
            return;
        case WEFT_AST_TY_SPAN:
            cg_str(g, "WeftSpan");
            return;
        case WEFT_AST_TY_ARRAY:
            cg_ch(g, '[');
            cg_rust_ty(g, c, n->first_child);
            cg_str(g, "; ");
            cg_u64(g, n->value);
            cg_ch(g, ']');
            return;
        case WEFT_AST_TY_STR:
            cg_str(g, "[u8; ");
            cg_u64(g, n->value);
            cg_ch(g, ']');
            return;
        default: {
            uint32_t k;
            for (k = 0; k < n->name_len; k++)
                cg_ch(g, c->src[n->name_off + k]);
            return;
        }
        }
    }
}

/* Swift type. */
static void cg_swift_ty(CG *g, const weftc_ctx_t *c, uint32_t node)
{
    if (node == WEFT_AST_NONE || node >= c->nnodes) { cg_str(g, "UInt8"); return; }
    {
        const weft_ast_node_t *n = &c->nodes[node];
        switch (n->kind) {
        case WEFT_AST_TY_PRIM:
            cg_str(g, prim_swift[(n->value >= 1 && n->value <= 12u)
                                 ? n->value : 1]);
            if (n->value == 9) cg_str(g, " /* f16 raw bits */");
            return;
        case WEFT_AST_TY_SPAN:
            cg_str(g, "WeftSpan");
            return;
        case WEFT_AST_TY_ARRAY:
            cg_str(g, "(");
            cg_swift_ty(g, c, n->first_child);
            cg_str(g, ", count: ");
            cg_u64(g, n->value);
            cg_ch(g, ')');
            return;
        case WEFT_AST_TY_STR:
            cg_str(g, "[UInt8] /* str[");
            cg_u64(g, n->value);
            cg_str(g, "] */");
            return;
        default: {
            uint32_t k;
            for (k = 0; k < n->name_len; k++)
                cg_ch(g, c->src[n->name_off + k]);
            return;
        }
        }
    }
}

static void cg_header(CG *g, const weftc_ctx_t *c, int lang)
{
    static const char *const comment[7] = {
        "/* Weft Studio - .weft schema preview (C). */",
        "/* Weft Studio - .weft schema preview (C++). */",
        "// Weft Studio - .weft schema preview (Rust).",
        "// Weft Studio - .weft schema preview (TypeScript).",
        "# Weft Studio - .weft schema preview (Python).",
        "// Weft Studio - .weft schema preview (Dart).",
        "// Weft Studio - .weft schema preview (Swift)."
    };
    cg_str(g, comment[lang]);
    cg_ch(g, '\n');
    if (lang == WEFT_LANG_C) {
        cg_str(g, "#include <stdint.h>\n#include <stddef.h>\n");
    } else if (lang == WEFT_LANG_CPP) {
        cg_str(g, "#include <cstdint>\n#include <cstddef>\n");
    }
    if (lang == WEFT_LANG_C || lang == WEFT_LANG_CPP) {
        if (decl_has_span(c)) {
            cg_str(g, "typedef struct weft_span { uint64_t offset; "
                      "uint64_t len; } weft_span;\n");
        }
        cg_str(g, "/* endianness: ");
        cg_str(g, c->endian_big ? "big" : "little");
        cg_str(g, " */\n");
    } else if (lang == WEFT_LANG_RUST) {
        if (decl_has_span(c)) {
            cg_str(g, "#[repr(C)] #[derive(Clone, Copy)] "
                      "pub struct WeftSpan { pub offset: u64, pub len: u64 }\n");
        }
        cg_str(g, "// endianness: ");
        cg_str(g, c->endian_big ? "big" : "little");
        cg_ch(g, '\n');
    } else {
        cg_str(g, "// endianness: ");
        cg_str(g, c->endian_big ? "big" : "little");
        cg_ch(g, '\n');
    }
    cg_str(g, lang == WEFT_LANG_PYTHON ? "# abi_hash " : "// abi_hash ");
    cg_hex64(g, c->abi_hash);
    cg_str(g, ", schema_id ");
    cg_hex64(g, c->schema_id);
    cg_ch(g, '\n');
    cg_ch(g, '\n');
}

static void cg_enum(CG *g, const weftc_ctx_t *c, uint32_t di, int lang)
{
    const weftc_decl_t *d = &c->decls[di];
    const char *nm = decl_name_ptr(c, d);
    uint32_t s;
    if (lang == WEFT_LANG_C || lang == WEFT_LANG_CPP) {
        if (d->kind == DCL_ENUM) {
            cg_str(g, "/* enum ");
            cg_str(g, nm);
            cg_str(g, " : ");
            cg_str(g, weft_prim_names[d->backing]);
            cg_str(g, " - ");
            cg_u64(g, d->size);
            cg_str(g, " B, abi_hash ");
            cg_hex64(g, d->abi_hash);
            cg_str(g, " */\nenum ");
            cg_str(g, nm);
            cg_str(g, " { ");
            for (s = 0; s < d->nvariants; s++) {
                const weftc_variant_t *v = &c->variants[d->first_variant + s];
                if (s) cg_str(g, ", ");
                cg_str(g, nm);
                cg_ch(g, '_');
                cg_str(g, c->name_pool + v->name_pool);
                cg_str(g, " = ");
                cg_i64(g, v->value);
            }
            cg_str(g, " };\n\n");
        } else {
            cg_str(g, "/* bitflags ");
            cg_str(g, nm);
            cg_str(g, " : ");
            cg_str(g, weft_prim_names[d->backing]);
            cg_str(g, " - ");
            cg_u64(g, d->size);
            cg_str(g, " B, abi_hash ");
            cg_hex64(g, d->abi_hash);
            cg_str(g, " */\ntypedef ");
            cg_str(g, prim_c[d->backing]);
            cg_ch(g, ' ');
            cg_str(g, nm);
            cg_str(g, ";\n");
            for (s = 0; s < d->nvariants; s++) {
                const weftc_variant_t *v = &c->variants[d->first_variant + s];
                cg_str(g, "#define ");
                {
                    /* NAME_VARIANT upper */
                    uint32_t k;
                    for (k = 0; nm[k]; k++)
                        cg_ch(g, (char)((nm[k] >= 'a' && nm[k] <= 'z')
                                        ? nm[k] - 32 : nm[k]));
                    cg_ch(g, '_');
                }
                cg_str(g, c->name_pool + v->name_pool);
                cg_str(g, " ((");
                cg_str(g, nm);
                cg_str(g, ")");
                cg_hex32(g, (uint64_t)v->value);
                cg_str(g, "u)\n");
            }
            cg_ch(g, '\n');
        }
        return;
    }
    if (lang == WEFT_LANG_RUST) {
        cg_str(g, "#[repr(");
        cg_str(g, weft_prim_names[d->backing]);
        cg_str(g, ")]\n");
        if (d->kind == DCL_ENUM) {
            cg_str(g, "pub enum ");
            cg_str(g, nm);
            cg_str(g, " { ");
            for (s = 0; s < d->nvariants; s++) {
                const weftc_variant_t *v = &c->variants[d->first_variant + s];
                if (s) cg_str(g, ", ");
                cg_str(g, c->name_pool + v->name_pool);
                cg_str(g, " = ");
                cg_i64(g, v->value);
            }
            cg_str(g, " }");
        } else {
            cg_str(g, "pub struct ");
            cg_str(g, nm);
            cg_str(g, "(pub ");
            cg_str(g, prim_rust[d->backing]);
            cg_str(g, ");\nimpl ");
            cg_str(g, nm);
            cg_str(g, " { ");
            for (s = 0; s < d->nvariants; s++) {
                const weftc_variant_t *v = &c->variants[d->first_variant + s];
                cg_str(g, "pub const ");
                cg_str(g, c->name_pool + v->name_pool);
                cg_str(g, ": Self = Self(");
                cg_i64(g, v->value);
                cg_str(g, "); ");
            }
            cg_str(g, "}");
        }
        cg_str(g, " // ");
        cg_u64(g, d->size);
        cg_str(g, " B, abi_hash ");
        cg_hex64(g, d->abi_hash);
        cg_str(g, "\n\n");
        return;
    }
    if (lang == WEFT_LANG_TYPESCRIPT) {
        cg_str(g, "// ");
        cg_str(g, d->kind == DCL_ENUM ? "enum " : "bitflags ");
        cg_str(g, nm);
        cg_str(g, " : ");
        cg_str(g, weft_prim_names[d->backing]);
        cg_str(g, " - ");
        cg_u64(g, d->size);
        cg_str(g, " B\nexport const ");
        {
            uint32_t k;
            for (k = 0; nm[k]; k++)
                cg_ch(g, (char)((nm[k] >= 'a' && nm[k] <= 'z')
                                ? nm[k] - 32 : nm[k]));
        }
        cg_str(g, " = { ");
        for (s = 0; s < d->nvariants; s++) {
            const weftc_variant_t *v = &c->variants[d->first_variant + s];
            if (s) cg_str(g, ", ");
            cg_str(g, c->name_pool + v->name_pool);
            cg_str(g, ": ");
            cg_i64(g, v->value);
        }
        cg_str(g, " } as const;\n\n");
        return;
    }
    if (lang == WEFT_LANG_PYTHON) {
        cg_str(g, "class ");
        cg_str(g, nm);
        cg_str(g, d->kind == DCL_ENUM ? "(enum.IntEnum)" : "(enum.IntFlag)");
        cg_str(g, ":  # backing ");
        cg_str(g, weft_prim_names[d->backing]);
        cg_str(g, ", ");
        cg_u64(g, d->size);
        cg_str(g, " B\n");
        for (s = 0; s < d->nvariants; s++) {
            const weftc_variant_t *v = &c->variants[d->first_variant + s];
            cg_str(g, "    ");
            cg_str(g, c->name_pool + v->name_pool);
            cg_str(g, " = ");
            cg_i64(g, v->value);
            cg_ch(g, '\n');
        }
        cg_ch(g, '\n');
        return;
    }
    if (lang == WEFT_LANG_DART) {
        cg_str(g, "/// ");
        cg_str(g, d->kind == DCL_ENUM ? "enum " : "bitflags ");
        cg_str(g, nm);
        cg_str(g, " : ");
        cg_str(g, weft_prim_names[d->backing]);
        cg_str(g, " - ");
        cg_u64(g, d->size);
        cg_str(g, " B\nabstract final class ");
        cg_str(g, nm);
        cg_str(g, "Values {\n");
        for (s = 0; s < d->nvariants; s++) {
            const weftc_variant_t *v = &c->variants[d->first_variant + s];
            const char *vn = c->name_pool + v->name_pool;
            cg_str(g, "  static const int ");
            {
                /* lowerCamel */
                uint32_t k;
                for (k = 0; vn[k]; k++) {
                    char ch = vn[k];
                    if (k == 0 && ch >= 'A' && ch <= 'Z')
                        cg_ch(g, (char)(ch + 32));
                    else
                        cg_ch(g, ch);
                }
            }
            cg_str(g, " = ");
            cg_i64(g, v->value);
            cg_str(g, ";\n");
        }
        cg_str(g, "}\n\n");
        return;
    }
    /* Swift */
    if (d->kind == DCL_ENUM) {
        cg_str(g, "enum ");
        cg_str(g, nm);
        cg_str(g, ": ");
        cg_str(g, prim_swift[d->backing]);
        cg_str(g, " {  // ");
        cg_u64(g, d->size);
        cg_str(g, " B\n    case ");
        for (s = 0; s < d->nvariants; s++) {
            const weftc_variant_t *v = &c->variants[d->first_variant + s];
            const char *vn = c->name_pool + v->name_pool;
            if (s) cg_str(g, ", ");
            {
                uint32_t k;
                for (k = 0; vn[k]; k++) {
                    char ch = vn[k];
                    if (k == 0 && ch >= 'A' && ch <= 'Z')
                        cg_ch(g, (char)(ch + 32));
                    else
                        cg_ch(g, ch);
                }
            }
            cg_str(g, " = ");
            cg_i64(g, v->value);
        }
        cg_str(g, "\n}\n\n");
    } else {
        cg_str(g, "typealias ");
        cg_str(g, nm);
        cg_str(g, " = ");
        cg_str(g, prim_swift[d->backing]);
        cg_str(g, "  // bitflags, ");
        cg_u64(g, d->size);
        cg_str(g, " B: ");
        for (s = 0; s < d->nvariants; s++) {
            const weftc_variant_t *v = &c->variants[d->first_variant + s];
            if (s) cg_str(g, ", ");
            cg_str(g, c->name_pool + v->name_pool);
            cg_str(g, "=");
            cg_hex32(g, (uint64_t)v->value);
        }
        cg_str(g, "\n\n");
    }
}

static void cg_struct(CG *g, const weftc_ctx_t *c, uint32_t di, int lang)
{
    const weftc_decl_t *d = &c->decls[di];
    const char *nm = decl_name_ptr(c, d);
    uint32_t s;
    uint32_t pad_i = 0;
    if (d->kind != DCL_STRUCT) return;

    if (lang == WEFT_LANG_C || lang == WEFT_LANG_CPP) {
        cg_str(g, "/* struct ");
        cg_str(g, nm);
        cg_str(g, " - ");
        cg_u64(g, d->size);
        cg_str(g, " B, align ");
        cg_u64(g, d->align);
        cg_str(g, ", abi_hash ");
        cg_hex64(g, d->abi_hash);
        cg_str(g, " */\n");
        if (d->nplanned == 0) {
            cg_str(g, "/* (empty struct: size 0 is legal in .weft, not in "
                      "ISO C) */\n\n");
            return;
        }
        if (lang == WEFT_LANG_C) {
            cg_str(g, "typedef struct");
            if (d->has_align_attr && d->align_attr > 1) {
                cg_str(g, " __attribute__((aligned(");
                cg_u64(g, d->align_attr);
                cg_str(g, ")))");
            }
            cg_ch(g, ' ');
            cg_str(g, nm);
            cg_str(g, " {\n");
        } else {
            cg_str(g, "struct ");
            cg_str(g, nm);
            cg_str(g, " {\n");
        }
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            cg_str(g, "    ");
            if (f->ty_node != WEFT_AST_NONE &&
                c->nodes[f->ty_node].kind == WEFT_AST_TY_STR) {
                cg_str(g, "char ");
                cg_str(g, fn);
                cg_ch(g, '[');
                cg_u64(g, c->nodes[f->ty_node].value);
                cg_ch(g, ']');
            } else {
                cg_c_decl(g, c, f->ty_node, fn);
            }
            cg_str(g, ";  /* offset ");
            cg_u64(g, f->offset);
            cg_str(g, ", size ");
            cg_u64(g, f->size);
            cg_str(g, ", align ");
            cg_u64(g, f->align);
            cg_str(g, " */\n");
            if (f->padding_after > 0) {
                cg_str(g, "    uint8_t _weft_pad_");
                cg_u64(g, pad_i++);
                cg_ch(g, '[');
                cg_u64(g, f->padding_after);
                cg_str(g, "];  /* hole [");
                cg_u64(g, f->offset + f->size);
                cg_str(g, ", ");
                cg_u64(g, f->offset + f->size + f->padding_after);
                cg_str(g, ") */\n");
            }
        }
        if (d->trailing_pad > 0) {
            cg_str(g, "    uint8_t _weft_pad_end[");
            cg_u64(g, d->trailing_pad);
            cg_str(g, "];  /* trailing pad */\n");
        }
        if (lang == WEFT_LANG_C) {
            cg_str(g, "} ");
            cg_str(g, nm);
            cg_str(g, ";\n");
        } else {
            cg_str(g, "};\n");
        }
        cg_str(g, lang == WEFT_LANG_C ? "_Static_assert" : "static_assert");
        cg_str(g, "(sizeof(");
        cg_str(g, nm);
        cg_str(g, ") == ");
        cg_u64(g, d->size);
        cg_str(g, ", \"weft: ");
        cg_str(g, nm);
        cg_str(g, " size drifted\");\n");
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            cg_str(g, lang == WEFT_LANG_C ? "_Static_assert" : "static_assert");
            cg_str(g, "(offsetof(");
            cg_str(g, nm);
            cg_str(g, ", ");
            cg_str(g, fn);
            cg_str(g, ") == ");
            cg_u64(g, f->offset);
            cg_str(g, ", \"weft: ");
            cg_str(g, nm);
            cg_ch(g, '.');
            cg_str(g, fn);
            cg_str(g, " offset drifted\");\n");
        }
        cg_ch(g, '\n');
        return;
    }
    if (lang == WEFT_LANG_RUST) {
        cg_str(g, "/* struct ");
        cg_str(g, nm);
        cg_str(g, " - ");
        cg_u64(g, d->size);
        cg_str(g, " B, align ");
        cg_u64(g, d->align);
        cg_str(g, ", abi_hash ");
        cg_hex64(g, d->abi_hash);
        cg_str(g, " */\n");
        cg_str(g, "#[repr(C, align(");
        cg_u64(g, d->align);
        cg_str(g, "))]\n#[derive(Clone, Copy)]\npub struct ");
        cg_str(g, nm);
        if (d->nplanned == 0) {
            cg_str(g, ";\n\n");
            return;
        }
        cg_str(g, " {\n");
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            cg_str(g, "    pub ");
            cg_str(g, fn);
            cg_str(g, ": ");
            cg_rust_ty(g, c, f->ty_node);
            cg_str(g, ",  // offset ");
            cg_u64(g, f->offset);
            cg_str(g, ", size ");
            cg_u64(g, f->size);
            cg_ch(g, '\n');
            if (f->padding_after > 0) {
                cg_str(g, "    pub _weft_pad_");
                cg_u64(g, pad_i++);
                cg_str(g, ": [u8; ");
                cg_u64(g, f->padding_after);
                cg_str(g, "],  // hole\n");
            }
        }
        if (d->trailing_pad > 0) {
            cg_str(g, "    pub _weft_pad_end: [u8; ");
            cg_u64(g, d->trailing_pad);
            cg_str(g, "],  // trailing pad\n");
        }
        cg_str(g, "}\nconst _: () = assert!(core::mem::size_of::<");
        cg_str(g, nm);
        cg_str(g, ">() == ");
        cg_u64(g, d->size);
        cg_str(g, ");\nconst _: () = assert!(core::mem::align_of::<");
        cg_str(g, nm);
        cg_str(g, ">() == ");
        cg_u64(g, d->align);
        cg_str(g, ");\n\n");
        return;
    }
    if (lang == WEFT_LANG_TYPESCRIPT) {
        cg_str(g, "// struct ");
        cg_str(g, nm);
        cg_str(g, " - ");
        cg_u64(g, d->size);
        cg_str(g, " B, align ");
        cg_u64(g, d->align);
        cg_str(g, ", abi_hash ");
        cg_hex64(g, d->abi_hash);
        cg_str(g, "\nexport const ");
        {
            uint32_t k;
            for (k = 0; nm[k]; k++)
                cg_ch(g, (char)((nm[k] >= 'a' && nm[k] <= 'z')
                                ? nm[k] - 32 : nm[k]));
        }
        cg_str(g, "_LAYOUT = { size: ");
        cg_u64(g, d->size);
        cg_str(g, ", align: ");
        cg_u64(g, d->align);
        cg_str(g, ", fields: {\n");
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            cg_str(g, "  ");
            cg_str(g, fn);
            cg_str(g, ": { offset: ");
            cg_u64(g, f->offset);
            cg_str(g, ", size: ");
            cg_u64(g, f->size);
            cg_str(g, ", align: ");
            cg_u64(g, f->align);
            cg_str(g, ", type: '");
            cg_str(g, c->name_pool + f->tystr_pool);
            cg_str(g, "' },\n");
        }
        cg_str(g, "} } as const;\n\n");
        return;
    }
    if (lang == WEFT_LANG_PYTHON) {
        cg_str(g, "class ");
        cg_str(g, nm);
        cg_str(g, ":  # struct - ");
        cg_u64(g, d->size);
        cg_str(g, " B, align ");
        cg_u64(g, d->align);
        cg_str(g, ", abi_hash ");
        cg_hex64(g, d->abi_hash);
        cg_str(g, "\n    SIZE = ");
        cg_u64(g, d->size);
        cg_str(g, "\n    ALIGN = ");
        cg_u64(g, d->align);
        cg_str(g, "\n    FIELDS = (\n");
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            cg_str(g, "        (\"");
            cg_str(g, fn);
            cg_str(g, "\", ");
            cg_u64(g, f->offset);
            cg_str(g, ", ");
            cg_u64(g, f->size);
            cg_str(g, ", \"");
            cg_str(g, c->name_pool + f->tystr_pool);
            cg_str(g, "\"),\n");
        }
        cg_str(g, "    )\n    __slots__ = (");
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            if (s) cg_str(g, ", ");
            cg_ch(g, '"');
            cg_str(g, fn);
            cg_ch(g, '"');
        }
        cg_str(g, ")\n\n");
        return;
    }
    if (lang == WEFT_LANG_DART) {
        cg_str(g, "/// struct ");
        cg_str(g, nm);
        cg_str(g, " - ");
        cg_u64(g, d->size);
        cg_str(g, " B, align ");
        cg_u64(g, d->align);
        cg_str(g, ", abi_hash ");
        cg_hex64(g, d->abi_hash);
        cg_str(g, "\nclass ");
        cg_str(g, nm);
        cg_str(g, " {\n  static const int size = ");
        cg_u64(g, d->size);
        cg_str(g, ";\n  static const int align = ");
        cg_u64(g, d->align);
        cg_str(g, ";\n  static const Map<String, List<Object>> fields = {\n");
        for (s = 0; s < d->nplanned; s++) {
            const weftc_field_t *f =
                &c->fields[c->field_order[d->first_field + s]];
            const char *fn = c->name_pool + f->name_pool;
            if (f->size == 0) continue;
            cg_str(g, "    '");
            cg_str(g, fn);
            cg_str(g, "': [");
            cg_u64(g, f->offset);
            cg_str(g, ", ");
            cg_u64(g, f->size);
            cg_str(g, ", '");
            cg_str(g, c->name_pool + f->tystr_pool);
            cg_str(g, "'],\n");
        }
        cg_str(g, "  };\n}\n\n");
        return;
    }
    /* Swift */
    cg_str(g, "struct ");
    cg_str(g, nm);
    cg_str(g, " {  // ");
    cg_u64(g, d->size);
    cg_str(g, " B, align ");
    cg_u64(g, d->align);
    cg_str(g, ", abi_hash ");
    cg_hex64(g, d->abi_hash);
    cg_ch(g, '\n');
    if (d->nplanned == 0) {
        cg_str(g, "}\n\n");
        return;
    }
    for (s = 0; s < d->nplanned; s++) {
        const weftc_field_t *f =
            &c->fields[c->field_order[d->first_field + s]];
        const char *fn = c->name_pool + f->name_pool;
        if (f->size == 0) continue;
        cg_str(g, "    var ");
        cg_str(g, fn);
        cg_str(g, ": ");
        cg_swift_ty(g, c, f->ty_node);
        cg_str(g, "    // offset ");
        cg_u64(g, f->offset);
        cg_str(g, ", size ");
        cg_u64(g, f->size);
        cg_ch(g, '\n');
    }
    cg_str(g, "}\n");
    if (d->trailing_pad > 0) {
        cg_str(g, "// (trailing pad: ");
        cg_u64(g, d->trailing_pad);
        cg_str(g, " B)\n");
    }
    cg_ch(g, '\n');
}

static int cg_emit(weftc_ctx_t *c, int lang, char *out, size_t cap,
                   size_t *out_len)
{
    CG g;
    uint32_t di;
    if (lang < 0 || lang >= WEFT_LANG__COUNT) return WEFT_STUDIO_EBOUNDS;
    if (!out || cap == 0) return WEFT_STUDIO_EBOUNDS;
    g.buf = out; g.cap = cap; g.n = 0; g.of = 0;
    cg_header(&g, c, lang);
    for (di = 0; di < c->ndecls; di++) {
        if (c->decls[di].kind == DCL_STRUCT) continue;
        cg_enum(&g, c, di, lang);
    }
    for (di = 0; di < c->ndecls; di++)
        cg_struct(&g, c, di, lang);
    if (g.of) return WEFT_STUDIO_EBOUNDS;
    if (g.n + 1u > cap) return WEFT_STUDIO_EBOUNDS;
    out[g.n] = '\0';
    if (out_len) *out_len = g.n;
    return WEFT_STUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
size_t weftc_ctx_size(void)
{
    return sizeof(weftc_ctx_t) + 64u;
}

int weftc_ctx_init(void *mem, size_t mem_size, weftc_ctx_t **out)
{
    weftc_ctx_t *c;
    if (!mem || !out) return WEFT_STUDIO_EBOUNDS;
    if (mem_size < sizeof(weftc_ctx_t)) return WEFT_STUDIO_EBOUNDS;
    if (((uintptr_t)mem & 7u) != 0u) return WEFT_STUDIO_EALIGN;
    c = (weftc_ctx_t *)mem;
    memset(c, 0, sizeof *c);
    c->cache_line = 64;
    *out = c;
    return WEFT_STUDIO_OK;
}

void weftc_ctx_reset(weftc_ctx_t *ctx)
{
    uint8_t cl;
    if (!ctx) return;
    cl = ctx->cache_line;
    memset(ctx, 0, sizeof *ctx);
    ctx->cache_line = cl ? cl : 64;
}

static void ctx_soft_reset(weftc_ctx_t *c)
{
    c->ntoks = 0; c->nlines = 0; c->nnodes = 0; c->ndecls = 0;
    c->nfields = 0; c->nvariants = 0; c->nholes = 0;
    c->name_pool_n = 0; c->msg_pool_n = 0;
    c->nstaging = 0; c->diags_dropped = 0; c->ndiags = 0; c->nlayouts = 0;
    c->abi_hash = 0; c->schema_id = 0; c->fnv1a64_abi = 0;
    c->endian_big = 0; c->endianness_declared = 0;
    c->capacity_hit = 0; c->compiled = 0;
    c->pending_fix_valid = 0;
    memset(c->dstate, 0, WEFTC_MAX_DECLS);
}

int weftc_compile(weftc_ctx_t *ctx, const char *src, size_t src_len)
{
    Parser p;
    uint32_t i;
    if (!ctx) return WEFT_STUDIO_EBOUNDS;
    if (!src && src_len != 0) return WEFT_STUDIO_EBOUNDS;
    if (src_len > 0xFFFFFFFFull) return WEFT_STUDIO_EBOUNDS;
    ctx_soft_reset(ctx);
    ctx->src = src;
    ctx->src_len = src_len;
    lex_all(ctx, src, src_len);
    p.c = ctx;
    p.ti = 0;
    p.root = WEFT_AST_NONE;
    parse_schema(&p);
    check_variants(ctx);
    build_name_index(ctx);
    for (i = 0; i < ctx->ndecls; i++)
        layout_decl(ctx, i, 0);
    studio_layout_diags(ctx);
    compute_hashes(ctx);
    finalize_diags(ctx);
    finalize_layouts(ctx);
    ctx->compiled = 1;
    return ctx->capacity_hit ? WEFT_STUDIO_EBOUNDS : WEFT_STUDIO_OK;
}

uint32_t weftc_diag_count(const weftc_ctx_t *ctx)
{
    return ctx ? ctx->ndiags : 0;
}
const weft_diag_t *weftc_diag_at(const weftc_ctx_t *ctx, uint32_t i)
{
    if (!ctx || i >= ctx->ndiags) return NULL;
    return &ctx->diags[i];
}
uint32_t weftc_decl_count(const weftc_ctx_t *ctx)
{
    return ctx ? ctx->nlayouts : 0;
}
const weft_struct_layout_t *weftc_decl_at(const weftc_ctx_t *ctx, uint32_t i)
{
    if (!ctx || i >= ctx->nlayouts) return NULL;
    return &ctx->layouts[i];
}
int32_t weftc_decl_find(const weftc_ctx_t *ctx, const char *name)
{
    uint32_t i;
    if (!ctx || !name) return -1;
    for (i = 0; i < ctx->nlayouts; i++) {
        const char *a = ctx->layouts[i].name;
        const char *b = name;
        size_t k = 0;
        if (!a) continue;
        for (;;) {
            if (a[k] != b[k]) break;
            if (a[k] == 0) return (int32_t)i;
            k++;
        }
    }
    return -1;
}
uint32_t weftc_ast_count(const weftc_ctx_t *ctx)
{
    return ctx ? ctx->nnodes : 0;
}
const weft_ast_node_t *weftc_ast_at(const weftc_ctx_t *ctx, uint32_t i)
{
    if (!ctx || i >= ctx->nnodes) return NULL;
    return &ctx->nodes[i];
}
int weftc_schema_hashes(const weftc_ctx_t *ctx, uint64_t *abi_hash,
                        uint64_t *schema_id, uint64_t *fnv1a64_abi)
{
    if (!ctx || !ctx->compiled) return WEFT_STUDIO_EPARSE;
    if (abi_hash) *abi_hash = ctx->abi_hash;
    if (schema_id) *schema_id = ctx->schema_id;
    if (fnv1a64_abi) *fnv1a64_abi = ctx->fnv1a64_abi;
    return WEFT_STUDIO_OK;
}
int weftc_endianness_big(const weftc_ctx_t *ctx)
{
    return (ctx && ctx->compiled && ctx->endian_big) ? 1 : 0;
}
int weftc_codegen(const weftc_ctx_t *ctx, int lang, char *out,
                  size_t cap, size_t *out_len)
{
    if (!ctx || !ctx->compiled) return WEFT_STUDIO_EPARSE;
    return cg_emit((weftc_ctx_t *)ctx, lang, out, cap, out_len);
}
