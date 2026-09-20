/* diag.c — arena, byte buffers, source loading, and the diagnostics
 * engine (GCC/Clang-style messages with spans, carets, notes and help).
 *
 * Rendering contract (pinned by tests/golden):
 *
 *   frames.weft:12:9: error[WE007]: unknown type `Vec3f`
 *      12 |     vel: Vec3f,
 *         |         ^^^^^
 *     help: did you mean `Vec3`? (declared at frames.weft:7:8)
 *
 *   error: aborting due to 3 previous errors
 *
 * The `path:line:col: severity[code]:` prefix is machine-parseable the
 * same way GCC/Clang output is. Colors follow NO_COLOR / --color.
 */
#include "weftc.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Fatal OOM — the one loud exit the compiler allows (Law 4).          */
/* ------------------------------------------------------------------ */
static void fatal_oom(void)
{
    fprintf(stderr, "weftc: fatal: out of memory\n");
    exit(2);
}

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used, cap;
    /* payload follows */
} ArenaBlock;

struct Arena {
    ArenaBlock *blocks;
};

#define ARENA_BLOCK_MIN (64u * 1024u)

Arena *arena_create(void)
{
    Arena *a = malloc(sizeof(Arena));
    if (!a) fatal_oom();
    a->blocks = NULL;
    return a;
}

void arena_destroy(Arena *a)
{
    if (!a) return;
    ArenaBlock *b = a->blocks;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

static ArenaBlock *arena_new_block(size_t need)
{
    size_t cap = need > ARENA_BLOCK_MIN ? need : ARENA_BLOCK_MIN;
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
    if (!b) fatal_oom();
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    if (align < 1) align = 1;
    if (align > 16) align = 16; /* nothing here needs more than 16 */
    if (size == 0) size = 1;
    if (!a->blocks) a->blocks = arena_new_block(size + align);
    ArenaBlock *b = a->blocks;
    uintptr_t base = (uintptr_t)b + sizeof(ArenaBlock) + b->used;
    uintptr_t aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t room = b->cap - (size_t)(aligned - (uintptr_t)b - sizeof(ArenaBlock));
    if (room < size) {
        b = arena_new_block(size + align);
        b->next = a->blocks; /* newest first */
        a->blocks = b;
        base = (uintptr_t)b + sizeof(ArenaBlock);
        aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
    }
    b->used = (size_t)(aligned + size - (uintptr_t)b - sizeof(ArenaBlock));
    return (void *)aligned;
}

char *arena_dupn(Arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1, 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *arena_dup(Arena *a, const char *s)
{
    return arena_dupn(a, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* Byte buffer (grows through the arena; old bytes wasted, bounded 2x) */
/* ------------------------------------------------------------------ */
static void bb_reserve(ByteBuf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *np = arena_alloc(b->ar, nc, 16);
    if (b->len) memcpy(np, b->data, b->len);
    b->data = np;
    b->cap = nc;
}

void bb_init(ByteBuf *b, Arena *ar) { b->data = NULL; b->len = b->cap = 0; b->ar = ar; }

void bb_bytes(ByteBuf *b, const void *p, size_t n)
{
    bb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
void bb_u8(ByteBuf *b, uint8_t v)     { bb_bytes(b, &v, 1); }
void bb_u16le(ByteBuf *b, uint16_t v) { uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; bb_bytes(b, t, 2); }
void bb_u32le(ByteBuf *b, uint32_t v) { uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; bb_bytes(b, t, 4); }
void bb_u64le(ByteBuf *b, uint64_t v) { for (int i = 0; i < 8; i++) bb_u8(b, (uint8_t)(v >> (8 * i))); }
void bb_i64le(ByteBuf *b, int64_t v)  { bb_u64le(b, (uint64_t)v); }
void bb_str(ByteBuf *b, const char *s)
{
    size_t n = strlen(s);
    if (n > 0xFFFF) n = 0xFFFF; /* name lengths are validated <= 255 earlier */
    bb_u16le(b, (uint16_t)n);
    bb_bytes(b, s, n);
}

/* ------------------------------------------------------------------ */
/* Source loading                                                      */
/* ------------------------------------------------------------------ */
static void build_line_starts(Arena *ar, SourceFile *sf)
{
    size_t cap = 64, n = 0;
    size_t *ls = arena_alloc(ar, cap * sizeof(size_t), 16);
    ls[n++] = 0;
    for (size_t i = 0; i < sf->len; i++) {
        if (sf->src[i] == '\n') {
            if (n == cap) {
                size_t nc = cap * 2;
                size_t *np = arena_alloc(ar, nc * sizeof(size_t), 16);
                memcpy(np, ls, n * sizeof(size_t));
                ls = np; cap = nc;
            }
            ls[n++] = i + 1;
        }
    }
    sf->line_starts = ls;
    sf->nlines = n;
}

void source_from_memory(Arena *ar, const char *path, const char *src,
                        size_t len, SourceFile *out)
{
    out->path = arena_dup(ar, path);
    out->src = src;
    out->len = len;
    build_line_starts(ar, out);
}

int source_load(Arena *ar, const char *path, SourceFile *out, const char **err)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "cannot open file"; return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *err = "cannot seek file"; return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); *err = "cannot size file"; return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); *err = "cannot seek file"; return -1; }
    char *buf = arena_alloc(ar, (size_t)sz + 1, 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); *err = "short read"; return -1;
    }
    fclose(f);
    buf[sz] = '\0';
    source_from_memory(ar, path, buf, (size_t)sz, out);
    return 0;
}

