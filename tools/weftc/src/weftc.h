/* weftc.h — Universal Zero-Serialization Schema Compiler (RFC-0017, Pillar 1).
 *
 * Single shared header for the weftc compiler: public front-door API (the
 * seam Engineers 2 and 3 build on) plus internal types shared by the
 * pipeline stages:
 *
 *     source -> lexer -> parser -> AST -> layout engine -> IR (+hashes)
 *                                        -> dumpers (json/bin/inspect/header)
 *
 * Laws honored here (RFC-0017 §3):
 *   L1  no hidden allocations — every generated layout is flat, fixed-bound;
 *   L2  zero compiler crashes — arena allocation, bounded recursion,
 *       panic-mode diagnostics (never abort on bad input);
 *   L3  architectural parity — all multi-byte numbers in manifests and the
 *       binary IR are explicit little-endian; layout math is host-independent;
 *   L4  no bloated dependencies — C11 + libc only.
 *
 * Everything the compiler allocates lives in one arena owned by WeftUnit;
 * weft_unit_free() releases it all. There is no free() anywhere else.
 */
#ifndef WEFTC_H
#define WEFTC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#define WEFTC_VERSION   "0.1.0"
#define WEFTC_IR_VERSION 1u

/* Hard bounds (RFC-0017 §4.4). Fixed sizes must be statically computable
 * and bounded; these limits keep every intermediate computation in u64
 * without overflow and keep generated types portable to 32-bit hosts. */
#define WEFTC_MAX_ALIGN        4096ull  /* @align/@simd upper bound          */
#define WEFTC_MAX_TOTAL_BYTES  (1ull << 48) /* total struct size ceiling     */
#define WEFTC_MAX_FIXED_LEN    0xFFFFFFFFull /* [T;N] / str[N] length (u32)  */
#define WEFTC_MAX_TYPE_DEPTH   64       /* type-expression nesting depth     */
#define WEFTC_MAX_STRUCT_NEST   256      /* struct-contains-struct depth      */
#define WEFTC_SPAN_SIZE        16u      /* span<T> = { u64 offset, u64 len } */
#define WEFTC_SPAN_ALIGN       8u

/* ------------------------------------------------------------------ */
/* Arena — single owning allocator for the whole compilation.          */
/* ------------------------------------------------------------------ */
typedef struct Arena Arena;
Arena *arena_create(void);
void   arena_destroy(Arena *a);
/* Never returns NULL: allocation failure is a fatal, loud exit (Law 4 —
 * out-of-memory is reported, never silently ignored). */
void  *arena_alloc(Arena *a, size_t size, size_t align);
char  *arena_dupn(Arena *a, const char *s, size_t n); /* n bytes + NUL */
char  *arena_dup (Arena *a, const char *s);

/* ------------------------------------------------------------------ */
/* Byte buffer — grows through the arena (old space wasted, bounded 2x) */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *data;
    size_t   len, cap;
    Arena   *ar;
} ByteBuf;
void bb_init(ByteBuf *b, Arena *ar);
void bb_u8   (ByteBuf *b, uint8_t v);
void bb_u16le(ByteBuf *b, uint16_t v);
void bb_u32le(ByteBuf *b, uint32_t v);
void bb_u64le(ByteBuf *b, uint64_t v);
void bb_i64le(ByteBuf *b, int64_t v);
void bb_bytes(ByteBuf *b, const void *p, size_t n);
void bb_str  (ByteBuf *b, const char *s); /* u16 length prefix + bytes, no NUL */

/* ------------------------------------------------------------------ */
/* Source + spans                                                      */
/* ------------------------------------------------------------------ */
typedef struct { size_t off, len; } Span; /* byte range in the source */

typedef struct {
    const char *path;   /* as passed on the command line (reproducible IR) */
    const char *src;
    size_t      len;
    size_t     *line_starts; /* offset of each line start; nlines entries */
    size_t      nlines;
} SourceFile;

/* Loads `path` fully into the arena. Returns 0 on success; on failure
 * returns -1 and sets *err to a static message (missing/unreadable). */
int  source_load(Arena *ar, const char *path, SourceFile *out, const char **err);
void source_from_memory(Arena *ar, const char *path, const char *src,
                        size_t len, SourceFile *out);
void source_line_col(const SourceFile *sf, Span sp, size_t *line, size_t *col,
                     size_t *line_off, size_t *line_len);

/* ------------------------------------------------------------------ */
/* Diagnostics — GCC/Clang-style: "path:line:col: error[CODE]: msg"    */
/* with a caret diagram, secondary spans, notes and help.              */
/* ------------------------------------------------------------------ */
typedef enum { SEV_ERROR, SEV_WARNING, SEV_NOTE, SEV_HELP } Severity;

typedef struct { Span span; const char *label; } SecSpan;
typedef struct { const char *text; int is_help; } DiagNote;

typedef struct {
    Severity    sev;
    const char *code;      /* "WE007" / "WW001", or NULL */
    const char *msg;       /* arena-owned, printf-formatted */
    Span        primary;
    SecSpan     secs[8];
    size_t      nsecs;
    DiagNote    notes[4]; /* arena-owned note/help lines */
    size_t      nnotes;
} Diag;

