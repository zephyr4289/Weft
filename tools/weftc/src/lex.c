/* lex.c — lexer for the .weft schema language (RFC-0017 §3.1).
 *
 * Tokens carry byte spans; text is arena-owned and NUL-terminated.
 * The lexer never aborts: bad characters become TOK_BADCHAR tokens with
 * a WE023 diagnostic and scanning continues (Law 2 — resilient).
 *
 * Integer literals: decimal, 0x hex, 0b binary; '_' digit separators
 * (0x8000_0000). Overflow saturates at U64_MAX with WE021 (downstream
 * bounds checks then reject the value explicitly).
 */
#include "weftc.h"

#include <string.h>

const char *tok_kind_str(TokKind k)
{
    switch (k) {
    case TOK_EOF:        return "end of file";
    case TOK_IDENT:      return "identifier";
    case TOK_INT:        return "integer literal";
    case TOK_BADCHAR:    return "invalid character";
    case TOK_KW_STRUCT:  return "'struct'";
    case TOK_KW_ENUM:    return "'enum'";
    case TOK_KW_BITFLAGS:return "'bitflags'";
    case TOK_KW_SPAN:    return "'span'";
    case TOK_KW_STR:     return "'str'";
    case TOK_KW_ENDIANNESS: return "'endianness'";
    case TOK_KW_LITTLE:  return "'little'";
    case TOK_KW_BIG:     return "'big'";
    case TOK_KW_PACKING: return "'packing'";
    case TOK_KW_U8:      return "'u8'";
    case TOK_KW_I8:      return "'i8'";
    case TOK_KW_U16:     return "'u16'";
    case TOK_KW_I16:     return "'i16'";
    case TOK_KW_U32:     return "'u32'";
    case TOK_KW_I32:     return "'i32'";
    case TOK_KW_U64:     return "'u64'";
    case TOK_KW_I64:     return "'i64'";
    case TOK_KW_F16:     return "'f16'";
    case TOK_KW_F32:     return "'f32'";
    case TOK_KW_F64:     return "'f64'";
    case TOK_KW_BOOL:    return "'bool'";
    case TOK_LBRACE:     return "'{'";
    case TOK_RBRACE:     return "'}'";
    case TOK_LBRACKET:   return "'['";
    case TOK_RBRACKET:   return "']'";
    case TOK_LPAREN:     return "'('";
    case TOK_RPAREN:     return "')'";
    case TOK_LANG:       return "'<'";
    case TOK_RANG:       return "'>'";
    case TOK_COLON:      return "':'";
    case TOK_SEMI:       return "';'";
    case TOK_COMMA:      return "','";
    case TOK_EQ:         return "'='";
    case TOK_AT:         return "'@'";
    case TOK_MINUS:      return "'-'";
    }
    return "?";
}

typedef struct { const char *kw; TokKind kind; } KwEntry;

static const KwEntry KEYWORDS[] = {
    { "struct",     TOK_KW_STRUCT },
    { "enum",       TOK_KW_ENUM },
    { "bitflags",   TOK_KW_BITFLAGS },
    { "span",       TOK_KW_SPAN },
    { "str",        TOK_KW_STR },
    { "endianness", TOK_KW_ENDIANNESS },
    { "little",     TOK_KW_LITTLE },
    { "big",        TOK_KW_BIG },
    { "packing",    TOK_KW_PACKING },
    { "u8",  TOK_KW_U8 },  { "i8",  TOK_KW_I8 },
    { "u16", TOK_KW_U16 }, { "i16", TOK_KW_I16 },
    { "u32", TOK_KW_U32 }, { "i32", TOK_KW_I32 },
    { "u64", TOK_KW_U64 }, { "i64", TOK_KW_I64 },
    { "f16", TOK_KW_F16 }, { "f32", TOK_KW_F32 },
    { "f64", TOK_KW_F64 }, { "bool", TOK_KW_BOOL },
    { NULL, (TokKind)0 },
};

