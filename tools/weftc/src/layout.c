/* layout.c — semantic analysis + the deterministic layout engine
 * (RFC-0017 §4). This is the heart of weftc: it turns the parsed AST into
 * byte-exact, frozen StructLayouts — the contract Engineers 2 and 3
 * generate code from.
 *
 * Determinism contract (L3): every number computed below is pure integer
 * math over the declaration order; the same schema file yields the same
 * layouts on every host, every run, 32- or 64-bit.
 *
 * Robustness contract (L2): no crashes on any input. All recursion is
 * bounded (type expressions: 64 by the parser; struct containment:
 * WEFTC_MAX_STRUCT_NEST); all sizes are checked against 2^48 before use;
 * name lookups are binary searches over a sorted index, not O(n^2) scans.
 *
 * Layout rules (pinned by tests and RFC-0017 §4.2):
 *   - natural alignment: field placed at align_up(cursor, field_align)
 *   - @packed (struct):  every field placed at align 1, struct align 1
 *   - @packed (field):   that field placed at align 1
 *   - @align(N) (struct): struct alignment FLOOR (raise only)
 *   - @align(N) (field):  field alignment, must be >= natural (raise only)
 *   - @simd(N) (struct):  struct floor N, plus every field whose SIZE is
 *                         >= N is raised to alignment N (the SIMD lanes)
 *   - @simd(N) (field):   raise to N if the field's size >= N
 *   - @optimize(packing): fields relaid out in alignment-descending order,
 *                         ties broken by declaration index (stable, so the
 *                         result is a pure function of the source)
 *   - span<T> never creates a containment edge (it is a reference, not a
 *     value) — that is what makes linked topologies non-recursive.
 */
#include "weftc.h"

#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Canonical type strings (RFC-0017 §3.3)                              */
/* ------------------------------------------------------------------ */
static void bb_raw(ByteBuf *b, const char *s)
{
    bb_bytes(b, s, strlen(s));
}

static void ty_str_rec(Arena *ar, const Ty *t, ByteBuf *b, int depth)
{
    if (depth > WEFTC_MAX_TYPE_DEPTH) return; /* parser already errored */
    char num[32];
    switch (t->kind) {
    case TY_PRIM:
        bb_raw(b, prim_info(t->prim)->name);
        return;
    case TY_NAMED:
        bb_raw(b, t->name);
        return;
    case TY_ARRAY:
        bb_u8(b, '[');
        ty_str_rec(ar, t->elem, b, depth + 1);
        bb_bytes(b, "; ", 2);
        snprintf(num, sizeof num, "%llu", (unsigned long long)t->n);
        bb_raw(b, num);
        bb_u8(b, ']');
        return;
    case TY_STR:
        bb_bytes(b, "str[", 4);
        snprintf(num, sizeof num, "%llu", (unsigned long long)t->n);
        bb_raw(b, num);
        bb_u8(b, ']');
        return;
    case TY_SPAN:
        bb_bytes(b, "span<", 5);
        ty_str_rec(ar, t->elem, b, depth + 1);
        bb_u8(b, '>');
        return;
    }
}