typedef struct {
    Diag   *items;
    size_t  n, cap;
    Arena  *ar;          /* owns messages + render scratch */
    const SourceFile *file;
    size_t  n_errors, n_warnings;
    int     werror;      /* warnings promoted to errors for exit status */
    int     color;       /* 0 = never */
} DiagSink;

void diag_init(DiagSink *sink, Arena *ar, const SourceFile *file,
               int color, int werror);

/* Primary emitters. notes/help/secs attach to the most recent diagnostic
 * (a lone note with no prior diagnostic is dropped — callers always emit
 * an error or warning first). */
void diag_error(DiagSink *s, const char *code, Span sp, const char *fmt, ...);
void diag_warn (DiagSink *s, const char *code, Span sp, const char *fmt, ...);
void diag_note (DiagSink *s, const char *fmt, ...);
void diag_help (DiagSink *s, const char *fmt, ...);
void diag_sec  (DiagSink *s, Span sp, const char *label);

/* Renders all diagnostics in emission order + trailing summary. */
void diag_render_all(const DiagSink *s, FILE *out);

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    TOK_EOF = 0, TOK_IDENT, TOK_INT, TOK_BADCHAR,
    /* keywords: declarations */
    TOK_KW_STRUCT, TOK_KW_ENUM, TOK_KW_BITFLAGS, TOK_KW_SPAN, TOK_KW_STR,
    TOK_KW_ENDIANNESS, TOK_KW_LITTLE, TOK_KW_BIG, TOK_KW_PACKING,
    /* keywords: primitive types */
    TOK_KW_U8,  TOK_KW_I8,  TOK_KW_U16, TOK_KW_I16,
    TOK_KW_U32, TOK_KW_I32, TOK_KW_U64, TOK_KW_I64,
    TOK_KW_F16, TOK_KW_F32, TOK_KW_F64, TOK_KW_BOOL,
    /* punctuation */
    TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_LPAREN, TOK_RPAREN, TOK_LANG, TOK_RANG,
    TOK_COLON, TOK_SEMI, TOK_COMMA, TOK_EQ, TOK_AT,
    TOK_MINUS
} TokKind;

const char *tok_kind_str(TokKind k); /* "':'", "'struct'", "identifier", ... */

typedef struct {
    TokKind    kind;
    Span       span;
    const char *text;   /* arena, NUL-terminated (idents/bad chars) */
    uint64_t   ival;    /* TOK_INT value (saturating on overflow) */
} Token;

typedef struct {
    const SourceFile *sf;
    Arena            *ar;
    DiagSink         *sink;
    size_t            pos;   /* byte offset of cur */
    Token             cur;
} Lexer;

void lex_init(Lexer *lx, const SourceFile *sf, Arena *ar, DiagSink *sink);
void lex_advance(Lexer *lx); /* cur = next token */

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */
typedef enum {
    PRIM_NONE = 0,
    PRIM_U8, PRIM_I8, PRIM_U16, PRIM_I16, PRIM_U32, PRIM_I32,
    PRIM_U64, PRIM_I64, PRIM_F16, PRIM_F32, PRIM_F64, PRIM_BOOL,
    PRIM__COUNT
} PrimKind;

typedef struct { const char *name; uint8_t size, align, is_int, is_signed; } PrimInfo;
const PrimInfo *prim_info(PrimKind k); /* PRIM_NONE -> NULL */

typedef enum { TY_PRIM, TY_NAMED, TY_ARRAY, TY_STR, TY_SPAN } TyKind;

typedef struct Ty {
    TyKind      kind;
    PrimKind    prim;    /* TY_PRIM */
    const char *name;    /* TY_NAMED (unresolved at AST stage) */
    Span        span;
    struct Ty  *elem;    /* TY_ARRAY / TY_SPAN */
    uint64_t    n;       /* TY_ARRAY length / TY_STR capacity */
    int32_t     decl_idx;/* TY_NAMED: resolved decl index; -1 until the
                            semantic pass resolves it (parse sets -1) */
} Ty;

/* Canonical type string ("u64", "Frame", "[u8; 4096]", "str[64]",
 * "span<u8>") — arena-owned. */
const char *ty_str(Arena *ar, const Ty *t);

typedef enum { ATTR_ALIGN, ATTR_PACKED, ATTR_SIMD, ATTR_OPT_PACK } AttrKind;

typedef struct {
    AttrKind    kind;
    uint64_t    num;     /* ATTR_ALIGN / ATTR_SIMD */
    bool        has_num;
    Span        span;    /* full "@..." span */
    const char *name;    /* raw text as written */
} Attr;

typedef struct Field {
    const char *name;
    Span        span;
    Ty         *ty;
    Attr       *attrs;
    size_t      nattrs;
    bool        poisoned; /* syntax error already reported; skip semantics */
} Field;

typedef struct Variant {
    const char *name;
    Span        span;
    int64_t     value;
} Variant;