static int is_ident_start(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident_cont(uint8_t c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

void lex_init(Lexer *lx, const SourceFile *sf, Arena *ar, DiagSink *sink)
{
    lx->sf = sf;
    lx->ar = ar;
    lx->sink = sink;
    lx->pos = 0;
    lx->cur.kind = TOK_EOF;
    lx->cur.span.off = lx->cur.span.len = 0;
    lx->cur.text = "";
    lx->cur.ival = 0;
    lex_advance(lx);
}

/* Skips whitespace and comments; returns 0 normally, 1 at EOF. */
static int skip_trivia(Lexer *lx)
{
    const char *s = lx->sf->src;
    size_t n = lx->sf->len;
    for (;;) {
        while (lx->pos < n && (s[lx->pos] == ' ' || s[lx->pos] == '\t' ||
                               s[lx->pos] == '\r' || s[lx->pos] == '\n'))
            lx->pos++;
        if (lx->pos + 1 < n && s[lx->pos] == '/' && s[lx->pos + 1] == '/') {
            while (lx->pos < n && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (lx->pos + 1 < n && s[lx->pos] == '/' && s[lx->pos + 1] == '*') {
            size_t start = lx->pos;
            lx->pos += 2;
            int closed = 0;
            while (lx->pos < n) {
                if (s[lx->pos] == '*' && lx->pos + 1 < n && s[lx->pos + 1] == '/') {
                    lx->pos += 2;
                    closed = 1;
                    break;
                }
                lx->pos++;
            }
            if (!closed) {
                diag_error(lx->sink, "WE022",
                           (Span){ start, 2 },
                           "unterminated block comment");
                return 1;
            }
            continue;
        }
        return lx->pos >= n;
    }
}

/* Scans an integer literal at lx->pos. */
static uint64_t scan_int(Lexer *lx, size_t start, int *overflow)
{
    const char *s = lx->sf->src;
    size_t n = lx->sf->len;
    int base = 10;
    size_t p = lx->pos;
    if (p + 1 < n && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) {
        base = 16; p += 2;
    } else if (p + 1 < n && s[p] == '0' && (s[p + 1] == 'b' || s[p + 1] == 'B')) {
        base = 2; p += 2;
    }
    *overflow = 0;
    int any = 0;
    uint64_t v = 0;
    while (p < n) {
        uint8_t c = (uint8_t)s[p];
        if (c == '_') { p++; continue; }
        int d;
        if (is_digit(c)) d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        any = 1;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
            *overflow = 1;
            v = UINT64_MAX;
        } else if (!*overflow) {
            v = v * (uint64_t)base + (uint64_t)d;
        }
        p++;
    }
    if (!any && base != 10) {
        /* "0x" / "0b" prefix with no digits — consume and report */
        diag_error(lx->sink, "WE041",
                   (Span){ start, p - start },
                   "malformed integer literal: base prefix with no digits");
    }
    (void)start;
    lx->pos = p;
    return v;
}

void lex_advance(Lexer *lx)
{
    if (lx->cur.kind == TOK_EOF) {
        /* allow re-init but nothing to do */
    }
    if (skip_trivia(lx)) {
        lx->cur.kind = TOK_EOF;
        lx->cur.span.off = lx->sf->len;
        lx->cur.span.len = 0;
        lx->cur.text = "";
        lx->cur.ival = 0;
        return;
    }
    const char *s = lx->sf->src;
    size_t n = lx->sf->len;
    size_t start = lx->pos;
    uint8_t c = (uint8_t)s[start];

    Token t;
    t.kind = TOK_BADCHAR;
    t.span.off = start;
    t.span.len = 1;
    t.text = "";
    t.ival = 0;

    if (is_ident_start(c)) {
        size_t p = start + 1;
        while (p < n && is_ident_cont((uint8_t)s[p])) p++;
        t.span.len = p - start;
        t.text = arena_dupn(lx->ar, s + start, p - start);
        t.kind = TOK_IDENT;
        for (const KwEntry *e = KEYWORDS; e->kw; e++) {
            if (strcmp(e->kw, t.text) == 0) { t.kind = e->kind; break; }
        }
        lx->pos = p;
    } else if (is_digit(c)) {
        int overflow = 0;
        t.ival = scan_int(lx, start, &overflow);
        t.kind = TOK_INT;
        t.span.len = lx->pos - start;
        t.text = "";
        if (overflow) {
            diag_error(lx->sink, "WE021", t.span,
                       "integer literal exceeds the u64 range");
        }
    } else {
        lx->pos = start + 1;
        switch (c) {
        case '{': t.kind = TOK_LBRACE; break;
        case '}': t.kind = TOK_RBRACE; break;
        case '[': t.kind = TOK_LBRACKET; break;
        case ']': t.kind = TOK_RBRACKET; break;
        case '(': t.kind = TOK_LPAREN; break;
        case ')': t.kind = TOK_RPAREN; break;
        case '<': t.kind = TOK_LANG; break;
        case '>': t.kind = TOK_RANG; break;
        case ':': t.kind = TOK_COLON; break;
        case ';': t.kind = TOK_SEMI; break;
        case ',': t.kind = TOK_COMMA; break;
        case '=': t.kind = TOK_EQ; break;
        case '@': t.kind = TOK_AT; break;
        case '-': t.kind = TOK_MINUS; break;
        default:
            if (c < 0x20 || c > 0x7E) {
                char buf[8];
                snprintf(buf, sizeof buf, "'\\x%02x'", c);
                t.text = arena_dup(lx->ar, buf);
            } else {
                char buf[3] = { '`', 0, 0 };
                buf[1] = (char)c;
                t.text = arena_dup(lx->ar, buf);
            }
            diag_error(lx->sink, "WE023", t.span,
                       "invalid character %s in schema", t.text);
            t.kind = TOK_BADCHAR;
            break;
        }
    }
    lx->cur = t;
}

/* ------------------------------------------------------------------ */
/* Primitive table (sizes/aligns are the layout contract, RFC §4.1)    */
/* ------------------------------------------------------------------ */
static const PrimInfo PRIMS[PRIM__COUNT] = {
    [PRIM_NONE] = { "none", 0, 0, 0, 0 },
    [PRIM_U8]    = { "u8",    1, 1, 1, 0 },
    [PRIM_I8]    = { "i8",    1, 1, 1, 1 },
    [PRIM_U16]   = { "u16",   2, 2, 1, 0 },
    [PRIM_I16]   = { "i16",   2, 2, 1, 1 },
    [PRIM_U32]   = { "u32",   4, 4, 1, 0 },
    [PRIM_I32]   = { "i32",   4, 4, 1, 1 },
    [PRIM_U64]   = { "u64",   8, 8, 1, 0 },
    [PRIM_I64]   = { "i64",   8, 8, 1, 1 },
    [PRIM_F16]   = { "f16",   2, 2, 0, 0 },
    [PRIM_F32]   = { "f32",   4, 4, 0, 0 },
    [PRIM_F64]   = { "f64",   8, 8, 0, 0 },
    [PRIM_BOOL]  = { "bool",  1, 1, 1, 0 },
};

const PrimInfo *prim_info(PrimKind k)
{
    if (k <= PRIM_NONE || k >= PRIM__COUNT) return NULL;
    return &PRIMS[k];
}
