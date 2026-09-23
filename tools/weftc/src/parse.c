/* parse.c — recursive-descent parser for the .weft schema language
 * (RFC-0017 §3.2). Panic-mode recovery: on a syntax error we skip to a
 * synchronization token (',' '}' ';' or a decl keyword) so ONE run
 * reports as many real errors as possible — the "zero compiler crashes,
 * resilient diagnostics" law (L2).
 *
 * Attribute *names and arities* are resolved here (purely lexical);
 * placement, conflicts and numeric validity are validated by the
 * semantic pass in layout.c. Types that fail to parse yield a
 * placeholder `u8` and the owning field is marked poisoned so semantic
 * checks do not cascade.
 */
#include "weftc.h"

#include <string.h>

typedef struct {
    Lexer    lx;
    DiagSink *sink;
    Arena    *ar;
} Parser;

static Token cur(Parser *p) { return p->lx.cur; }
static void  adv(Parser *p) { lex_advance(&p->lx); }

#define SPAN0 ((Span){ 0, 0 })

/* arena-growing array push */
#define PUSH(arr, n, cap, ty, v) do {                                      \
    if ((n) == (cap)) {                                                    \
        size_t nc_ = (cap) ? (size_t)(cap) * 2 : 4;                        \
        ty *np_ = arena_alloc(p->ar, nc_ * sizeof(ty), 16);                \
        if (n) memcpy(np_, (arr), (size_t)(n) * sizeof(ty));               \
        (arr) = np_; (cap) = (int)nc_;                                     \
    }                                                                      \
    (arr)[(n)++] = (v);                                                    \
} while (0)

static void diag_expected(Parser *p, const char *what)
{
    Token t = cur(p);
    diag_error(p->sink, "WE019", t.span, "expected %s, found %s",
               what, tok_kind_str(t.kind));
}

/* Skip until one of ',' '}' ';' EOF (does not consume it). */
static void sync_field(Parser *p)
{
    for (;;) {
        Token t = cur(p);
        if (t.kind == TOK_COMMA || t.kind == TOK_RBRACE ||
            t.kind == TOK_SEMI || t.kind == TOK_EOF)
            return;
        adv(p);
    }
}

/* Skip until a token that can start a top-level item (or EOF). Braces
 * are skipped balanced so a stray '{...}' block does not swallow the
 * rest of the file. */
static void sync_top(Parser *p)
{
    int depth = 0;
    for (;;) {
        Token t = cur(p);
        if (t.kind == TOK_EOF) return;
        if (depth == 0 &&
            (t.kind == TOK_KW_STRUCT || t.kind == TOK_KW_ENUM ||
             t.kind == TOK_KW_BITFLAGS || t.kind == TOK_KW_ENDIANNESS ||
             t.kind == TOK_AT))
            return;
        if (t.kind == TOK_LBRACE) depth++;
        if (t.kind == TOK_RBRACE && depth > 0) depth--;
        adv(p);
    }
}

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
static PrimKind prim_from_tok(TokKind k)
{
    switch (k) {
    case TOK_KW_U8:   return PRIM_U8;
    case TOK_KW_I8:   return PRIM_I8;
    case TOK_KW_U16:  return PRIM_U16;
    case TOK_KW_I16:  return PRIM_I16;
    case TOK_KW_U32:  return PRIM_U32;
    case TOK_KW_I32:  return PRIM_I32;
    case TOK_KW_U64:  return PRIM_U64;
    case TOK_KW_I64:  return PRIM_I64;
    case TOK_KW_F16:  return PRIM_F16;
    case TOK_KW_F32:  return PRIM_F32;
    case TOK_KW_F64:  return PRIM_F64;
    case TOK_KW_BOOL: return PRIM_BOOL;
    default:          return PRIM_NONE;
    }
}

static Ty *ty_new(Parser *p, Ty t)
{
    Ty *ty = arena_alloc(p->ar, sizeof(Ty), 8);
    *ty = t;
    return ty;
}