typedef enum { DECL_STRUCT, DECL_ENUM, DECL_BITFLAGS } DeclKind;

typedef struct Decl {
    DeclKind    kind;
    const char *name;
    Span        span;
    Attr       *attrs;
    size_t      nattrs;
    Field      *fields;     /* struct */
    size_t      nfields;
    Variant    *variants;   /* enum / bitflags */
    size_t      nvariants;
    PrimKind    backing;    /* enum / bitflags */
    bool        poisoned;
    size_t      index;      /* declaration order (stable identity) */
} Decl;

/* Parses `sf` into decls_out/ndecls_out (arena-owned array). Diagnostics
 * go to `sink`. Always returns a syntactically-consistent AST: erroneous
 * nodes are marked poisoned. endian_big_out receives the declared
 * endianness (0 = little/canonical default, 1 = big). */
void parse_schema(const SourceFile *sf, Arena *ar, DiagSink *sink,
                  Decl **decls_out, size_t *ndecls_out, int *endian_big_out);

/* ------------------------------------------------------------------ */
/* Layout + IR (the frozen contract handed to Engineers 2 and 3)       */
/* ------------------------------------------------------------------ */
typedef struct FieldLayout {
    const char *name;
    const char *type_str;
    Span        span;
    const Ty   *ty;
    uint64_t    offset, size, align;
    uint32_t    orig_index;   /* declaration index before @optimize reorder */
} FieldLayout;

typedef struct { uint64_t offset, size; } Hole;

typedef struct DeclLayout {
    const char *name;
    DeclKind    kind;
    PrimKind    backing;         /* enum / bitflags */
    uint64_t    size, align;
    FieldLayout *fields;         /* struct: FINAL offset order */
    size_t      nfields;
    Hole       *holes;           /* internal padding gaps, offset order */
    size_t      nholes;
    uint64_t    internal_pad, trailing_pad;
    uint64_t    abi_hash;        /* WH64 over the structural manifest */
    bool        packed, reordered, has_align_attr, has_simd_attr;
    uint64_t    align_attr;      /* @align(N) value, 0 = none          */
    uint64_t    simd_attr;       /* @simd(N) value, 0 = none           */
    uint64_t    hint_opt_size;   /* size under @optimize(packing); 0 = no
                                   improvement available (inspect hint) */
    const Variant *variants;     /* borrowed from Decl */
    size_t      nvariants;
    size_t      decl_index;
} DeclLayout;

typedef struct WeftUnit {
    Arena       *ar;       /* owns everything below — freed last */
    SourceFile   file;
    DiagSink     diags;
    Decl        *decls;
    size_t       ndecls;
    DeclLayout  *layouts;  /* parallel to decls; valid iff has_layout */
    int          endian_big;
    uint64_t     abi_hash;      /* whole-schema structural identity   */
    uint64_t     schema_id;     /* whole-schema source identity       */
    uint64_t     fnv_debug;     /* FNV-1a-64 of the abi manifest      */
    bool         has_layout;    /* semantic pass completed w/o errors */
} WeftUnit;

#define WEFT_OPT_COLOR  1u
#define WEFT_OPT_WERROR 2u

/* Front doors. Result is never NULL; check weft_error_count(). The unit
 * owns its copy of the source text. */
WeftUnit *weft_compile_source(const char *path, const char *src, size_t len,
                              unsigned opts);
WeftUnit *weft_compile_file(const char *path, unsigned opts);
void      weft_unit_free(WeftUnit *u);

size_t            weft_error_count(const WeftUnit *u);
const DeclLayout *weft_find  (const WeftUnit *u, const char *name);
const FieldLayout *weft_field(const DeclLayout *dl, const char *name);

/* Dumps — deterministic byte-for-byte across runs and hosts. All return
 * 0 on success, nonzero on I/O failure (Law 4: loud, never silent). */
int weft_dump_json  (const WeftUnit *u, FILE *out);
int weft_dump_binary(const WeftUnit *u, ByteBuf *out);
int weft_emit_verify_header(const WeftUnit *u, FILE *out);
void weft_inspect(const WeftUnit *u, FILE *out);

/* Hashers (exposed for tests + downstream handshake tooling). */
uint64_t wh64   (const uint8_t *data, size_t len); /* WH64, RFC-0017 §5.2 */
uint64_t fnv1a64(const uint8_t *data, size_t len);

/* Canonical manifests (RFC-0017 §5.1). abi manifest = structural, name-
 * free; id manifest = full source identity. */
void weft_build_abi_manifest(const WeftUnit *u, ByteBuf *out);
void weft_build_id_manifest (const WeftUnit *u, ByteBuf *out);

/* Semantic analysis + layout. Called by the front doors; exposed for
 * tests. Fills u->layouts/hashes; diags report every violation found. */
void weft_run_layout(WeftUnit *u);

/* Fills per-decl abi_hash + whole-schema abi_hash/schema_id/fnv_debug
 * from the canonical manifests (RFC-0017 §5). Called by the layout pass. */
void weft_hash_all(WeftUnit *u);

#endif /* WEFTC_H */