const char *ty_str(Arena *ar, const Ty *t)
{
    ByteBuf b;
    bb_init(&b, ar);
    ty_str_rec(ar, t, &b, 0);
    char *s = arena_alloc(ar, b.len + 1, 1);
    if (b.len) memcpy(s, b.data, b.len);
    s[b.len] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* Layout context                                                      */
/* ------------------------------------------------------------------ */
typedef struct LayoutCtx {
    WeftUnit  *u;
    Arena      *ar;
    DiagSink   *sink;
    Decl       *decls;
    size_t      ndecls;
    /* sorted name index for O(log n) lookups */
    size_t     *by_name;   /* decl indices sorted by (name, index) */
    uint8_t    *state;     /* 0 = clean, 1 = visiting, 2 = done, 3 = poisoned */
    size_t     *path;      /* containment chain of decl indices */
    size_t      path_n;
} LayoutCtx;

static uint64_t align_up(uint64_t v, uint64_t a)
{
    if (a <= 1) return v;
    return (v + a - 1) & ~(a - 1);
}

/* ------------------------------------------------------------------ */
/* Diagnostics helpers                                                 */
/* ------------------------------------------------------------------ */
static void check_ident_len(LayoutCtx *c, const char *name, Span sp,
                            const char *what);

/* Levenshtein distance with a hard bail (only used for suggestions; both
 * names capped at 64 bytes so the scratch matrix stays stack-small). */
static size_t edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la > 64 || lb > 64) return SIZE_MAX;
    if (la == 0) return lb;
    if (lb == 0) return la;
    size_t prev[65], cur[65];
    for (size_t j = 0; j <= lb; j++) prev[j] = j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = i;
        for (size_t j = 1; j <= lb; j++) {
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

/* "did you mean `X`?" over declared names then primitive names,
 * declaration order first — deterministic. */
static void suggest_type(LayoutCtx *c, const char *name)
{
    size_t best = 3; /* only suggest distance <= 2 */
    const char *best_name = NULL;
    for (size_t i = 0; i < c->ndecls; i++) {
        const char *cand = c->decls[i].name;
        if (!cand) continue;
        size_t d = edit_distance(name, cand);
        if (d < best) { best = d; best_name = cand; }
    }
    for (int k = 1; k < PRIM__COUNT; k++) {
        size_t d = edit_distance(name, prim_info((PrimKind)k)->name);
        if (d < best) { best = d; best_name = prim_info((PrimKind)k)->name; }
    }
    if (best_name)
        diag_help(c->sink, "did you mean `%s`?", best_name);
}

/* ------------------------------------------------------------------ */
/* Name index (sorted; duplicates -> WE005)                            */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; size_t idx; } NameEntry;

static int cmp_name_entry(const void *pa, const void *pb)
{
    const NameEntry *a = pa, *b = pb;
    int r = strcmp(a->name, b->name);
    if (r) return r;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static void check_ident_len(LayoutCtx *c, const char *name, Span sp,
                            const char *what)
{
    if (name && strlen(name) > 255)
        diag_error(c->sink, "WE036", sp,
                   "%s name `%s` exceeds the 255-byte identifier limit",
                   what, name);
}

static void build_name_index(LayoutCtx *c)
{
    Arena *ar = c->ar;
    NameEntry *tmp = NULL;
    if (c->ndecls) tmp = arena_alloc(ar, c->ndecls * sizeof(NameEntry), 16);
    for (size_t i = 0; i < c->ndecls; i++) {
        if (c->decls[i].name)
            check_ident_len(c, c->decls[i].name, c->decls[i].span,
                            "declaration");
        tmp[i].name = c->decls[i].name ? c->decls[i].name : "";
        tmp[i].idx = i;
    }
    qsort(tmp, c->ndecls, sizeof(NameEntry), cmp_name_entry);
    for (size_t i = 1; i < c->ndecls; i++) {
        if (strcmp(tmp[i - 1].name, tmp[i].name) == 0) {
            Decl *first = &c->decls[tmp[i - 1].idx];
            Decl *dup   = &c->decls[tmp[i].idx];
            if (first->name && strcmp(first->name, "") != 0) {
                diag_error(c->sink, "WE005", dup->span,
                           "duplicate declaration name `%s`", dup->name);
                diag_sec(c->sink, first->span, "previous declaration here");
                dup->poisoned = true;
            }
        }
    }
    c->by_name = arena_alloc(ar, c->ndecls * sizeof(size_t), 16);
    for (size_t i = 0; i < c->ndecls; i++) c->by_name[i] = tmp[i].idx;
}

/* Returns decl index or -1. */
static long find_decl(LayoutCtx *c, const char *name)
{
    size_t lo = 0, hi = c->ndecls;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = strcmp(c->decls[c->by_name[mid]].name, name);
        if (r == 0) return (long)c->by_name[mid];
        if (r < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Attribute plumbing                                                  */
/* ------------------------------------------------------------------ */
static const Attr *attr_solo(LayoutCtx *c, const Attr *attrs, size_t n,
                             AttrKind kind, Span where, const char *what)
{
    const Attr *found = NULL;
    int seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].kind == kind) { found = &attrs[i]; seen++; }
    }
    if (seen > 1)
        diag_error(c->sink, "WE040", where,
                   "attribute `@%s` appears %d times on this %s — attributes "
                   "may appear at most once", found->name, seen, what);
    return found;
}

/* Validates an @align/@simd numeric argument: power of two, 1..4096.
 * Returns the value or 0 on error. */
