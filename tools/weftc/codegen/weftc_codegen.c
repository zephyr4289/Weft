// weftc_codegen.c — shared infrastructure (see weftc_codegen.h §1-§7).
//
// Pure C11 + libc. The strictness here is deliberate: every rejection is a
// bug in an upstream layout that would otherwise become an ABI violation in
// generated code on someone's device.

#include "weftc_codegen.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// §1 arena
// ---------------------------------------------------------------------------

#define WEFT_ARENA_CHUNK (64u * 1024u)

static weft_arena_chunk* arena_new_chunk(size_t need)
{
    size_t cap = need > WEFT_ARENA_CHUNK ? need : WEFT_ARENA_CHUNK;
    weft_arena_chunk* c = (weft_arena_chunk*)malloc(sizeof(weft_arena_chunk) + cap);
    if (!c) {
        fprintf(stderr, "weftc-codegen: fatal: out of memory (arena)\n");
        exit(3);
    }
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    return c;
}

void* weft_arena_alloc(weft_arena* a, size_t n, size_t align)
{
    if (align > 16) { // we never need more than pointer alignment in the IR
        align = 16;
    }
    if (a->head == NULL) {
        a->head = arena_new_chunk(0);
    }
    weft_arena_chunk* c = a->head;
    size_t off = (size_t)((char*)(c + 1) + c->used - (char*)NULL) & (align - 1); // overkill; keep simple below
    (void)off;
    uintptr_t base = (uintptr_t)((char*)(c + 1)) + c->used;
    uintptr_t aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t need = n + (size_t)(aligned - base);
    if (c->used + need > c->cap) {
        weft_arena_chunk* nc = arena_new_chunk(need + 64);
        nc->next = a->head;
        a->head = nc;
        c = nc;
        base = (uintptr_t)((char*)(c + 1)) + c->used;
        aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
        need = n + (size_t)(aligned - base);
    }
    c->used += need;
    a->total += need;
    return (void*)aligned;
}

char* weft_arena_strdup(weft_arena* a, const char* s)
{
    size_t n = strlen(s);
    char* d = (char*)weft_arena_alloc(a, n + 1, 1);
    memcpy(d, s, n + 1);
    return d;
}