void source_line_col(const SourceFile *sf, Span sp, size_t *line, size_t *col,
                     size_t *line_off, size_t *line_len)
{
    size_t off = sp.off < sf->len ? sp.off : sf->len;
    size_t lo = 0, hi = sf->nlines;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sf->line_starts[mid] <= off) lo = mid; else hi = mid;
    }
    size_t lstart = sf->line_starts[lo];
    size_t lend = (lo + 1 < sf->nlines) ? sf->line_starts[lo + 1] : sf->len;
    if (lend > lstart && sf->src[lend - 1] == '\n') lend--;
    if (lend > lstart && sf->src[lend - 1] == '\r') lend--;
    *line = lo + 1;
    *col = off - lstart + 1;
    *line_off = lstart;
    *line_len = lend - lstart;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_CYAN    "\x1b[36m"
#define C_DIM     "\x1b[2m"

void diag_init(DiagSink *sink, Arena *ar, const SourceFile *file,
               int color, int werror)
{
    sink->items = NULL; sink->n = sink->cap = 0;
    sink->ar = ar;
    sink->file = file;
    sink->n_errors = sink->n_warnings = 0;
    sink->werror = werror;
    sink->color = color;
}

static Diag *diag_last(DiagSink *s) { return s->n ? &s->items[s->n - 1] : NULL; }

static void diag_push(DiagSink *s, Severity sev, const char *code, Span sp,
                      const char *fmt, va_list ap)
{
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        Diag *np = arena_alloc(s->ar, nc * sizeof(Diag), 16);
        if (s->n) memcpy(np, s->items, s->n * sizeof(Diag));
        s->items = np; s->cap = nc;
    }
    Diag *d = &s->items[s->n++];
    memset(d, 0, sizeof *d);
    d->sev = sev;
    d->code = code;
    d->primary = sp;
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) need = 0;
    char *buf = arena_alloc(s->ar, (size_t)need + 1, 1);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    d->msg = buf;
    if (sev == SEV_ERROR) s->n_errors++;
    if (sev == SEV_WARNING) s->n_warnings++;
}

void diag_error(DiagSink *s, const char *code, Span sp, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    diag_push(s, SEV_ERROR, code, sp, fmt, ap);
    va_end(ap);
}

void diag_warn(DiagSink *s, const char *code, Span sp, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    diag_push(s, SEV_WARNING, code, sp, fmt, ap);
    va_end(ap);
}

static void diag_push_note(DiagSink *s, int is_help, const char *fmt, va_list ap)
{
    Diag *d = diag_last(s);
    if (!d || d->nnotes == 4) return;
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) need = 0;
    char *buf = arena_alloc(s->ar, (size_t)need + 1, 1);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    d->notes[d->nnotes].text = buf;
    d->notes[d->nnotes].is_help = is_help;
    d->nnotes++;
}

void diag_note(DiagSink *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    diag_push_note(s, 0, fmt, ap);
    va_end(ap);
}

void diag_help(DiagSink *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    diag_push_note(s, 1, fmt, ap);
    va_end(ap);
}

void diag_sec(DiagSink *s, Span sp, const char *label)
{
    Diag *d = diag_last(s);
    if (!d || d->nsecs == 8) return;
    d->secs[d->nsecs].span = sp;
    d->secs[d->nsecs].label = label;
    d->nsecs++;
}

/* --- rendering ----------------------------------------------------- */

static const char *sev_word(Severity s)
{
    switch (s) {
    case SEV_ERROR:   return "error";
    case SEV_WARNING: return "warning";
    case SEV_NOTE:    return "note";
    case SEV_HELP:    return "help";
    }
    return "?";
}

static const char *sev_color(Severity s, int color)
{
    if (!color) return "";
    switch (s) {
    case SEV_ERROR:   return C_BOLD C_RED;
    case SEV_WARNING: return C_BOLD C_YELLOW;
    case SEV_NOTE:    return C_BOLD C_CYAN;
    case SEV_HELP:    return C_BOLD C_GREEN;
    }
    return "";
}

/* Line-number gutter width for one diagnostic: the widest line number
 * among the primary + secondary spans, +1 space. */
