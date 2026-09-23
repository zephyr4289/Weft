/* weft_studio_internal.h — shared internal declarations for the Weft
 * Studio computational core (NOT part of the frozen ABI).
 *
 * Engine discipline:
 *   - only <string.h> (memcpy/memset/memmove/memcmp) beyond the frozen
 *     header: no stdio, no malloc, no syscalls — the same translation
 *     units compile for native hosts and wasm32 targets;
 *   - every pool is a fixed-capacity array inside a caller-provided
 *     context; exhaustion is an EBOUNDS refusal, never a crash;
 *   - bounded recursion only (type expressions <= 64, containment
 *     <= 256, enforced before descent);
 *   - all message text is composed into ctx pools with tiny local
 *     append helpers (no snprintf — keeps the WASM surface honest).
 */
#ifndef WEFT_STUDIO_INTERNAL_H
#define WEFT_STUDIO_INTERNAL_H

#include "weft_studio.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fixed pool bounds (public via weft_studio.h documentation)           */
/* ------------------------------------------------------------------ */
#define WEFTC_MAX_DECLS        1024u
#define WEFTC_MAX_FIELDS       16384u
#define WEFTC_MAX_FIELDS_PER_DECL 2048u
#define WEFTC_MAX_VARIANTS     8192u
#define WEFTC_MAX_NODES        32768u
#define WEFTC_MAX_TOKENS       32768u
#define WEFTC_MAX_HOLES        16384u
#define WEFTC_MAX_DIAGS        256u
#define WEFTC_MAX_LINES        65536u
#define WEFTC_MAX_NAME_POOL    (384u * 1024u)
#define WEFTC_MAX_TYSTR_POOL   (384u * 1024u)
#define WEFTC_MAX_MSG_POOL     (192u * 1024u)
#define WEFTC_MAX_MANIFEST     (1024u * 1024u)

#define WEFTC_IR_VERSION       1u
#define WEFTC_MAX_ALIGN        4096u
#define WEFTC_MAX_TOTAL_BYTES  UINT64_C(0x1000000000000) /* 2^48 */
#define WEFTC_SPAN_SIZE        16u
#define WEFTC_SPAN_ALIGN       8u
#define WEFTC_MAX_TYPE_DEPTH   64u
#define WEFTC_MAX_NEST_DEPTH   256u
#define WEFTC_MAX_IDENT        255u

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */
enum {
    TOK_EOF = 0, TOK_IDENT, TOK_INT, TOK_PUNCT,
    TOK_COMMENT_LINE, TOK_COMMENT_BLOCK, TOK_INVALID
};

typedef struct weft_tok_t {
    uint32_t off, len;      /* byte span in the source                */
    uint32_t line, col;     /* 1-based                                */
    uint64_t num;           /* TOK_INT value (saturated at u64 max)   */
    uint16_t kind;          /* TOK_*                                  */
    int8_t   neg;           /* TOK_INT leading '-'                    */
    uint8_t  overflow;      /* TOK_INT exceeded u64                   */
    uint8_t  ch;            /* TOK_PUNCT: the punctuation character   */
    uint8_t  reserved0;
    uint32_t reserved1;
} weft_tok_t;

/* ------------------------------------------------------------------ */
/* Compiler context (opaque in the ABI; defined here)                   */
/* ------------------------------------------------------------------ */
enum { DCL_STRUCT = 0, DCL_ENUM = 1, DCL_BITFLAGS = 2 };

typedef struct weftc_field_t {
    uint32_t name_off, name_len;   /* source span                    */
    uint32_t name_pool;            /* offset of NUL copy in name pool */
    uint32_t ty_node;              /* AST node of the type expression */
    uint32_t span_start, span_end; /* source byte span (diag ranges)  */
    uint32_t line, col;            /* 1-based, for diagnostics        */
    uint32_t attr_align;           /* 0 = none (else 1..4096)         */
    uint32_t attr_simd;            /* 0 = none                         */
    uint8_t  packed;               /* field-level @packed              */
    uint8_t  has_attr_align;
    uint8_t  has_attr_simd;
    uint8_t  bad;                  /* geometry unavailable             */
    /* layout results (final order filled by the layout engine)       */
    uint64_t offset, size, align, padding_after;
    uint32_t orig_index;
    uint32_t tystr_pool;           /* canonical type string            */
    uint32_t decl_ref;             /* named-type target decl, or UINT32_MAX */
    uint32_t ty_tag;               /* weft_type_tag                     */
} weftc_field_t;

typedef struct weftc_variant_t {
    uint32_t name_off, name_len;
    uint32_t name_pool;
    uint32_t line, col;
    uint32_t span_start, span_end;
    int64_t  value;
} weftc_variant_t;