char* weft_arena_strndup(weft_arena* a, const char* s, size_t n)
{
    char* d = (char*)weft_arena_alloc(a, n + 1, 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

// ---------------------------------------------------------------------------
// §1 strbuf
// ---------------------------------------------------------------------------

void wsb_init(strbuf* b)
{
    b->cap = 8192;
    b->len = 0;
    b->data = (char*)malloc(b->cap);
    if (!b->data) {
        fprintf(stderr, "weftc-codegen: fatal: out of memory (strbuf)\n");
        exit(3);
    }
    b->data[0] = '\0';
}

void wsb_free(strbuf* b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void wsb_reserve(strbuf* b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) {
            b->cap *= 2;
        }
        char* nd = (char*)realloc(b->data, b->cap);
        if (!nd) {
            fprintf(stderr, "weftc-codegen: fatal: out of memory (strbuf grow)\n");
            exit(3);
        }
        b->data = nd;
    }
}

void wsb_putc(strbuf* b, char c)
{
    wsb_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void wsb_puts(strbuf* b, const char* s)
{
    wsb_write(b, s, strlen(s));
}

void wsb_write(strbuf* b, const char* s, size_t n)
{
    wsb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void wsb_printf(strbuf* b, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    wsb_reserve(b, (size_t)need);
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
}

size_t wsb_len(const strbuf* b) { return b->len; }

// ---------------------------------------------------------------------------
// §2 JSON — strict subset parser
// ---------------------------------------------------------------------------

typedef struct {
    weft_arena* a;
    const char* s;
    size_t n;
    size_t i;
    unsigned line, col;
    char err[256];
    bool failed;
    int depth;
} jparse;

static void jp_err(jparse* p, const char* fmt, ...)
{
    if (p->failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    p->failed = true;
}

static void jp_advance_linecol(jparse* p, char c)
{
    if (c == '\n') { p->line++; p->col = 1; } else { p->col++; }
}

static void jp_ws(jparse* p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            jp_advance_linecol(p, c);
            p->i++;
        } else {
            break;
        }
    }
}

static jv* jp_value(jparse* p);

static jv* jv_new(jparse* p, jv_kind k)
{
    jv* v = (jv*)weft_arena_alloc(p->a, sizeof(jv), 8);
    memset(v, 0, sizeof(*v));
    v->kind = k;
    return v;
}

static void utf8_encode(strbuf* b, uint32_t cp)
{
    if (cp < 0x80) {
        wsb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        wsb_putc(b, (char)(0xC0 | (cp >> 6)));
        wsb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        wsb_putc(b, (char)(0xE0 | (cp >> 12)));
        wsb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        wsb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// parses a JSON string starting at '"' (already consumed by caller check)
static char* jp_string_raw(jparse* p)
{
    // precondition: p->s[p->i] == '"'
    p->i++; // opening quote
    strbuf b;
    wsb_init(&b);
    while (p->i < p->n) {
        char c = p->s[p->i];
        jp_advance_linecol(p, c);
        if (c == '"') {
            p->i++;
            char* out = weft_arena_strndup(p->a, b.data, b.len);
            wsb_free(&b);
            return out;
        }
        if ((unsigned char)c < 0x20) {
            wsb_free(&b);
            jp_err(p, "raw control character in string");
            return NULL;
        }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) { wsb_free(&b); jp_err(p, "truncated escape"); return NULL; }
            char e = p->s[p->i];
            jp_advance_linecol(p, e);
            p->i++;
            switch (e) {
            case '"': wsb_putc(&b, '"'); break;
            case '\\': wsb_putc(&b, '\\'); break;
            case '/': wsb_putc(&b, '/'); break;
            case 'b': wsb_putc(&b, '\b'); break;
            case 'f': wsb_putc(&b, '\f'); break;
            case 'n': wsb_putc(&b, '\n'); break;
            case 'r': wsb_putc(&b, '\r'); break;
            case 't': wsb_putc(&b, '\t'); break;
            case 'u': {
                uint32_t cp = 0;
                for (int k = 0; k < 4; k++) {
                    if (p->i >= p->n) { wsb_free(&b); jp_err(p, "truncated \\u escape"); return NULL; }
                    int hv = hexval(p->s[p->i]);
                    if (hv < 0) { wsb_free(&b); jp_err(p, "bad hex digit in \\u escape"); return NULL; }
                    cp = (cp << 4) | (uint32_t)hv;
                    jp_advance_linecol(p, p->s[p->i]);
                    p->i++;
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    wsb_free(&b);
                    jp_err(p, "surrogate escapes unsupported (IR is ASCII)");
                    return NULL;
                }
                utf8_encode(&b, cp);
                break;
            }
            default:
                wsb_free(&b);
                jp_err(p, "invalid escape \\%c", e);
                return NULL;
            }
        } else {
            wsb_putc(&b, c);
            p->i++;
        }
    }
    wsb_free(&b);
    jp_err(p, "unterminated string");
    return NULL;
}

static bool jp_number(jparse* p, jv* v)
{
    size_t start = p->i;
    bool is_int = true;
    if (p->i < p->n && (p->s[p->i] == '-' || p->s[p->i] == '+')) {
        if (p->s[p->i] == '+') { jp_err(p, "leading '+' not allowed in JSON"); return false; }
        p->i++;
    }
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') { p->i++; }
    if (p->i < p->n && p->s[p->i] == '.') {
        is_int = false;
        p->i++;
        if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') {
            jp_err(p, "digit expected after '.'");
            return false;
        }
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') { p->i++; }
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        is_int = false;
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '-' || p->s[p->i] == '+')) { p->i++; }
        if (p->i >= p->n || p->s[p->i] < '0' || p->s[p->i] > '9') {
            jp_err(p, "digit expected in exponent");
            return false;
        }
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') { p->i++; }
    }
    if (p->i == start) { jp_err(p, "number expected"); return false; }
    char buf[64];
    size_t len = p->i - start;
    if (len >= sizeof(buf)) { jp_err(p, "number too long"); return false; }
    memcpy(buf, p->s + start, len);
    buf[len] = '\0';
    for (size_t k = 0; k < len; k++) { jp_advance_linecol(p, buf[k]); }
    if (is_int) {
        errno = 0;
        char* end = NULL;
        long long ll = strtoll(buf, &end, 10);
        if (errno == 0 && end && *end == '\0') {
            v->kind = JV_INT;
            v->i = (int64_t)ll;
            return true;
        }
        // falls through to double when out of int64 range
    }
    v->kind = JV_NUM;
    v->num = strtod(buf, NULL);
    return true;
}