static size_t gutter_width(const DiagSink *s, const Diag *d)
{
    size_t maxl = 0;
    size_t line, col, lo, ll;
    source_line_col(s->file, d->primary, &line, &col, &lo, &ll);
    maxl = line;
    for (size_t k = 0; k < d->nsecs; k++) {
        source_line_col(s->file, d->secs[k].span, &line, &col, &lo, &ll);
        if (line > maxl) maxl = line;
    }
    char numbuf[32];
    snprintf(numbuf, sizeof numbuf, "%zu", maxl);
    return strlen(numbuf) + 1;
}

/* " NNN | <source line>" — tabs flattened (goldens are tab-free). */
static void render_line_row(const DiagSink *s, FILE *out, size_t line,
                            size_t line_off, size_t line_len, size_t gw)
{
    if (s->color) fputs(C_DIM, out);
    fprintf(out, "%*zu ", (int)(gw - 1), line);
    if (s->color) fputs(C_RESET C_DIM, out);
    fputc('|', out);
    if (s->color) fputs(C_RESET, out);
    fputc(' ', out);
    for (size_t i = 0; i < line_len; i++)
        fputc(s->file->src[line_off + i] == '\t' ? ' '
              : s->file->src[line_off + i], out);
    if (s->color) fputs(C_RESET, out);
    fputc('\n', out);
}

/* "     |      ^^^^^ optional-label" under a line row. */
static void render_caret_row(const DiagSink *s, FILE *out, Span sp,
                             const char *label, size_t gw)
{
    size_t line, col, line_off, line_len;
    source_line_col(s->file, sp, &line, &col, &line_off, &line_len);
    for (size_t i = 0; i < gw; i++) fputc(' ', out);
    fputs("| ", out);
    size_t start = sp.off >= line_off ? sp.off - line_off : 0;
    if (start > line_len) start = line_len;
    size_t width = sp.len ? sp.len : 1;
    if (start + width > line_len) width = line_len - start;
    for (size_t i = 0; i < start; i++) fputc(' ', out);
    if (s->color) fputs(C_BOLD C_CYAN, out);
    for (size_t i = 0; i < width; i++) fputc('^', out);
    if (s->color) fputs(C_RESET, out);
    if (label && *label)
        fprintf(out, " %s%s%s", s->color ? C_CYAN : "", label,
                s->color ? C_RESET : "");
    fputc('\n', out);
}

void diag_render_all(const DiagSink *s, FILE *out)
{
    for (size_t i = 0; i < s->n; i++) {
        const Diag *d = &s->items[i];
        size_t line, col, line_off, line_len;
        source_line_col(s->file, d->primary, &line, &col, &line_off, &line_len);
        const char *ec = sev_color(d->sev, s->color);
        const char *rs = s->color ? C_RESET : "";
        fprintf(out, "%s:%zu:%zu: %s%s%s%s%s: %s%s%s\n",
                s->file->path, line, col, ec, sev_word(d->sev),
                d->code ? "[" : "", d->code ? d->code : "", d->code ? "]" : "",
                s->color ? C_RESET : "", d->msg, rs);
        size_t gw = gutter_width(s, d);
        render_line_row(s, out, line, line_off, line_len, gw);
        render_caret_row(s, out, d->primary, NULL, gw);
        for (size_t k = 0; k < d->nsecs; k++) {
            size_t l2, c2, lo2, ll2;
            source_line_col(s->file, d->secs[k].span, &l2, &c2, &lo2, &ll2);
            fprintf(out, "%s:%zu:%zu: %s%s%s%s\n", s->file->path, l2, c2,
                    s->color ? C_DIM : "", d->secs[k].label ? d->secs[k].label : "",
                    s->color ? C_RESET : "", s->color ? C_RESET : "");
            render_line_row(s, out, l2, lo2, ll2, gw);
            render_caret_row(s, out, d->secs[k].span, NULL, gw);
        }
        for (size_t k = 0; k < d->nnotes; k++) {
            const char *word = d->notes[k].is_help ? "help" : "note";
            const char *nc = d->notes[k].is_help ? sev_color(SEV_HELP, s->color)
                                                 : sev_color(SEV_NOTE, s->color);
            fprintf(out, "  %s%s%s: %s%s%s\n", nc, word,
                    s->color ? C_RESET : "",
                    s->color ? C_RESET : "", d->notes[k].text,
                    s->color ? C_RESET : "");
        }
    }
    size_t errs = s->n_errors + (s->werror ? s->n_warnings : 0);
    if (errs > 0)
        fprintf(out, "%serror%s: aborting due to %zu previous error%s\n",
                s->color ? C_BOLD C_RED : "", s->color ? C_RESET : "",
                errs, errs == 1 ? "" : "s");
    else if (s->n_warnings > 0)
        fprintf(out, "%swarning%s: %zu warning%s emitted\n",
                s->color ? C_BOLD C_YELLOW : "", s->color ? C_RESET : "",
                s->n_warnings, s->n_warnings == 1 ? "" : "s");
}