static uint64_t attr_num_valid(LayoutCtx *c, const Attr *a, Span where)
{
    if (a->num == 0 || (a->num & (a->num - 1)) != 0) {
        diag_error(c->sink, "WE025", where,
                   "attribute `@%s` requires a power-of-two value, got %llu",
                   a->name, (unsigned long long)a->num);
        return 0;
    }
    if (a->num > WEFTC_MAX_ALIGN) {
        diag_error(c->sink, "WE026", where,
                   "attribute `@%s(%llu)` exceeds the %llu-byte alignment "
                   "limit", a->name, (unsigned long long)a->num,
                   (unsigned long long)WEFTC_MAX_ALIGN);
        return 0;
    }
    return a->num;
}

/* ------------------------------------------------------------------ */
/* Type resolution (WE007) + size computation                          */
/* ------------------------------------------------------------------ */
static void resolve_ty(LayoutCtx *c, Ty *t)
{
    if (!t) return;
    switch (t->kind) {
    case TY_PRIM:
    case TY_STR:
        return;
    case TY_NAMED: {
        long i = find_decl(c, t->name);
        if (i < 0) {
            diag_error(c->sink, "WE007", t->span,
                       "unknown type `%s`", t->name);
            suggest_type(c, t->name);
            return;
        }
        t->decl_idx = (int32_t)i;
        return;
    }
    case TY_ARRAY:
    case TY_SPAN:
        resolve_ty(c, t->elem);
        return;
    }
}

typedef struct { uint64_t size, align; } TyGeom;

/* Forward: layout_decl computes a DeclLayout for decl index i. */
static int layout_decl(LayoutCtx *c, size_t i, int depth);

/* Size/align of a RESOLVED type. Returns ok=0 if a dependency is poisoned
 * (recursive or erroring); callers poison upward. */