static bool jp_lit(jparse* p, const char* lit)
{
    size_t l = strlen(lit);
    if (p->i + l <= p->n && memcmp(p->s + p->i, lit, l) == 0) {
        for (size_t k = 0; k < l; k++) { jp_advance_linecol(p, lit[k]); }
        p->i += l;
        return true;
    }
    return false;
}

static jv* jp_value(jparse* p)
{
    if (p->depth > 64) {
        jp_err(p, "nesting too deep (>64)");
        return NULL;
    }
    jp_ws(p);
    if (p->i >= p->n) { jp_err(p, "value expected"); return NULL; }
    char c = p->s[p->i];
    if (c == '{') {
        p->depth++;
        p->i++;
        jp_advance_linecol(p, c);
        jv* o = jv_new(p, JV_OBJ);
        // capacity growth: arena arrays grown by doubling
        uint32_t cap = 4;
        o->keys = (const char**)weft_arena_alloc(p->a, cap * sizeof(char*), 8);
        o->vals = (jv**)weft_arena_alloc(p->a, cap * sizeof(jv*), 8);
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; p->depth--; return o; }
        for (;;) {
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') { jp_err(p, "object key (string) expected"); return NULL; }
            char* key = jp_string_raw(p);
            if (!key) return NULL;
            for (uint32_t k = 0; k < o->nkeys; k++) {
                if (strcmp(o->keys[k], key) == 0) {
                    jp_err(p, "duplicate object key '%s'", key);
                    return NULL;
                }
            }
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { jp_err(p, "':' expected after key"); return NULL; }
            jp_advance_linecol(p, ':');
            p->i++;
            jv* val = jp_value(p);
            if (!val) return NULL;
            if (o->nkeys == cap) {
                uint32_t ncap = cap * 2;
                const char** nk = (const char**)weft_arena_alloc(p->a, ncap * sizeof(char*), 8);
                jv** nv = (jv**)weft_arena_alloc(p->a, ncap * sizeof(jv*), 8);
                memcpy(nk, o->keys, cap * sizeof(char*));
                memcpy(nv, o->vals, cap * sizeof(jv*));
                o->keys = nk; o->vals = nv; cap = ncap;
            }
            o->keys[o->nkeys] = key;
            o->vals[o->nkeys] = val;
            o->nkeys++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { jp_advance_linecol(p, ','); p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { jp_advance_linecol(p, '}'); p->i++; break; }
            jp_err(p, "',' or '}' expected in object");
            return NULL;
        }
        p->depth--;
        return o;
    }
    if (c == '[') {
        p->depth++;
        p->i++;
        jp_advance_linecol(p, c);
        jv* a = jv_new(p, JV_ARR);
        uint32_t cap = 4;
        a->items = (jv**)weft_arena_alloc(p->a, cap * sizeof(jv*), 8);
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; p->depth--; return a; }
        for (;;) {
            jv* v = jp_value(p);
            if (!v) return NULL;
            if (a->nitems == cap) {
                uint32_t ncap = cap * 2;
                jv** ni = (jv**)weft_arena_alloc(p->a, ncap * sizeof(jv*), 8);
                memcpy(ni, a->items, cap * sizeof(jv*));
                a->items = ni; cap = ncap;
            }
            a->items[a->nitems++] = v;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { jp_advance_linecol(p, ','); p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { jp_advance_linecol(p, ']'); p->i++; break; }
            jp_err(p, "',' or ']' expected in array");
            return NULL;
        }
        p->depth--;
        return a;
    }
    if (c == '"') {
        char* s = jp_string_raw(p);
        if (!s) return NULL;
        jv* v = jv_new(p, JV_STR);
        v->str = s;
        return v;
    }
    if (c == 't') {
        if (!jp_lit(p, "true")) { jp_err(p, "invalid literal"); return NULL; }
        jv* v = jv_new(p, JV_BOOL);
        v->boolean = true;
        return v;
    }
    if (c == 'f') {
        if (!jp_lit(p, "false")) { jp_err(p, "invalid literal"); return NULL; }
        jv* v = jv_new(p, JV_BOOL);
        v->boolean = false;
        return v;
    }
    if (c == 'n') {
        if (!jp_lit(p, "null")) { jp_err(p, "invalid literal"); return NULL; }
        return jv_new(p, JV_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        jv* v = jv_new(p, JV_NUM);
        if (!jp_number(p, v)) return NULL;
        return v;
    }
    jp_err(p, "unexpected character '%c'", c);
    return NULL;
}

jv* jv_parse(weft_arena* a, const char* text, size_t len,
             char err[256], unsigned* line, unsigned* col)
{
    jparse p;
    memset(&p, 0, sizeof(p));
    p.a = a;
    p.s = text;
    p.n = len;
    p.line = 1;
    p.col = 1;
    jv* v = jp_value(&p);
    if (p.failed) {
        snprintf(err, 256, "%.230s (line %u, col %u)", p.err, p.line, p.col);
        *line = p.line;
        *col = p.col;
        return NULL;
    }
    jp_ws(&p);
    if (p.i < p.n) {
        snprintf(err, 256, "trailing content after JSON value (line %u, col %u)", p.line, p.col);
        *line = p.line;
        *col = p.col;
        return NULL;
    }
    return v;
}

jv* jv_get(const jv* obj, const char* key)
{
    if (!obj || obj->kind != JV_OBJ) return NULL;
    for (uint32_t k = 0; k < obj->nkeys; k++) {
        if (strcmp(obj->keys[k], key) == 0) return obj->vals[k];
    }
    return NULL;
}

const char* jv_get_str(const jv* obj, const char* key, const char* dflt)
{
    jv* v = jv_get(obj, key);
    return (v && v->kind == JV_STR) ? v->str : dflt;
}

bool jv_get_u32(const jv* obj, const char* key, uint32_t min, uint32_t max, uint32_t* out)
{
    jv* v = jv_get(obj, key);
    if (!v || v->kind != JV_INT) return false;
    if (v->i < (int64_t)min || v->i > (int64_t)max) return false;
    *out = (uint32_t)v->i;
    return true;
}

bool jv_get_bool(const jv* obj, const char* key, bool dflt, bool* out)
{
    jv* v = jv_get(obj, key);
    if (!v) { *out = dflt; return true; }
    if (v->kind != JV_BOOL) return false;
    *out = v->boolean;
    return true;
}

// ---------------------------------------------------------------------------
// §6 naming — reserved words across all four target languages
// ---------------------------------------------------------------------------

static const char* k_reserved[] = {
    // C / C-ish
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "_Bool", "_Alignas", "_Alignof", "_Atomic",
    "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
    "_Thread_local", "bool", "true", "false", "NULL",
    // Rust
    "as", "async", "await", "become", "box", "crate", "dyn", "final", "impl",
    "in", "let", "loop", "match", "mod", "move", "mut", "override", "priv",
    "pub", "ref", "self", "Self", "super", "trait", "type", "typeof",
    "unsized", "use", "where", "yield", "try", "abstract", "macro", "fn",
    // WGSL
    "alias", "array", "atomic", "bitcast", "discard", "enable", "f16",
    "fallthrough", "read", "read_write", "uniform", "workgroup", "var",
    "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2",
    "mat4x3", "mat4x4", "vec2", "vec3", "vec4",
    // GLSL
    "attribute", "binding", "buffer", "centroid", "coherent", "flat",
    "highp", "invariant", "layout", "location", "lowp", "mediump", "noperspective",
    "patch", "precise", "precision", "readonly", "sample", "set", "shared",
    "smooth", "subroutine", "varying", "volatile", "writeonly", "active",
    "filter", "image", "sampler", "texture",
    NULL
};

static bool is_reserved(const char* s)
{
    for (int k = 0; k_reserved[k]; k++) {
        if (strcmp(k_reserved[k], s) == 0) return true;
    }
    return false;
}

bool weft_ident_ok(const char* s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63) return false;
    if (!(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return false;
    for (size_t k = 1; k < n; k++) {
        char c = s[k];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    if (is_reserved(s)) return false;
    return true;
}

bool weft_snake_ok(const char* s)
{
    if (!s[0] || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    size_t n = strlen(s);
    if (n > 63) return false;
    for (size_t k = 0; k < n; k++) {
        char c = s[k];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    if (is_reserved(s)) return false;
    return true;
}

void weft_camel(const char* snake, char out[80])
{
    size_t o = 0;
    bool up = true;
    for (size_t k = 0; snake[k] && o < 78; k++) {
        char c = snake[k];
        if (c == '_') { up = true; continue; }
        out[o++] = up && (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        up = false;
    }
    out[o] = '\0';
}

void weft_upper(const char* snake, char out[160])
{
    size_t o = 0;
    for (size_t k = 0; snake[k] && o < 158; k++) {
        char c = snake[k];
        if (c == '_') {
            out[o++] = '_';
        } else if (c >= 'a' && c <= 'z') {
            out[o++] = (char)(c - 'a' + 'A');
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

bool weft_names_derive(weft_arena* a, weft_struct* s, char err[256])
{
    char buf[160];
    if (s->c_name) {
        if (!weft_ident_ok(s->c_name)) {
            snprintf(err, 256, "struct %s: c_name '%s' is not a valid identifier", s->name, s->c_name);
            return false;
        }
    } else {
        snprintf(buf, sizeof(buf), "weft_%s_t", s->name);
        s->c_name = weft_arena_strdup(a, buf);
    }
    if (s->rust_name) {
        if (!weft_ident_ok(s->rust_name)) {
            snprintf(err, 256, "struct %s: rust_name '%s' is not a valid identifier", s->name, s->rust_name);
            return false;
        }
    } else {
        weft_camel(s->name, buf);
        s->rust_name = weft_arena_strdup(a, buf);
    }
    return true;
}

// ---------------------------------------------------------------------------
// §4 type table — canonical C layout
// ---------------------------------------------------------------------------

static const weft_layout k_scalar_layout[] = {
    [WT_U8]  = {1, 1}, [WT_I8]  = {1, 1},
    [WT_U16] = {2, 2}, [WT_I16] = {2, 2},
    [WT_U32] = {4, 4}, [WT_I32] = {4, 4},
    [WT_U64] = {8, 8}, [WT_I64] = {8, 8},
    [WT_F16] = {2, 2}, [WT_F32] = {4, 4}, [WT_F64] = {8, 8},
    [WT_BOOL] = {1, 1},
    [WT_VEC2F32] = {8, 4}, [WT_VEC3F32] = {12, 4}, [WT_VEC4F32] = {16, 4},
    [WT_VEC2F16] = {4, 2}, [WT_VEC3F16] = {6, 2}, [WT_VEC4F16] = {8, 2},
};

weft_layout weft_type_layout(const weft_field* f)
{
    weft_layout l = {0, 0};
    if (f->kind == WT_ARRAY) {
        weft_field tmp = *f;
        tmp.kind = f->elem_kind;
        weft_layout e = weft_type_layout(&tmp);
        l.size = e.size * f->count;
        l.align = e.align;
        return l;
    }
    if (f->kind == WT_STRUCT) {
        l.size = f->struct_type->size;
        l.align = f->struct_type->align;
        return l;
    }
    l = k_scalar_layout[f->kind];
    return l;
}

void weft_type_canonical(const weft_field* f, char out[128])
{
    switch (f->kind) {
    case WT_U8: strcpy(out, "u8"); break;
    case WT_I8: strcpy(out, "i8"); break;
    case WT_U16: strcpy(out, "u16"); break;
    case WT_I16: strcpy(out, "i16"); break;
    case WT_U32: strcpy(out, "u32"); break;
    case WT_I32: strcpy(out, "i32"); break;
    case WT_U64: strcpy(out, "u64"); break;
    case WT_I64: strcpy(out, "i64"); break;
    case WT_F16: strcpy(out, "f16"); break;
    case WT_F32: strcpy(out, "f32"); break;
    case WT_F64: strcpy(out, "f64"); break;
    case WT_BOOL: strcpy(out, "bool"); break;
    case WT_VEC2F32: strcpy(out, "vec2<f32>"); break;
    case WT_VEC3F32: strcpy(out, "vec3<f32>"); break;
    case WT_VEC4F32: strcpy(out, "vec4<f32>"); break;
    case WT_VEC2F16: strcpy(out, "vec2<f16>"); break;
    case WT_VEC3F16: strcpy(out, "vec3<f16>"); break;
    case WT_VEC4F16: strcpy(out, "vec4<f16>"); break;
    case WT_ARRAY: {
        weft_field tmp = *f;
        tmp.kind = f->elem_kind;
        char elem[128];
        weft_type_canonical(&tmp, elem);
        snprintf(out, 128, "[%.100s; %u]", elem, f->count);
        break;
    }
    case WT_STRUCT:
        snprintf(out, 80, "%s", f->struct_ref);
        break;
    default: strcpy(out, "?"); break;
    }
}

// ---------------------------------------------------------------------------
// §7 schema signature + FNV-1a-64
// ---------------------------------------------------------------------------

uint64_t weft_fnv1a64(const char* s)
{
    uint64_t h = 14695981039346656037ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

void weft_schema_signature(const weft_struct* s, char out[512])
{
    size_t o = (size_t)snprintf(out, 512, "weft-ir/v1|struct=%s|align=%u|size=%u|root=%d",
                                s->name, s->align, s->size, s->root ? 1 : 0);
    for (uint32_t k = 0; k < s->nfields && o < 510; k++) {
        const weft_field* f = &s->fields[k];
        char canon[128];
        weft_type_canonical(f, canon);
        o += (size_t)snprintf(out + o, 512 - o, "|%s:%s@%u", f->name, canon, f->offset);
        if (f->gpu_type && o < 510) {
            o += (size_t)snprintf(out + o, 512 - o, "+gpu=%s", f->gpu_type);
        }
        for (uint32_t b = 0; b < f->nbits && o < 510; b++) {
            o += (size_t)snprintf(out + o, 512 - o, "+bit=%s[%u:%u]", f->bits[b].name, f->bits[b].lo, f->bits[b].hi);
        }
    }
}

// ---------------------------------------------------------------------------
// emitter helpers
// ---------------------------------------------------------------------------

void weft_warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("weftc-codegen: warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

int weft_mkdir_p(const char* path)
{
    char tmp[512];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    for (size_t k = 1; k < n; k++) {
        if (tmp[k] == '/') {
            tmp[k] = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            tmp[k] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

int weft_write_file(const char* path, const strbuf* b)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "weftc-codegen: error: cannot open '%s' for write\n", path);
        return -1;
    }
    if (b->len && fwrite(b->data, 1, b->len, f) != b->len) {
        fprintf(stderr, "weftc-codegen: error: short write to '%s'\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

void weft_hex64(uint64_t v, char out[24])
{
    snprintf(out, 24, "0x%016llX", (unsigned long long)v);
}

void weft_hex32(uint32_t v, char out[16])
{
    snprintf(out, 16, "0x%08X", v);
}