typedef struct weftc_decl_t {
    uint32_t name_off, name_len;
    uint32_t name_pool;
    uint32_t span_start, span_end;
    uint32_t line, col;
    uint32_t kw_line, kw_col;       /* the decl keyword span (diags)   */
    uint8_t  kind;                  /* DCL_*                            */
    uint8_t  packed;
    uint8_t  has_align_attr, has_simd_attr, reordered, poisoned;
    uint16_t align_attr, simd_attr; /* 1..4096                          */
    uint32_t first_field, nfields;
    uint32_t first_variant, nvariants;
    uint32_t backing;               /* prim tag for enum/bitflags       */
    /* layout results */
    uint64_t size, align, internal_pad, trailing_pad, optimize_hint;
    uint64_t abi_hash;
    uint32_t hole_start, hole_count;
    uint32_t nplanned;               /* planned (non-bad) field count */
    uint32_t flags;                 /* WEFT_SLF_*                       */
} weftc_decl_t;

/* Diagnostic staging: byte spans + message pool offsets; finalized into
 * weft_diag_t (LSP 0-based line/col) at compile end. */
typedef struct weftc_diagst_t {
    uint32_t code;
    uint16_t sev;
    uint16_t reserved;
    uint32_t msg_off;               /* msg pool, NUL-terminated         */
    uint32_t fix_off;               /* msg pool, or UINT32_MAX          */
    uint32_t start_off, end_off;    /* source byte span, half-open      */
} weftc_diagst_t;

struct weftc_ctx_t {
    /* input (borrowed between compile calls) */
    const char *src;
    size_t      src_len;

    /* lexer products */
    weft_tok_t  toks[WEFTC_MAX_TOKENS];
    uint32_t    ntoks;
    uint32_t    line_starts[WEFTC_MAX_LINES];
    uint32_t    nlines;

    /* AST */
    weft_ast_node_t nodes[WEFTC_MAX_NODES];
    uint32_t    nnodes;

    /* declarations */
    weftc_decl_t    decls[WEFTC_MAX_DECLS];
    uint32_t    ndecls;
    weftc_field_t   fields[WEFTC_MAX_FIELDS];
    uint32_t    nfields;
    weftc_variant_t variants[WEFTC_MAX_VARIANTS];
    uint32_t    nvariants;
    weft_hole_t holes[WEFTC_MAX_HOLES];
    uint32_t    nholes;

    /* pools */
    char        name_pool[WEFTC_MAX_NAME_POOL];
    uint32_t    name_pool_n;
    char        tystr_pool[WEFTC_MAX_TYSTR_POOL];
    uint32_t    tystr_pool_n;
    char        msg_pool[WEFTC_MAX_MSG_POOL];
    uint32_t    msg_pool_n;
    uint8_t     manifest[WEFTC_MAX_MANIFEST];
    uint32_t    manifest_n;

    /* diagnostics */
    weftc_diagst_t staging[WEFTC_MAX_DIAGS];
    uint32_t    nstaging;
    uint32_t    diags_dropped;
    weft_diag_t diags[WEFTC_MAX_DIAGS];
    uint32_t    ndiags;

    /* reflection (ABI view, rebuilt every compile) */
    weft_struct_layout_t layouts[WEFTC_MAX_DECLS];
    weft_field_layout_t  flayouts[WEFTC_MAX_FIELDS];
    uint32_t    nlayouts;

    /* schema state */
    uint64_t    abi_hash, schema_id, fnv1a64_abi;
    uint8_t     endian_big;
    uint8_t     endianness_declared;
    uint8_t     cache_line;          /* 64 default, 128 in wide mode   */
    uint8_t     reserved0;

    /* layout scratch (rebuilt every compile) */
    uint32_t plan_field[WEFTC_MAX_FIELDS_PER_DECL];
    uint64_t plan_align[WEFTC_MAX_FIELDS_PER_DECL];
    uint32_t field_order[WEFTC_MAX_FIELDS]; /* final (planned) order   */
    uint32_t by_name[WEFTC_MAX_DECLS];
    uint8_t  dstate[WEFTC_MAX_DECLS];   /* 0 clean, 1 visiting, 2 done */
    uint32_t dpath[WEFTC_MAX_NEST_DEPTH + 1u];
    uint32_t dpath_n;
    uint8_t  compiled;

    /* one-shot fix-suggestion slot (WE007 did-you-mean) */
    char     pending_fix[192];
    uint8_t  pending_fix_valid;

    /* refusal bookkeeping (returned by weftc_compile) */
    uint8_t     capacity_hit;        /* pools exhausted -> EBOUNDS     */
};

/* shared engine helpers (weftc_inmem.c) */
extern const char *const weft_prim_names[13];   /* index = tag, [0] unused */
extern const uint8_t  weft_prim_sizes[13];
extern const uint8_t  weft_prim_aligns[13];

uint64_t weft_wh64(const uint8_t *data, size_t len);
uint64_t weft_fnv1a64(const uint8_t *data, size_t len);
void     weft_mix_u64le(uint8_t *out, uint64_t v);   /* manifest writer helper */

/* pool append helper used across engines: appends at cursor, refuses
 * (returns 0) when it would overflow — callers raise EBOUNDS. */
typedef struct weft_strpool_t {
    char    *base;
    uint32_t cap;
    uint32_t n;
} weft_strpool_t;

int  weft_strpool_add(weft_strpool_t *p, const char *s, uint32_t len);

#endif /* WEFT_STUDIO_INTERNAL_H */