static Ty *ty_placeholder(Parser *p, Span sp)
{
    Ty t;
    memset(&t, 0, sizeof t);
    t.kind = TY_PRIM;
    t.prim = PRIM_U8;
    t.span = sp;
    return ty_new(p, t);
}

/* Validates a fixed length literal ([T; N] / str[N]); returns the value
 * (or 1 on error, with *poisoned set). */
static uint64_t check_fixed_len(Parser *p, Token num, bool *poisoned)
{
    *poisoned = false;
    if (num.kind != TOK_INT) {
        diag_expected(p, "an integer literal");
        *poisoned = true;
        return 1;
    }
    if (num.ival == 0) {
        diag_error(p->sink, "WE012", num.span,
                   "fixed length must be at least 1 (use span<T> for "
                   "variable-length content)");
        *poisoned = true;
        return 1;
    }
    if (num.ival > WEFTC_MAX_FIXED_LEN) {
        diag_error(p->sink, "WE029", num.span,
                   "fixed length %llu exceeds the u32 limit (2^32-1)",
                   (unsigned long long)num.ival);
        *poisoned = true;
        return 1;
    }
    return num.ival;
}

static Ty *parse_type(Parser *p, int depth, bool *poisoned)
{
    Token t = cur(p);
    if (depth > WEFTC_MAX_TYPE_DEPTH) {
        diag_error(p->sink, "WE027", t.span,
                   "type nesting exceeds the maximum depth of %d",
                   WEFTC_MAX_TYPE_DEPTH);
        *poisoned = true;
        return ty_placeholder(p, t.span);
    }
    PrimKind pk = prim_from_tok(t.kind);
    if (pk != PRIM_NONE) {
        adv(p);
        Ty ty;
        memset(&ty, 0, sizeof ty);
        ty.kind = TY_PRIM;
        ty.prim = pk;
        ty.span = t.span;
        return ty_new(p, ty);
    }
    switch (t.kind) {
    case TOK_IDENT: {
        adv(p);
        Ty ty;
        memset(&ty, 0, sizeof ty);
        ty.kind = TY_NAMED;
        ty.name = t.text;
        ty.span = t.span;
        ty.decl_idx = -1; /* resolved by the semantic pass (layout.c) */
        return ty_new(p, ty);
    }
    case TOK_LBRACKET: {
        adv(p);
        Ty *elem = parse_type(p, depth + 1, poisoned);
        if (cur(p).kind != TOK_SEMI) {
            diag_expected(p, "';'");
            *poisoned = true;
        } else {
            adv(p);
        }
        Token num = cur(p);
        uint64_t len = check_fixed_len(p, num, poisoned);
        if (num.kind == TOK_INT) adv(p);
        if (cur(p).kind != TOK_RBRACKET) {
            diag_expected(p, "']'");
            *poisoned = true;
        } else {
            adv(p);
        }
        Ty ty;
        memset(&ty, 0, sizeof ty);
        ty.kind = TY_ARRAY;
        ty.elem = elem;
        ty.n = len;
        ty.span = t.span;
        return ty_new(p, ty);
    }
    case TOK_KW_STR: {
        adv(p);
        if (cur(p).kind != TOK_LBRACKET) {
            diag_expected(p, "'['");
            *poisoned = true;
            return ty_placeholder(p, t.span);
        }
        adv(p);
        Token num = cur(p);
        uint64_t cap_len = check_fixed_len(p, num, poisoned);
        if (num.kind == TOK_INT) adv(p);
        if (cur(p).kind != TOK_RBRACKET) {
            diag_expected(p, "']'");
            *poisoned = true;
        } else {
            adv(p);
        }
        Ty ty;
        memset(&ty, 0, sizeof ty);
        ty.kind = TY_STR;
        ty.n = cap_len;
        ty.span = t.span;
        return ty_new(p, ty);
    }
    case TOK_KW_SPAN: {
        adv(p);
        if (cur(p).kind != TOK_LANG) {
            diag_expected(p, "'<'");
            *poisoned = true;
            return ty_placeholder(p, t.span);
        }
        adv(p);
        Ty *elem = parse_type(p, depth + 1, poisoned);
        if (elem->kind == TY_SPAN) {
            diag_error(p->sink, "WE024", elem->span,
                       "span element type must not be `span` (a span "
                       "points into a buffer; it cannot nest)");
            *poisoned = true;
        }
        if (cur(p).kind != TOK_RANG) {
            diag_expected(p, "'>'");
            *poisoned = true;
        } else {
            adv(p);
        }
        Ty ty;
        memset(&ty, 0, sizeof ty);
        ty.kind = TY_SPAN;
        ty.elem = elem;
        ty.span = t.span;
        return ty_new(p, ty);
    }
    default:
        diag_error(p->sink, "WE020", t.span,
                   "unexpected %s where a type was expected",
                   tok_kind_str(t.kind));
        *poisoned = true;
        return ty_placeholder(p, t.span);
    }
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */
/* Grammar:  @ident  |  @ident ( int )  |  @optimize ( packing )
 * Names/arity resolved here; value semantics live in layout.c. */
static Attr *parse_attrs(Parser *p, size_t *nattrs_out)
{
    Attr *attrs = NULL;
    int n = 0, cap = 0;
    while (cur(p).kind == TOK_AT) {
        Token at = cur(p);
        adv(p);
        Token name = cur(p);
        if (name.kind != TOK_IDENT) {
            diag_expected(p, "an attribute name");
            sync_field(p);
            break;
        }
        adv(p);
        Attr a;
        memset(&a, 0, sizeof a);
        a.name = name.text;
        a.span.off = at.span.off;
        if (strcmp(name.text, "align") == 0 || strcmp(name.text, "simd") == 0) {
            a.kind = (name.text[0] == 'a') ? ATTR_ALIGN : ATTR_SIMD;
            if (cur(p).kind != TOK_LPAREN) {
                diag_error(p->sink, "WE002", name.span,
                           "attribute `@%s` requires an integer argument, "
                           "e.g. @%s(64)", name.text, name.text);
                continue;
            }
            adv(p);
            Token num = cur(p);
            if (num.kind != TOK_INT) {
                diag_error(p->sink, "WE032", num.span,
                           "attribute `@%s` requires an integer argument",
                           name.text);
                /* skip to ')' */
                while (cur(p).kind != TOK_RPAREN && cur(p).kind != TOK_EOF)
                    adv(p);
                if (cur(p).kind == TOK_RPAREN) adv(p);
                continue;
            }
            adv(p);
            if (cur(p).kind != TOK_RPAREN) {
                diag_expected(p, "')'");
            } else {
                adv(p);
            }
            a.num = num.ival;
            a.has_num = true;
            a.span.len = num.span.off + num.span.len - at.span.off;
        } else if (strcmp(name.text, "packed") == 0) {
            a.kind = ATTR_PACKED;
            if (cur(p).kind == TOK_LPAREN) {
                diag_error(p->sink, "WE003", name.span,
                           "attribute `@packed` does not take an argument");
                while (cur(p).kind != TOK_RPAREN && cur(p).kind != TOK_EOF)
                    adv(p);
                if (cur(p).kind == TOK_RPAREN) adv(p);
                continue;
            }
            a.span.len = name.span.off + name.span.len - at.span.off;
        } else if (strcmp(name.text, "optimize") == 0) {
            a.kind = ATTR_OPT_PACK;
            if (cur(p).kind != TOK_LPAREN) {
                diag_error(p->sink, "WE002", name.span,
                           "attribute `@optimize` requires the argument "
                           "`packing`: @optimize(packing)");
                continue;
            }
            adv(p);
            Token arg = cur(p);
            /* `packing` is a keyword (TOK_KW_PACKING) — accept either the
             * keyword token or a plain identifier with that text */
            if ((arg.kind != TOK_IDENT && arg.kind != TOK_KW_PACKING) ||
                strcmp(arg.text, "packing") != 0) {
                diag_error(p->sink, "WE032", arg.span,
                           "expected `packing` — the only @optimize "
                           "strategy in weft-schema-v1");
                while (cur(p).kind != TOK_RPAREN && cur(p).kind != TOK_EOF)
                    adv(p);
                if (cur(p).kind == TOK_RPAREN) adv(p);
                continue;
            }
            adv(p);
            if (cur(p).kind != TOK_RPAREN) {
                diag_expected(p, "')'");
            } else {
                adv(p);
            }
            a.span.len = arg.span.off + arg.span.len - at.span.off;
        } else {
            diag_error(p->sink, "WE001", name.span,
                       "unknown attribute `@%s`", name.text);
            /* tolerate an argument so parsing continues */
            if (cur(p).kind == TOK_LPAREN) {
                while (cur(p).kind != TOK_RPAREN && cur(p).kind != TOK_EOF)
                    adv(p);
                if (cur(p).kind == TOK_RPAREN) adv(p);
            }
            continue;
        }
        PUSH(attrs, n, cap, Attr, a);
    }
    *nattrs_out = (size_t)n;
    return attrs;
}

/* ------------------------------------------------------------------ */
/* Declarations                                                        */
/* ------------------------------------------------------------------ */
static void parse_struct(Parser *p, Attr *attrs, size_t nattrs, Decl *d)
{
    Token name = cur(p);
    if (name.kind != TOK_IDENT) {
        diag_expected(p, "a struct name");
        d->poisoned = true;
        sync_top(p);
        return;
    }
    adv(p);
    d->name = name.text;
    d->span = name.span;
    if (cur(p).kind != TOK_LBRACE) {
        diag_expected(p, "'{'");
        d->poisoned = true;
        sync_top(p);
        return;
    }
    adv(p);
    Field *fields = NULL;
    int nf = 0, fcap = 0;
    int closed = 0;
    for (;;) {
        Token t = cur(p);
        if (t.kind == TOK_RBRACE) { closed = 1; break; }
        if (t.kind == TOK_EOF) break;
        size_t nfa = 0;
        Attr *fattrs = parse_attrs(p, &nfa);
        Token fname = cur(p);
        if (fname.kind != TOK_IDENT) {
            diag_expected(p, "a field name");
            if (nfa > 0)
                diag_note(p->sink, "attributes must annotate a field or a "
                          "declaration");
            sync_field(p);
            if (cur(p).kind == TOK_COMMA) adv(p);
            else if (cur(p).kind == TOK_RBRACE) break;
            else if (cur(p).kind == TOK_EOF) break;
            continue;
        }
        adv(p);
        Field f;
        memset(&f, 0, sizeof f);
        f.name = fname.text;
        f.span = fname.span;
        f.attrs = fattrs;
        f.nattrs = nfa;
        if (cur(p).kind != TOK_COLON) {
            diag_expected(p, "':'");
            f.poisoned = true;
            sync_field(p);
        } else {
            adv(p);
            f.ty = parse_type(p, 0, &f.poisoned);
        }
        PUSH(fields, nf, fcap, Field, f);
        if (cur(p).kind == TOK_COMMA) {
            adv(p);
        } else if (cur(p).kind != TOK_RBRACE && cur(p).kind != TOK_EOF) {
            diag_error(p->sink, "WE019", cur(p).span,
                       "expected ',' or '}' after field, found %s",
                       tok_kind_str(cur(p).kind));
            sync_field(p);
            if (cur(p).kind == TOK_COMMA) adv(p);
        }
    }
    if (!closed)
        diag_error(p->sink, "WE019", cur(p).span,
                   "expected '}' before end of file (struct `%s` is not "
                   "closed)", d->name ? d->name : "?");
    if (cur(p).kind == TOK_RBRACE) adv(p);
    if (cur(p).kind == TOK_SEMI) adv(p); /* optional C-style ';' */
    d->fields = fields;
    d->nfields = (size_t)nf;
    (void)attrs; (void)nattrs;
}

static void parse_enum_like(Parser *p, Attr *attrs, size_t nattrs, Decl *d)
{
    Token name = cur(p);
    if (name.kind != TOK_IDENT) {
        diag_expected(p, "a name");
        d->poisoned = true;
        sync_top(p);
        return;
    }
    adv(p);
    d->name = name.text;
    d->span = name.span;
    if (cur(p).kind != TOK_COLON) {
        diag_expected(p, "':' followed by a backing type");
        d->poisoned = true;
        sync_top(p);
        return;
    }
    adv(p);
    Token btok = cur(p);
    PrimKind pk = prim_from_tok(btok.kind);
    const PrimInfo *pi = prim_info(pk);
    if (pk == PRIM_NONE) {
        diag_error(p->sink, "WE017", btok.span,
                   "backing type must be an integer primitive "
                   "(u8..u64 / i8..i64), found %s", tok_kind_str(btok.kind));
        d->poisoned = true;
        sync_top(p);
        return;
    }
    if (!pi->is_int || pk == PRIM_BOOL) {
        diag_error(p->sink, "WE017", btok.span,
                   "backing type must be an integer primitive "
                   "(u8..u64 / i8..i64), found `%s`", pi->name);
        d->poisoned = true;
        adv(p);
    } else {
        if (d->kind == DECL_BITFLAGS && pi->is_signed) {
            diag_error(p->sink, "WE016", btok.span,
                       "bitflags backing type must be unsigned, got `%s`",
                       pi->name);
            d->poisoned = true;
        }
        d->backing = pk;
        adv(p);
    }
    if (cur(p).kind != TOK_LBRACE) {
        diag_expected(p, "'{'");
        if (!d->poisoned) d->poisoned = true;
        sync_top(p);
        return;
    }
    adv(p);
    Variant *vars = NULL;
    int nv = 0, vcap = 0;
    int closed = 0;
    for (;;) {
        Token t = cur(p);
        if (t.kind == TOK_RBRACE) { closed = 1; break; }
        if (t.kind == TOK_EOF) break;
        Token vname = cur(p);
        if (vname.kind != TOK_IDENT) {
            diag_expected(p, "a variant name");
            sync_field(p);
            if (cur(p).kind == TOK_COMMA) adv(p);
            else if (cur(p).kind == TOK_RBRACE) break;
            else if (cur(p).kind == TOK_EOF) break;
            continue;
        }
        adv(p);
        if (cur(p).kind != TOK_EQ) {
            diag_expected(p, "'='");
            Variant v;
            memset(&v, 0, sizeof v);
            v.name = vname.text;
            v.span = vname.span;
            v.value = 0;
            PUSH(vars, nv, vcap, Variant, v);
            sync_field(p);
            if (cur(p).kind == TOK_COMMA) adv(p);
            continue;
        }
        adv(p);
        int neg = 0;
        if (cur(p).kind == TOK_MINUS) {
            neg = 1;
            adv(p);
        }
        Token num = cur(p);
        if (num.kind != TOK_INT) {
            diag_expected(p, "an integer value");
            Variant v;
            memset(&v, 0, sizeof v);
            v.name = vname.text;
            v.span = vname.span;
            v.value = 0;
            PUSH(vars, nv, vcap, Variant, v);
            sync_field(p);
            if (cur(p).kind == TOK_COMMA) adv(p);
            continue;
        }
        adv(p);
        Variant v;
        memset(&v, 0, sizeof v);
        v.name = vname.text;
        v.span = vname.span;
        uint64_t raw = num.ival;
        if (neg) {
            if (raw > (uint64_t)INT64_MAX + 1ull) {
                diag_error(p->sink, "WE021", num.span,
                           "negated integer literal exceeds the i64 range");
                d->poisoned = true;
            }
            v.value = (raw == (uint64_t)INT64_MAX + 1ull)
                      ? INT64_MIN : -(int64_t)raw;
            if (d->kind == DECL_BITFLAGS) {
                diag_error(p->sink, "WE031", num.span,
                           "bitflags values must be non-negative");
            }
        } else {
            if (raw > (uint64_t)INT64_MAX) {
                /* keep raw bits; fit check against the backing type will
                 * reject it for any real backing (max u64 would overflow
                 * first, but WE021 already flagged the literal) */
                v.value = (int64_t)raw;
            } else {
                v.value = (int64_t)raw;
            }
        }
        PUSH(vars, nv, vcap, Variant, v);
        if (cur(p).kind == TOK_COMMA) adv(p);
        else if (cur(p).kind != TOK_RBRACE && cur(p).kind != TOK_EOF) {
            diag_error(p->sink, "WE019", cur(p).span,
                       "expected ',' or '}' after variant, found %s",
                       tok_kind_str(cur(p).kind));
            sync_field(p);
            if (cur(p).kind == TOK_COMMA) adv(p);
        }
    }
    if (!closed)
        diag_error(p->sink, "WE019", cur(p).span,
                   "expected '}' before end of file (`%s` is not closed)",
                   d->name ? d->name : "?");
    if (cur(p).kind == TOK_RBRACE) adv(p);
    if (cur(p).kind == TOK_SEMI) adv(p);
    d->variants = vars;
    d->nvariants = (size_t)nv;
    (void)attrs; (void)nattrs;
}

void parse_schema(const SourceFile *sf, Arena *ar, DiagSink *sink,
                  Decl **decls_out, size_t *ndecls_out, int *endian_big_out)
{
    Parser pst;
    Parser *p = &pst;
    p->sink = sink;
    p->ar = ar;
    lex_init(&p->lx, sf, ar, sink);

    Decl *decls = NULL;
    int nd = 0, dcap = 0;
    *endian_big_out = 0;
    int endian_seen = 0;
    Span endian_span = SPAN0;

    for (;;) {
        Token t = cur(p);
        if (t.kind == TOK_EOF) break;
        if (t.kind == TOK_KW_ENDIANNESS) {
            adv(p);
            Token val = cur(p);
            if (val.kind != TOK_KW_LITTLE && val.kind != TOK_KW_BIG) {
                diag_error(sink, "WE028", val.span,
                           "endianness must be `little` or `big`, found %s",
                           tok_kind_str(val.kind));
                sync_top(p);
                continue;
            }
            adv(p);
            if (cur(p).kind != TOK_SEMI) {
                diag_expected(p, "';'");
            } else {
                adv(p);
            }
            if (endian_seen) {
                diag_error(sink, "WE018", val.span,
                           "endianness already declared");
                diag_sec(sink, endian_span, "previous declaration here");
                continue;
            }
            endian_seen = 1;
            endian_span = val.span;
            *endian_big_out = (val.kind == TOK_KW_BIG);
            continue;
        }
        if (t.kind == TOK_AT || t.kind == TOK_KW_STRUCT ||
            t.kind == TOK_KW_ENUM || t.kind == TOK_KW_BITFLAGS) {
            size_t nattrs = 0;
            Attr *attrs = parse_attrs(p, &nattrs);
            Token kw = cur(p);
            Decl d;
            memset(&d, 0, sizeof d);
            d.attrs = attrs;
            d.nattrs = nattrs;
            d.index = (size_t)nd;
            if (kw.kind == TOK_KW_STRUCT) {
                adv(p);
                d.kind = DECL_STRUCT;
                parse_struct(p, attrs, nattrs, &d);
            } else if (kw.kind == TOK_KW_ENUM || kw.kind == TOK_KW_BITFLAGS) {
                adv(p);
                d.kind = (kw.kind == TOK_KW_ENUM) ? DECL_ENUM : DECL_BITFLAGS;
                parse_enum_like(p, attrs, nattrs, &d);
            } else {
                diag_error(sink, "WE019", kw.span,
                           "expected 'struct', 'enum' or 'bitflags' after "
                           "attributes, found %s", tok_kind_str(kw.kind));
                sync_top(p);
                continue;
            }
            PUSH(decls, nd, dcap, Decl, d);
            continue;
        }
        diag_error(sink, "WE020", t.span,
                   "unexpected %s at top level (expected a declaration)",
                   tok_kind_str(t.kind));
        sync_top(p);
    }

    *decls_out = decls;
    *ndecls_out = (size_t)nd;
}