static int ty_geom(LayoutCtx *c, const Ty *t, int depth, TyGeom *out)
{
    switch (t->kind) {
    case TY_PRIM: {
        const PrimInfo *pi = prim_info(t->prim);
        out->size = pi->size;
        out->align = pi->align;
        return 1;
    }
    case TY_STR:
        out->size = t->n;
        out->align = 1;
        return 1;
    case TY_SPAN:
        out->size = WEFTC_SPAN_SIZE;
        out->align = WEFTC_SPAN_ALIGN;
        return 1; /* no containment edge: a span is a reference */
    case TY_NAMED: {
        if (t->decl_idx < 0) return 0; /* unresolved: already reported */
        if (!layout_decl(c, (size_t)t->decl_idx, depth + 1)) return 0;
        const DeclLayout *dl = &c->u->layouts[t->decl_idx];
        out->size = dl->size;
        out->align = dl->align;
        return 1;
    }
    case TY_ARRAY: {
        TyGeom el;
        if (!ty_geom(c, t->elem, depth, &el)) return 0;
        /* checked multiply: N * stride against the 2^48 ceiling */
        if (el.size && t->n > WEFTC_MAX_TOTAL_BYTES / el.size) {
            diag_error(c->sink, "WE033", t->span,
                       "array `[...; %llu]` exceeds the 2^48-byte total size "
                       "limit", (unsigned long long)t->n);
            return 0;
        }
        out->size = el.size * t->n;
        out->align = el.align;
        return 1;
    }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Struct layout                                                       */
/* ------------------------------------------------------------------ */
typedef struct FieldPlan {
    const Field *f;
    uint64_t    offset, size, align;
    uint32_t    orig_index;
    int         bad; /* type geometry unavailable */
} FieldPlan;

static int cmp_fieldplan(const void *pa, const void *pb)
{
    const FieldPlan *a = pa, *b = pb;
    if (a->align != b->align) return a->align > b->align ? -1 : 1; /* desc */
    return a->orig_index < b->orig_index ? -1
         : (a->orig_index > b->orig_index ? 1 : 0);                 /* asc */
}

/* Computes the total size fields would occupy in `order`. Shared by the
 * real layout and the @optimize(packing) hint. Returns UINT64_MAX on
 * overflow. */
static uint64_t plan_size(const FieldPlan *fps, size_t n)
{
    uint64_t cursor = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t off = align_up(cursor, fps[i].align);
        uint64_t next = off + fps[i].size;
        if (next < off || next > WEFTC_MAX_TOTAL_BYTES) return UINT64_MAX;
        cursor = next;
    }
    return cursor;
}

static void layout_struct(LayoutCtx *c, size_t di, int depth)
{
    WeftUnit *u = c->u;
    Decl *d = &c->decls[di];
    DeclLayout *dl = &u->layouts[di];
    dl->name = d->name;
    dl->kind = DECL_STRUCT;
    dl->decl_index = di;

    /* --- duplicate field names (sorted, WE004) --- */
    if (d->nfields > 1) {
        NameEntry *fe = arena_alloc(c->ar, d->nfields * sizeof(NameEntry), 16);
        for (size_t i = 0; i < d->nfields; i++) {
            fe[i].name = d->fields[i].name ? d->fields[i].name : "";
            fe[i].idx = i;
        }
        qsort(fe, d->nfields, sizeof(NameEntry), cmp_name_entry);
        for (size_t i = 1; i < d->nfields; i++) {
            if (strcmp(fe[i - 1].name, fe[i].name) == 0 &&
                strcmp(fe[i].name, "") != 0) {
                diag_error(c->sink, "WE004", d->fields[fe[i].idx].span,
                           "duplicate field name `%s` in struct `%s`",
                           d->fields[fe[i].idx].name, d->name);
                diag_sec(c->sink, d->fields[fe[i - 1].idx].span,
                         "previous field here");
                d->fields[fe[i].idx].poisoned = true;
            }
        }
    }
    for (size_t i = 0; i < d->nfields; i++)
        check_ident_len(c, d->fields[i].name, d->fields[i].span, "field");

    /* --- declaration attributes --- */
    const Attr *a_pack  = attr_solo(c, d->attrs, d->nattrs, ATTR_PACKED,
                                    d->span, "struct");
    const Attr *a_align = attr_solo(c, d->attrs, d->nattrs, ATTR_ALIGN,
                                    d->span, "struct");
    const Attr *a_simd  = attr_solo(c, d->attrs, d->nattrs, ATTR_SIMD,
                                    d->span, "struct");
    const Attr *a_opt   = attr_solo(c, d->attrs, d->nattrs, ATTR_OPT_PACK,
                                    d->span, "struct");
    uint64_t align_floor = 1, simd_floor = 0;
    if (a_align) {
        uint64_t v = attr_num_valid(c, a_align, d->span);
        if (v) align_floor = v;
        dl->has_align_attr = true;
        dl->align_attr = a_align->num;
    }
    if (a_simd) {
        uint64_t v = attr_num_valid(c, a_simd, d->span);
        if (v) simd_floor = v;
        dl->has_simd_attr = true;
        dl->simd_attr = a_simd->num;
    }
    if (a_pack) dl->packed = true;
    if (a_opt)  dl->reordered = true;
    if (a_pack && a_opt)
        diag_warn(c->sink, "WW001", d->span,
                  "@packed struct with @optimize(packing) — every field is "
                  "already at alignment 1, the reorder is the identity");

    /* --- per-field geometry + attribute effects --- */
    FieldPlan *fps = NULL;
    if (d->nfields)
        fps = arena_alloc(c->ar, d->nfields * sizeof(FieldPlan), 16);
    size_t nfp = 0;
    int bad = 0;
    for (size_t i = 0; i < d->nfields; i++) {
        Field *f = &d->fields[i];
        if (f->poisoned || !f->ty) continue;
        TyGeom g;
        if (!ty_geom(c, f->ty, depth, &g)) { bad = 1; continue; }

        const Attr *f_pack  = attr_solo(c, f->attrs, f->nattrs, ATTR_PACKED,
                                        f->span, "field");
        const Attr *f_align = attr_solo(c, f->attrs, f->nattrs, ATTR_ALIGN,
                                        f->span, "field");
        const Attr *f_simd  = attr_solo(c, f->attrs, f->nattrs, ATTR_SIMD,
                                        f->span, "field");
        const Attr *f_opt   = attr_solo(c, f->attrs, f->nattrs, ATTR_OPT_PACK,
                                        f->span, "field");
        if (f_opt)
            diag_error(c->sink, "WE006", f_opt->span,
                       "attribute `@optimize(packing)` is only valid on a "
                       "struct declaration, not on a field");

        uint64_t fa = g.align;
        if (f_align || f_simd) {
            if (a_pack) {
                diag_error(c->sink, "WE009", f->span,
                           "field `%s` requests an explicit alignment but the "
                           "struct is @packed — remove one of the two",
                           f->name);
                bad = 1;
                continue;
            }
            if (f_pack && f_align) {
                diag_error(c->sink, "WE009", f->span,
                           "field `%s` is both @packed and @align — "
                           "contradictory alignment requests", f->name);
                bad = 1;
                continue;
            }
        }
        if (f_align) {
            uint64_t v = attr_num_valid(c, f_align, f->span);
            if (!v) { bad = 1; continue; }
            if (v < g.align) {
                diag_error(c->sink, "WE030", f_align->span,
                           "field `%s`: @align(%llu) is below the natural "
                           "alignment %llu of its type — alignment can only "
                           "be raised", f->name, (unsigned long long)v,
                           (unsigned long long)g.align);
                bad = 1;
                continue;
            }
            fa = v;
        }
        if (f_pack) fa = 1;
        if (a_pack) fa = 1;
        if (f_simd) {
            uint64_t v = attr_num_valid(c, f_simd, f->span);
            if (!v) { bad = 1; continue; }
            if (g.size >= v && v > fa) fa = v;
        }
        if (simd_floor && g.size >= simd_floor && simd_floor > fa)
            fa = simd_floor;

        FieldPlan fp;
        memset(&fp, 0, sizeof fp);
        fp.f = f;
        fp.size = g.size;
        fp.align = fa;
        fp.orig_index = (uint32_t)i;
        fps[nfp++] = fp;
    }
    if (bad) { /* a field's geometry is unavailable: errors already on the
                  sink; mark the decl so containers stop descending */
        d->poisoned = true;
        return;
    }

    /* --- field order --- */
    if (a_opt)
        qsort(fps, nfp, sizeof(FieldPlan), cmp_fieldplan);

    /* --- place --- */
    FieldLayout *fl = NULL;
    if (nfp) fl = arena_alloc(c->ar, nfp * sizeof(FieldLayout), 16);
    uint64_t cursor = 0;
    Hole *holes = NULL;
    size_t nholes = 0;
    if (nfp) holes = arena_alloc(c->ar, nfp * sizeof(Hole), 16);
    for (size_t i = 0; i < nfp; i++) {
        uint64_t off = align_up(cursor, fps[i].align);
        if (off > cursor) {
            holes[nholes].offset = cursor;
            holes[nholes].size = off - cursor;
            nholes++;
        }
        fl[i].name = fps[i].f->name;
        fl[i].span = fps[i].f->span;
        fl[i].ty = fps[i].f->ty;
        fl[i].type_str = ty_str(c->ar, fps[i].f->ty);
        fl[i].offset = off;
        fl[i].size = fps[i].size;
        fl[i].align = fps[i].align;
        fl[i].orig_index = fps[i].orig_index;
        uint64_t next = off + fps[i].size;
        if (next < off || next > WEFTC_MAX_TOTAL_BYTES) {
            diag_error(c->sink, "WE033", fps[i].f->span,
                       "struct `%s` exceeds the 2^48-byte total size limit",
                       d->name);
            d->poisoned = true;
            return;
        }
        cursor = next;
    }

    /* --- struct alignment + trailing pad --- */
    uint64_t sa = 1;
    for (size_t i = 0; i < nfp; i++)
        if (fps[i].align > sa) sa = fps[i].align;
    if (sa < align_floor) sa = align_floor;
    if (simd_floor && sa < simd_floor) sa = simd_floor;
    uint64_t size = align_up(cursor, sa);
    if (size > WEFTC_MAX_TOTAL_BYTES) {
        diag_error(c->sink, "WE033", d->span,
                   "struct `%s` exceeds the 2^48-byte total size limit",
                   d->name);
        d->poisoned = true;
        return;
    }

    /* --- @optimize(packing) hint (only when NOT already optimized) --- */
    if (!a_opt && nfp > 1) {
        FieldPlan *sorted = arena_alloc(c->ar, nfp * sizeof(FieldPlan), 16);
        memcpy(sorted, fps, nfp * sizeof(FieldPlan));
        qsort(sorted, nfp, sizeof(FieldPlan), cmp_fieldplan);
        uint64_t cur2 = plan_size(sorted, nfp);
        uint64_t size2 = cur2 == UINT64_MAX ? UINT64_MAX
                                            : align_up(cur2, sa);
        if (size2 != UINT64_MAX && size2 < size) dl->hint_opt_size = size2;
    }

    dl->fields = fl;
    dl->nfields = nfp;
    dl->holes = holes;
    dl->nholes = nholes;
    dl->size = size;
    dl->align = sa;
    uint64_t internal = 0;
    for (size_t i = 0; i < nholes; i++) internal += holes[i].size;
    dl->internal_pad = internal;
    dl->trailing_pad = size - cursor;
}

/* ------------------------------------------------------------------ */
/* Enum / bitflags layout                                              */
/* ------------------------------------------------------------------ */
static uint64_t umax_of(uint8_t bytes)
{
    return bytes >= 8 ? UINT64_MAX : ((uint64_t)1 << (8 * bytes)) - 1;
}
static int64_t imax_of(uint8_t bytes)
{
    return bytes >= 8 ? INT64_MAX
                      : (int64_t)(((uint64_t)1 << (8 * bytes - 1)) - 1);
}
static int64_t imin_of(uint8_t bytes)
{
    return bytes >= 8 ? INT64_MIN
                      : -(int64_t)((uint64_t)1 << (8 * bytes - 1));
}

static int cmp_variant_name(const void *pa, const void *pb)
{
    const NameEntry *a = pa, *b = pb;
    int r = strcmp(a->name, b->name);
    if (r) return r;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

typedef struct { int64_t value; size_t idx; } ValueEntry;
static int cmp_variant_value(const void *pa, const void *pb)
{
    const ValueEntry *a = pa, *b = pb;
    if (a->value != b->value)
        return a->value < b->value ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static void layout_enum(LayoutCtx *c, size_t di)
{
    WeftUnit *u = c->u;
    Decl *d = &c->decls[di];
    DeclLayout *dl = &u->layouts[di];
    dl->name = d->name;
    dl->kind = d->kind;
    dl->backing = d->backing;
    dl->decl_index = di;
    dl->variants = d->variants;
    dl->nvariants = d->nvariants;

    /* attributes: only @align makes sense on a fixed-width discriminant */
    for (size_t i = 0; i < d->nattrs; i++) {
        if (d->attrs[i].kind == ATTR_PACKED ||
            d->attrs[i].kind == ATTR_SIMD ||
            d->attrs[i].kind == ATTR_OPT_PACK) {
            diag_error(c->sink, "WE006", d->attrs[i].span,
                       "attribute `@%s` is not valid on an %s declaration — "
                       "only @align is", d->attrs[i].name,
                       d->kind == DECL_ENUM ? "enum" : "bitflags");
        }
    }
    const Attr *a_align = attr_solo(c, d->attrs, d->nattrs, ATTR_ALIGN,
                                    d->span, "enum");
    uint64_t floor = 1;
    if (a_align) {
        uint64_t v = attr_num_valid(c, a_align, d->span);
        if (v) floor = v;
        dl->has_align_attr = true;
        dl->align_attr = a_align->num;
    }

    if (d->nvariants == 0) {
        diag_error(c->sink, "WE014", d->span,
                   "%s `%s` declares no variants",
                   d->kind == DECL_ENUM ? "enum" : "bitflags",
                   d->name ? d->name : "?");
        d->poisoned = true;
        return;
    }
    for (size_t i = 0; i < d->nvariants; i++)
        check_ident_len(c, d->variants[i].name, d->variants[i].span,
                        "variant");

    /* duplicate names -> WE010 */
    if (d->nvariants > 1) {
        NameEntry *ve = arena_alloc(c->ar, d->nvariants * sizeof(NameEntry), 16);
        for (size_t i = 0; i < d->nvariants; i++) {
            ve[i].name = d->variants[i].name ? d->variants[i].name : "";
            ve[i].idx = i;
        }
        qsort(ve, d->nvariants, sizeof(NameEntry), cmp_variant_name);
        for (size_t i = 1; i < d->nvariants; i++) {
            if (strcmp(ve[i - 1].name, ve[i].name) == 0 &&
                strcmp(ve[i].name, "") != 0) {
                diag_error(c->sink, "WE010", d->variants[ve[i].idx].span,
                           "duplicate variant name `%s` in `%s`",
                           d->variants[ve[i].idx].name, d->name);
                diag_sec(c->sink, d->variants[ve[i - 1].idx].span,
                         "previous variant here");
            }
        }
    }

    const PrimInfo *pi = prim_info(d->backing);
    /* value fit + duplicates */
    if (d->nvariants > 1) {
        ValueEntry *vv = arena_alloc(c->ar, d->nvariants * sizeof(ValueEntry), 16);
        for (size_t i = 0; i < d->nvariants; i++) {
            vv[i].value = d->variants[i].value;
            vv[i].idx = i;
        }
        qsort(vv, d->nvariants, sizeof(ValueEntry), cmp_variant_value);
        for (size_t i = 1; i < d->nvariants; i++) {
            if (vv[i].value == vv[i - 1].value) {
                if (d->kind == DECL_ENUM) {
                    diag_error(c->sink, "WE011", d->variants[vv[i].idx].span,
                               "duplicate value %lld in enum `%s` — enum "
                               "discriminants must be unique",
                               (long long)vv[i].value, d->name);
                } else {
                    diag_warn(c->sink, "WW002", d->variants[vv[i].idx].span,
                              "bitflags `%s`: value %lld aliases `%s` — "
                              "aliasing is allowed but intentional",
                              d->name, (long long)vv[i].value,
                              d->variants[vv[i - 1].idx].name);
                }
            }
        }
    }
    for (size_t i = 0; i < d->nvariants; i++) {
        const Variant *v = &d->variants[i];
        if (pi->is_signed) {
            if (v->value < imin_of(pi->size) || v->value > imax_of(pi->size)) {
                diag_error(c->sink, "WE013", v->span,
                           "value %lld does not fit the backing type `%s` "
                           "of `%s`", (long long)v->value, pi->name, d->name);
            }
        } else {
            uint64_t raw = (uint64_t)v->value;
            if (raw > umax_of(pi->size)) {
                diag_error(c->sink, "WE013", v->span,
                           "value %llu does not fit the backing type `%s` "
                           "of `%s`", (unsigned long long)raw, pi->name,
                           d->name);
            }
        }
    }

    dl->size = pi->size;
    dl->align = pi->align < floor ? floor : pi->align;
}

/* ------------------------------------------------------------------ */
/* Decl layout driver: cycle detection + memoization                   */
/* ------------------------------------------------------------------ */
static int layout_decl(LayoutCtx *c, size_t i, int depth)
{
    if (c->state[i] == 2) return !c->decls[i].poisoned;
    if (c->state[i] == 3) return 0;
    if (c->state[i] == 1) {
        /* containment cycle: report the chain c->path[j..] + i */
        size_t j = c->path_n;
        while (j > 0 && c->path[j - 1] != i) j--;
        Decl *off = &c->decls[i];
        diag_error(c->sink, "WE008", off->span,
                   "recursive value type: `%s` is part of a containment "
                   "cycle (span<T> is a reference — use `span<%s>` to "
                   "link instead of embedding)", off->name, off->name);
        size_t shown = 0;
        for (size_t k = j == c->path_n ? 0 : j; k < c->path_n && shown < 8; k++) {
            diag_sec(c->sink, c->decls[c->path[k]].span,
                     "cycle member");
            shown++;
        }
        c->state[i] = 3;
        return 0;
    }
    if (depth > WEFTC_MAX_STRUCT_NEST) {
        diag_error(c->sink, "WE038", c->decls[i].span,
                   "struct nesting exceeds %d levels — value types must "
                   "stay finite; link with span<T> instead",
                   WEFTC_MAX_STRUCT_NEST);
        c->state[i] = 3;
        return 0;
    }
    if (c->decls[i].poisoned) { c->state[i] = 3; return 0; }

    c->state[i] = 1;
    c->path[c->path_n++] = i;
    if (c->decls[i].kind == DECL_STRUCT)
        layout_struct(c, i, depth);
    else
        layout_enum(c, i);
    c->path_n--;
    int ok = !c->decls[i].poisoned;
    c->state[i] = 2;
    /* a decl that errored during layout is done-but-invalid; the poisoned
     * flag makes containers stop descending (errors already reported) */
    return ok;
}

/* ------------------------------------------------------------------ */
/* Top-level driver                                                    */
/* ------------------------------------------------------------------ */
void weft_run_layout(WeftUnit *u)
{
    Arena *ar = u->ar;
    LayoutCtx c;
    memset(&c, 0, sizeof c);
    c.u = u;
    c.ar = ar;
    c.sink = &u->diags;
    c.decls = u->decls;
    c.ndecls = u->ndecls;

    u->layouts = arena_alloc(ar, (u->ndecls ? u->ndecls : 1) *
                                 sizeof(DeclLayout), 16);
    memset(u->layouts, 0, (u->ndecls ? u->ndecls : 1) * sizeof(DeclLayout));

    if (u->ndecls > 0xFFFFFFF0u) {
        diag_error(&u->diags, "WE037", (Span){ 0, 0 },
                   "schema declares more than 2^32-16 types");
        return;
    }

    build_name_index(&c);

    /* resolve every named type reference (WE007) */
    for (size_t i = 0; i < u->ndecls; i++) {
        Decl *d = &u->decls[i];
        if (d->poisoned || d->kind != DECL_STRUCT) continue;
        for (size_t f = 0; f < d->nfields; f++) {
            if (!d->fields[f].poisoned)
                resolve_ty(&c, d->fields[f].ty);
        }
    }

    c.state = arena_alloc(ar, (u->ndecls ? u->ndecls : 1), 16);
    memset(c.state, 0, u->ndecls ? u->ndecls : 1);
    c.path = arena_alloc(ar, (WEFTC_MAX_STRUCT_NEST + 2) * sizeof(size_t), 16);
    c.path_n = 0;

    for (size_t i = 0; i < u->ndecls; i++)
        layout_decl(&c, i, 0);

    u->has_layout = (u->diags.n_errors == 0);
    if (u->has_layout)
        weft_hash_all(u);
}

/* ------------------------------------------------------------------ */
/* Front doors                                                         */
/* ------------------------------------------------------------------ */
WeftUnit *weft_compile_source(const char *path, const char *src, size_t len,
                              unsigned opts)
{
    Arena *ar = arena_create();
    WeftUnit *u = malloc(sizeof *u);
    if (!u) { arena_destroy(ar); fprintf(stderr, "weftc: fatal: out of memory\n"); exit(2); }
    memset(u, 0, sizeof *u);
    u->ar = ar;
    /* the unit owns its own copy of the source (Law 4: no borrowed state) */
    char *copy = arena_alloc(ar, len + 1, 1);
    if (len) memcpy(copy, src, len);
    copy[len] = '\0';
    source_from_memory(ar, path, copy, len, &u->file);
    diag_init(&u->diags, ar, &u->file,
              (opts & WEFT_OPT_COLOR) ? 1 : 0,
              (opts & WEFT_OPT_WERROR) ? 1 : 0);
    parse_schema(&u->file, ar, &u->diags, &u->decls, &u->ndecls, &u->endian_big);
    weft_run_layout(u);
    return u;
}

WeftUnit *weft_compile_file(const char *path, unsigned opts)
{
    Arena *ar = arena_create();
    WeftUnit *u = malloc(sizeof *u);
    if (!u) { arena_destroy(ar); fprintf(stderr, "weftc: fatal: out of memory\n"); exit(2); }
    memset(u, 0, sizeof *u);
    u->ar = ar;
    const char *err = NULL;
    SourceFile sf;
    memset(&sf, 0, sizeof sf);
    if (source_load(ar, path, &sf, &err) != 0) {
        /* no source to point at: an empty unit carrying the loud error */
        source_from_memory(ar, path, "", 0, &u->file);
        diag_init(&u->diags, ar, &u->file,
                  (opts & WEFT_OPT_COLOR) ? 1 : 0,
                  (opts & WEFT_OPT_WERROR) ? 1 : 0);
        diag_error(&u->diags, "WE039", (Span){ 0, 0 },
                   "cannot compile `%s`: %s", path, err ? err : "unreadable");
        return u;
    }
    u->file = sf;
    diag_init(&u->diags, ar, &u->file,
              (opts & WEFT_OPT_COLOR) ? 1 : 0,
              (opts & WEFT_OPT_WERROR) ? 1 : 0);
    parse_schema(&u->file, ar, &u->diags, &u->decls, &u->ndecls, &u->endian_big);
    weft_run_layout(u);
    return u;
}

void weft_unit_free(WeftUnit *u)
{
    if (!u) return;
    Arena *ar = u->ar;
    free(u);
    arena_destroy(ar);
}

size_t weft_error_count(const WeftUnit *u)
{
    return u->diags.n_errors + (u->diags.werror ? u->diags.n_warnings : 0);
}

const DeclLayout *weft_find(const WeftUnit *u, const char *name)
{
    if (!u->has_layout) return NULL;
    for (size_t i = 0; i < u->ndecls; i++)
        if (strcmp(u->layouts[i].name, name) == 0) return &u->layouts[i];
    return NULL;
}

const FieldLayout *weft_field(const DeclLayout *dl, const char *name)
{
    if (!dl) return NULL;
    for (size_t i = 0; i < dl->nfields; i++)
        if (strcmp(dl->fields[i].name, name) == 0) return &dl->fields[i];
    return NULL;
}
