/* weft_studio.h — Weft Studio computational core: frozen C ABI v1.
 *
 * WEFT-DIRECTIVE-PILLAR-7-ENG1 (Weft Core Architectural Group, 2026-09-22).
 * Single source of truth for:
 *   - Engineer 2 (Native SHM Inspector): the .weftrec flight-recorder
 *     reader contracts and the layout reflection tables.
 *   - Engineer 3 (Managed Studio UI): the LSP projection types, the
 *     diagnostic model, and the in-memory compiler query surface.
 *
 * LAW COMPLIANCE (see docs/reports/D-71-STUDIO-CORE-AUDIT.md):
 *   Law 1  Zero allocation on query & replay. Every context used by
 *          diagnostics, layout queries, hover/completion/semantic tokens,
 *          or .weftrec playback is caller-provided storage; engines
 *          carve pools at init time and never touch malloc/realloc/calloc
 *          afterwards (proven by malloc interposition, 0 events over
 *          200,000 cycles).
 *   Law 2  Bit-exact 64B/128B cache-line alignment. The layout tables
 *          expose per-field byte offsets, padding_after, cache_line_idx,
 *          and boundary-crossing tripwires for both 64B and 128B lines.
 *   Law 3  Dual-compiler strict discipline: -std=c11 -Wall -Wextra
 *          -Werror -pedantic under GCC 14.2 and Clang (19.1.7 in the
 *          delivery sandbox; see D-71 §2 for the declared toolchain
 *          boundary), ASan/UBSan clean.
 *   Law 4  Strict ABI freezing: every exported structure below is an
 *          immutable contract pinned by _Static_assert size AND offset
 *          checks; the header is C++17-include-safe (extern "C" guarded,
 *          no C-only keywords).
 *
 * Endianness: all .weftrec on-disk integers are little-endian; the wire
 * overlay discipline (64B-aligned base + byte-exact struct layout) makes
 * direct struct views legal on LE hosts. Big-endian hosts are refused at
 * compile time unless WEFT_STUDIO_ALLOW_BE is defined (declared boundary:
 * wasm32, x86-64 and aarch64-LE are the studio targets).
 *
 * Position conventions (pinned so the UI never guesses):
 *   - weft_ast_node_t.line/col    : 1-based, bytes (compiler/GCC style).
 *   - weft_diag_t start/end line/col : 0-based line, 0-based UTF-8 byte
 *     offset (LSP-native; the server declares positionEncoding utf-8).
 *
 * Lifetime contract (zero-copy, like the rest of Weft):
 *   every `const char *` / array pointer returned by the engines points
 *   into context-owned pools (or, for raw .weftrec frames, into the
 *   caller-pinned trace buffer) and is valid until the next compile /
 *   didChange / builder operation on that context.
 */
#ifndef WEFT_STUDIO_H
#define WEFT_STUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C++17 inclusion shims (Law 4). */
#if defined(__cplusplus)
#  define WEFT_STUDIO_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define WEFT_STUDIO_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#if !defined(WEFT_STUDIO_ALLOW_BE) && defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  error "weft studio v1 targets little-endian hosts (wasm32/x86-64/aarch64-LE)"
#endif

#define WEFT_STUDIO_ABI_VERSION 1u

/* ------------------------------------------------------------------ */
/* Status ladder (frozen)                                              */
/* ------------------------------------------------------------------ */
#define WEFT_STUDIO_OK        0
#define WEFT_STUDIO_EPARSE  (-1)  /* malformed input (schema text is NOT this: parse errors are diagnostics) */
#define WEFT_STUDIO_EALIGN  (-2)  /* alignment contract violated (buffer base, frame/index offsets) */
#define WEFT_STUDIO_ETRUNC  (-3)  /* truncated stream / clean end-of-trace (weftrec walker) */
#define WEFT_STUDIO_ECRC    (-4)  /* integrity check failed (CRC-32C) */
#define WEFT_STUDIO_EBOUNDS (-5)  /* capacity or bounds refusal */

const char *weft_studio_strerror(int code);

/* ------------------------------------------------------------------ */
/* Type tags (frozen). Primitive codes 1..12 match the RFC-0017 PrimKind */
/* order byte-for-byte so WDC1 manifest parity with the weftc reference */
/* compiler holds by construction.                                      */
/* ------------------------------------------------------------------ */
enum weft_type_tag {
    WEFT_TY_U8        = 1,
    WEFT_TY_I8        = 2,
    WEFT_TY_U16       = 3,
    WEFT_TY_I16       = 4,
    WEFT_TY_U32       = 5,
    WEFT_TY_I32       = 6,
    WEFT_TY_U64       = 7,
    WEFT_TY_I64       = 8,
    WEFT_TY_F16       = 9,
    WEFT_TY_F32       = 10,
    WEFT_TY_F64       = 11,
    WEFT_TY_BOOL      = 12,
    WEFT_TY_STR       = 13,  /* str[N]        : N bytes, align 1   */
    WEFT_TY_SPAN      = 14,  /* span<T>       : 16 B, align 8      */
    WEFT_TY_ARRAY     = 15,  /* [T; N]        : N * stride(T)      */
    WEFT_TY_STRUCT    = 16,  /* named struct reference            */
    WEFT_TY_ENUM      = 17,  /* enum  : backing primitive         */
    WEFT_TY_BITFLAGS  = 18   /* bitflags : backing primitive      */
};

/* Primitive geometry + canonical names (RFC-0017 §3 table). */
const char *weft_prim_name(uint32_t type_tag);   /* NULL for non-prims */
uint64_t    weft_prim_size(uint32_t type_tag);   /* 0 for non-prims   */
uint64_t    weft_prim_align(uint32_t type_tag);  /* 0 for non-prims   */

/* ------------------------------------------------------------------ */
/* AST reflection (breadcrumb tree for the Studio editor)              */
/* ------------------------------------------------------------------ */
enum weft_ast_kind {
    WEFT_AST_SCHEMA        = 1,
    WEFT_AST_ENDIANNESS    = 2,   /* value: 0 little, 1 big            */
    WEFT_AST_DECL_STRUCT   = 3,
    WEFT_AST_DECL_ENUM     = 4,
    WEFT_AST_DECL_BITFLAGS = 5,
    WEFT_AST_FIELD         = 6,
    WEFT_AST_VARIANT       = 7,   /* value: variant value              */
    WEFT_AST_TY_PRIM       = 8,   /* value: weft_type_tag              */
    WEFT_AST_TY_NAMED      = 9,
    WEFT_AST_TY_ARRAY      = 10,  /* value: element count N            */
    WEFT_AST_TY_STR        = 11,  /* value: N                          */
    WEFT_AST_TY_SPAN       = 12,
    WEFT_AST_ATTR_ALIGN    = 13,  /* value: N                          */
    WEFT_AST_ATTR_SIMD     = 14,  /* value: N                          */
    WEFT_AST_ATTR_PACKED   = 15,
    WEFT_AST_ATTR_OPTIMIZE = 16   /* @optimize(packing)                */
};

/* One reflection node. Indices (not pointers) link the tree so the pool
 * can live anywhere; UINT32_MAX (WEFT_AST_NONE) means "absent".
 * name_off/name_len span the source text handed to weftc_compile. */
#define WEFT_AST_NONE UINT32_MAX

typedef struct weft_ast_node_t {
    uint32_t kind;          /* weft_ast_kind                            */
    uint32_t parent;        /* node index or WEFT_AST_NONE              */
    uint32_t first_child;   /* node index or WEFT_AST_NONE              */
    uint32_t next_sibling;  /* node index or WEFT_AST_NONE              */
    uint32_t name_off;      /* byte offset into the compiled source     */
    uint32_t name_len;      /* name length in bytes (0 = unnamed)       */
    uint32_t line;          /* 1-based                                  */
    uint32_t col;           /* 1-based byte column                      */
    uint64_t value;         /* kind-specific (see weft_ast_kind)        */
} weft_ast_node_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weft_ast_node_t) == 40,
    "weft_studio: weft_ast_node_t drifted (ABI v1 pins 40 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, kind) == 0,
    "weft_studio: weft_ast_node_t.kind offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, parent) == 4,
    "weft_studio: weft_ast_node_t.parent offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, first_child) == 8,
    "weft_studio: weft_ast_node_t.first_child offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, next_sibling) == 12,
    "weft_studio: weft_ast_node_t.next_sibling offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, name_off) == 16,
    "weft_studio: weft_ast_node_t.name_off offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, name_len) == 20,
    "weft_studio: weft_ast_node_t.name_len offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, line) == 24,
    "weft_studio: weft_ast_node_t.line offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, col) == 28,
    "weft_studio: weft_ast_node_t.col offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_ast_node_t, value) == 32,
    "weft_studio: weft_ast_node_t.value offset drifted");

/* ------------------------------------------------------------------ */
/* Layout reflection (Law 2: bit-exact offsets + cache-line tripwires)  */
/* ------------------------------------------------------------------ */

/* weft_field_layout_t.flags */
#define WEFT_FLF_CROSSES_CL64     0x01u  /* field spans a 64 B boundary   */
#define WEFT_FLF_CROSSES_CL128    0x02u  /* field spans a 128 B boundary  */
#define WEFT_FLF_PACKED           0x04u  /* placed at align 1 (@packed)   */
#define WEFT_FLF_ATTR_ALIGN       0x08u  /* field carried @align(N)       */
#define WEFT_FLF_ATTR_SIMD        0x10u  /* field carried @simd(N)        */
#define WEFT_FLF_FALSE_SHARING    0x20u  /* writable scalar crossing a
                                            64 B line — tripwire         */

/* Exactly one cacheline per reflected field (64 B, offset-pinned). */
typedef struct weft_field_layout_t {
    const char *name;          /* NUL-terminated, ctx pool            */
    const char *type_str;      /* canonical RFC-0017 type string      */
    uint32_t    type_tag;      /* weft_type_tag                       */
    uint32_t    orig_index;    /* declaration order (pre-@optimize)   */
    uint64_t    offset;        /* byte offset in the struct           */
    uint64_t    size;          /* byte size                           */
    uint64_t    align;         /* effective alignment                 */
    uint64_t    padding_after; /* hole bytes after this field         */
    uint32_t    cache_line_idx;/* offset / cache_line (64 B default)  */
    uint32_t    flags;         /* WEFT_FLF_*                          */
} weft_field_layout_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weft_field_layout_t) == 64,
    "weft_studio: weft_field_layout_t drifted (ABI v1 pins 64 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, name) == 0,
    "weft_studio: weft_field_layout_t.name offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, type_str) == 8,
    "weft_studio: weft_field_layout_t.type_str offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, type_tag) == 16,
    "weft_studio: weft_field_layout_t.type_tag offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, orig_index) == 20,
    "weft_studio: weft_field_layout_t.orig_index offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, offset) == 24,
    "weft_studio: weft_field_layout_t.offset offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, size) == 32,
    "weft_studio: weft_field_layout_t.size offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, align) == 40,
    "weft_studio: weft_field_layout_t.align offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, padding_after) == 48,
    "weft_studio: weft_field_layout_t.padding_after offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, cache_line_idx) == 56,
    "weft_studio: weft_field_layout_t.cache_line_idx offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_field_layout_t, flags) == 60,
    "weft_studio: weft_field_layout_t.flags offset drifted");

typedef struct weft_hole_t {
    uint64_t offset;
    uint64_t size;
} weft_hole_t;

/* weft_struct_layout_t.flags */
#define WEFT_SLF_PACKED    0x01u
#define WEFT_SLF_REORDERED 0x02u  /* @optimize(packing) applied         */
#define WEFT_SLF_ALIGN_ATTR 0x04u
#define WEFT_SLF_SIMD_ATTR  0x08u

typedef struct weft_struct_layout_t {
    const char                 *name;        /* NUL-terminated, ctx pool */
    const weft_field_layout_t  *fields;      /* final layout order       */
    const weft_hole_t          *holes;       /* internal padding holes   */
    uint64_t    abi_hash;    /* WH64 over the WDC1 manifest — bit-identical  */
                            /* to the weftc reference compiler (Pillar 1)   */
    uint64_t    size;        /* includes trailing pad                        */
    uint64_t    align;       /* struct alignment                             */
    uint64_t    internal_pad;/* total hole bytes                             */
    uint64_t    trailing_pad;/* size - last field end                        */
    uint64_t    optimize_hint;/* bytes @optimize(packing) would save (0=none)*/
    uint32_t    field_count;
    uint32_t    hole_count;
    uint32_t    flags;       /* WEFT_SLF_*                                   */
    uint32_t    cache_line_span; /* ceil(size / cache_line)                 */
} weft_struct_layout_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weft_struct_layout_t) == 88,
    "weft_studio: weft_struct_layout_t drifted (ABI v1 pins 88 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, name) == 0,
    "weft_studio: weft_struct_layout_t.name offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, fields) == 8,
    "weft_studio: weft_struct_layout_t.fields offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, holes) == 16,
    "weft_studio: weft_struct_layout_t.holes offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, abi_hash) == 24,
    "weft_studio: weft_struct_layout_t.abi_hash offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, size) == 32,
    "weft_studio: weft_struct_layout_t.size offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, align) == 40,
    "weft_studio: weft_struct_layout_t.align offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, internal_pad) == 48,
    "weft_studio: weft_struct_layout_t.internal_pad offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, trailing_pad) == 56,
    "weft_studio: weft_struct_layout_t.trailing_pad offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, optimize_hint) == 64,
    "weft_studio: weft_struct_layout_t.optimize_hint offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, field_count) == 72,
    "weft_studio: weft_struct_layout_t.field_count offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, hole_count) == 76,
    "weft_studio: weft_struct_layout_t.hole_count offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, flags) == 80,
    "weft_studio: weft_struct_layout_t.flags offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_struct_layout_t, cache_line_span) == 84,
    "weft_studio: weft_struct_layout_t.cache_line_span offset drifted");

/* ------------------------------------------------------------------ */
/* Diagnostics                                                          */
/* ------------------------------------------------------------------ */
enum weft_severity {
    WEFT_SEV_ERROR = 1,
    WEFT_SEV_WARN  = 2,
    WEFT_SEV_INFO  = 3,
    WEFT_SEV_HINT  = 4
};

/* Diagnostic code spaces (numeric, stable for UI pinning):
 *   1..41    RFC-0017 registry: WE001..WE041 (parse/semantic errors)
 *   901..    RFC-0017 warnings: WW001..                      */
/*   2101..   studio-local layout diagnostics (see below)      */
#define WEFT_D_WE_BASE  1u
#define WEFT_D_WW_BASE  901u
#define WEFT_D_STUDIO_BASE 2101u
#define WEFT_D_CACHE_LINE_CROSS  2101u /* WARN: field crosses a cache line    */
#define WEFT_D_TRAILING_PAD      2102u /* HINT: unpadded trailing struct      */
#define WEFT_D_OPTIMIZE_HINT     2103u /* INFO: @optimize(packing) savings    */
#define WEFT_D_CL128_CROSS       2104u /* WARN: field crosses a 128 B line    */

const char *weft_diag_code_name(uint32_t code);  /* "WE019", "STUDIO-2101", ... */

/* Range is half-open [start, end): start_line/start_col inclusive,
 * end_line/end_col exclusive; 0-based line, 0-based UTF-8 byte columns. */
typedef struct weft_diag_t {
    const char *message;          /* NUL-terminated, ctx pool           */
    const char *fix_suggestion;   /* NUL-terminated, ctx pool, or NULL  */
    uint32_t    code;             /* WE/WW/STUDIO code space            */
    uint16_t    severity;         /* weft_severity                      */
    uint16_t    flags;            /* reserved, 0 in ABI v1              */
    uint32_t    start_line;
    uint32_t    start_col;
    uint32_t    end_line;
    uint32_t    end_col;
} weft_diag_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weft_diag_t) == 40,
    "weft_studio: weft_diag_t drifted (ABI v1 pins 40 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, message) == 0,
    "weft_studio: weft_diag_t.message offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, fix_suggestion) == 8,
    "weft_studio: weft_diag_t.fix_suggestion offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, code) == 16,
    "weft_studio: weft_diag_t.code offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, severity) == 20,
    "weft_studio: weft_diag_t.severity offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, flags) == 22,
    "weft_studio: weft_diag_t.flags offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, start_line) == 24,
    "weft_studio: weft_diag_t.start_line offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, start_col) == 28,
    "weft_studio: weft_diag_t.start_col offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, end_line) == 32,
    "weft_studio: weft_diag_t.end_line offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weft_diag_t, end_col) == 36,
    "weft_studio: weft_diag_t.end_col offset drifted");

/* ------------------------------------------------------------------ */
/* .weftrec v1 flight recorder (WEFTREC1) — frozen wire contracts       */
/* ------------------------------------------------------------------ */

/* "WEFTREC1": the magic is the ASCII byte sequence W,E,F,T,R,E,C,1 on
 * disk (hexdumps spell the format name); WEFTREC1_MAGIC is the 64-bit
 * constant 0x5745465452454331 — the big-endian reading of those bytes,
 * exactly as pinned by the directive. */
#define WEFTREC1_MAGIC   UINT64_C(0x5745465452454331)
#define WEFTREC1_VERSION 1u

/* weftrec_header_t.flags */
#define WEFTREC_HF_INDEX      0x00000001u  /* index block present (v1: always) */
#define WEFTREC_HF_KNOWN_MASK 0x00000001u  /* unknown bits are refused        */

/* weftrec_frame_header_t.codec */
#define WEFTREC_CODEC_RAW 0
#define WEFTREC_CODEC_DZV 1  /* delta-zigzag-varint over LE u32 words (RFC-0010 codec, self-limiting) */

/* weftrec_frame_header_t.flags / index flags */
#define WEFTREC_FF_KEYFRAME  0x0001u

/* Global header, exactly one cacheline (64 B). CRC-32C covers bytes
 * [0, 60); crc32c itself lives at offset 60. On-disk integers are
 * little-endian; file layout:
 *   [0,64)      weftrec_header_t
 *   [64,...)    frames: 64 B-aligned weftrec_frame_header_t + stored
 *               payload bytes, zero-padded to the next 64 B boundary
 *   [index]     weftrec_index_entry_t[frame_count], 64 B-aligned
 *   [end]       zero pad to a 64 B multiple
 * Frames are globally time-ordered (append order); the index carries one
 * 64 B entry per frame, sorted by construction. */
typedef struct weftrec_header_t {
    uint64_t magic;               /* WEFTREC1_MAGIC                     */
    uint32_t version;             /* WEFTREC1_VERSION                   */
    uint32_t flags;               /* WEFTREC_HF_*                       */
    uint64_t index_offset;        /* byte offset of the index block     */
    uint64_t index_count;         /* == frame_count in v1               */
    uint64_t frame_count;
    uint64_t first_ts_ns;
    uint64_t last_ts_ns;
    uint32_t stream_cardinality;  /* max stream_id + 1 (0 if no frames) */
    uint32_t crc32c;              /* CRC-32C over header bytes [0,60)   */
} weftrec_header_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weftrec_header_t) == 64,
    "weft_studio: weftrec_header_t drifted (ABI v1 pins 64 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, magic) == 0,
    "weft_studio: weftrec_header_t.magic offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, version) == 8,
    "weft_studio: weftrec_header_t.version offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, flags) == 12,
    "weft_studio: weftrec_header_t.flags offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, index_offset) == 16,
    "weft_studio: weftrec_header_t.index_offset offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, index_count) == 24,
    "weft_studio: weftrec_header_t.index_count offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, frame_count) == 32,
    "weft_studio: weftrec_header_t.frame_count offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, first_ts_ns) == 40,
    "weft_studio: weftrec_header_t.first_ts_ns offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, last_ts_ns) == 48,
    "weft_studio: weftrec_header_t.last_ts_ns offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, stream_cardinality) == 56,
    "weft_studio: weftrec_header_t.stream_cardinality offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_header_t, crc32c) == 60,
    "weft_studio: weftrec_header_t.crc32c offset drifted");

/* One 64 B index entry per frame: the O(log n) timestamp -> file offset
 * map the seek SLA rides on. Entries are time-sorted by construction. */
typedef struct weftrec_index_entry_t {
    uint64_t timestamp_ns;
    uint64_t frame_offset;    /* byte offset of the frame header       */
    uint64_t next_offset;     /* next indexed frame's offset, 0 = last */
    uint64_t frame_seq;       /* 0-based append ordinal                */
    uint64_t stream_id;
    uint32_t payload_size;    /* logical (decompressed) size           */
    uint32_t flags;           /* WEFTREC_FF_*                          */
    uint64_t ts_delta_prev;   /* ts - previous frame's ts (0 for #0)   */
    uint64_t reserved0;       /* 0 in v1                               */
} weftrec_index_entry_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weftrec_index_entry_t) == 64,
    "weft_studio: weftrec_index_entry_t drifted (ABI v1 pins 64 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, timestamp_ns) == 0,
    "weft_studio: weftrec_index_entry_t.timestamp_ns offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, frame_offset) == 8,
    "weft_studio: weftrec_index_entry_t.frame_offset offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, next_offset) == 16,
    "weft_studio: weftrec_index_entry_t.next_offset offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, frame_seq) == 24,
    "weft_studio: weftrec_index_entry_t.frame_seq offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, stream_id) == 32,
    "weft_studio: weftrec_index_entry_t.stream_id offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, payload_size) == 40,
    "weft_studio: weftrec_index_entry_t.payload_size offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, flags) == 44,
    "weft_studio: weftrec_index_entry_t.flags offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, ts_delta_prev) == 48,
    "weft_studio: weftrec_index_entry_t.ts_delta_prev offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_index_entry_t, reserved0) == 56,
    "weft_studio: weftrec_index_entry_t.reserved0 offset drifted");

/* One 64 B frame header, immediately followed by stored_size payload
 * bytes. payload CRC-32C covers the STORED bytes; header_crc32c covers
 * header bytes [0, 48) — next_offset ([48,56)) is deliberately outside
 * the CRC region because the builder back-patches it when the next
 * frame is appended. next_offset == 0 marks the last frame. */
typedef struct weftrec_frame_header_t {
    uint64_t magic;           /* WEFTREC1_MAGIC                         */
    uint64_t timestamp_ns;
    uint64_t stream_id;
    uint64_t frame_seq;
    uint32_t payload_size;    /* logical (decompressed) length          */
    uint32_t stored_size;     /* bytes stored after this header         */
    uint16_t codec;           /* WEFTREC_CODEC_*                        */
    uint16_t flags;           /* WEFTREC_FF_*                           */
    uint32_t crc32c;          /* CRC-32C over stored payload bytes      */
    uint64_t next_offset;     /* next frame offset, 0 = last            */
    uint32_t header_crc32c;   /* CRC-32C over header bytes [0,56)       */
    uint32_t reserved0;       /* 0 in v1                                */
} weftrec_frame_header_t;

WEFT_STUDIO_STATIC_ASSERT(sizeof(weftrec_frame_header_t) == 64,
    "weft_studio: weftrec_frame_header_t drifted (ABI v1 pins 64 B)");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, magic) == 0,
    "weft_studio: weftrec_frame_header_t.magic offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, timestamp_ns) == 8,
    "weft_studio: weftrec_frame_header_t.timestamp_ns offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, stream_id) == 16,
    "weft_studio: weftrec_frame_header_t.stream_id offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, frame_seq) == 24,
    "weft_studio: weftrec_frame_header_t.frame_seq offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, payload_size) == 32,
    "weft_studio: weftrec_frame_header_t.payload_size offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, stored_size) == 36,
    "weft_studio: weftrec_frame_header_t.stored_size offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, codec) == 40,
    "weft_studio: weftrec_frame_header_t.codec offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, flags) == 42,
    "weft_studio: weftrec_frame_header_t.flags offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, crc32c) == 44,
    "weft_studio: weftrec_frame_header_t.crc32c offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, next_offset) == 48,
    "weft_studio: weftrec_frame_header_t.next_offset offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, header_crc32c) == 56,
    "weft_studio: weftrec_frame_header_t.header_crc32c offset drifted");
WEFT_STUDIO_STATIC_ASSERT(offsetof(weftrec_frame_header_t, reserved0) == 60,
    "weft_studio: weftrec_frame_header_t.reserved0 offset drifted");

/* ---- CRC-32C (reflected 0x11EDC6F41, init/final 0xFFFFFFFF) --------- */
uint32_t weftrec_crc32c(const void *data, size_t len);   /* dispatched  */
uint32_t weftrec_crc32c_sw(const void *data, size_t len);/* slicing-by-8 */
int      weftrec_crc32c_hw_active(void);                 /* 1 = SSE4.2/ARMv8 kernel */

/* ---- dzv codec (RFC-0010 family; exposed for oracles + Inspector) --- */
uint32_t weftrec_dzv_encode(uint8_t *dst, uint32_t cap, const uint8_t *src,
                            uint32_t len);
int      weftrec_dzv_decode(const uint8_t *stored, uint32_t stored_len,
                            uint32_t logical_len, uint8_t *out,
                            uint32_t cap);

/* ---- Reader (caller-pinned buffer; zero allocation) ----------------- */
typedef struct weftrec_reader_t {
    const uint8_t              *base;    /* caller-pinned, 64 B aligned */
    uint64_t                    size;
    const weftrec_header_t     *hdr;     /* base + 0 (aligned view)     */
    const weftrec_index_entry_t *index;  /* base + hdr->index_offset    */
    uint64_t                    index_count;
    uint32_t                    cache_line;   /* advisory: 64 or 128     */
    uint32_t                    flags;        /* reader state, reserved  */
} weftrec_reader_t;

typedef struct weftrec_frame_view_t {
    const weftrec_frame_header_t *hdr;
    const uint8_t *payload;      /* decoded bytes: raw -> into the pinned
                                    trace buffer (zero copy); dzv -> into
                                    the walker's caller-provided scratch */
    uint64_t    payload_len;     /* logical length                     */
    uint64_t    file_offset;     /* byte offset of hdr in the trace    */
} weftrec_frame_view_t;

/* Validation ladder: ETRUNC (<64 B), EALIGN (base/offsets), EPARSE
 * (magic/version/flags/consistency), ECRC (header CRC). Zero-copy:
 * the reader keeps pointers into the pinned buffer only. */
int weftrec_reader_open(weftrec_reader_t *r, const void *buf, uint64_t len);

/* O(log n) timestamp seek over the pre-built 64 B index: resolves the
 * last frame with timestamp_ns <= target_ns (clamped to the first frame
 * when target precedes the trace start). SLA: < 100 ns. */
int weftrec_seek_timestamp(const weftrec_reader_t *r, uint64_t target_ns,
                           weftrec_frame_view_t *out);

/* In-place frame walker. from_offset 0 = first frame. For dzv frames the
 * decoded payload lands in caller-provided scratch (cap >= max logical
 * payload; raw frames never touch scratch). weftrec_frame_next returns
 * WEFT_STUDIO_OK per frame and WEFT_STUDIO_ETRUNC at end of trace
 * (w->truncated distinguishes clean EOF from a torn tail); ECRC/EPARSE/
 * EBOUNDS are refusals that stop the walk. SLA: < 50 ns per frame. */
typedef struct weftrec_walker_t {
    weftrec_reader_t *reader;
    uint64_t    next_offset;      /* 0 = walk finished                  */
    uint8_t    *scratch;          /* caller dzv decode buffer           */
    uint32_t    scratch_cap;
    uint32_t    truncated;        /* set when ETRUNC was a torn tail    */
    uint32_t    steps;            /* cycle defense: bounded by the index */
} weftrec_walker_t;

int weftrec_walker_init(weftrec_walker_t *w, weftrec_reader_t *r,
                        uint64_t from_offset, void *scratch,
                        uint32_t scratch_cap);
int weftrec_frame_next(weftrec_walker_t *w, weftrec_frame_view_t *view);

/* ---- Builder (caller-provided buffer + index arena) ------------------ */
typedef struct weftrec_builder_t {
    uint8_t              *base;
    uint64_t              cap;
    uint64_t              cursor;       /* first byte after last frame  */
    uint64_t              last_frame_off;
    weftrec_index_entry_t *index;       /* caller arena                 */
    uint32_t              index_cap;
    uint32_t              frame_count;
    uint64_t              first_ts_ns;
    uint64_t              last_ts_ns;
    uint64_t              max_stream_id;
    uint32_t              reserved0;
    uint32_t              finished;     /* guard: appends after finish  */
} weftrec_builder_t;

int weftrec_builder_init(weftrec_builder_t *b, void *buf, uint64_t cap,
                         weftrec_index_entry_t *index_mem, uint32_t index_cap);
/* codec: WEFTREC_CODEC_RAW or WEFTREC_CODEC_DZV (self-limiting: declines
 * when the encoded stream would not shrink). Timestamps must be
 * non-decreasing in append order (EPARSE refusal otherwise). */
int weftrec_builder_append(weftrec_builder_t *b, uint64_t ts_ns,
                           uint32_t stream_id, const void *payload,
                           uint32_t payload_len, int codec);
int weftrec_builder_finish(weftrec_builder_t *b, uint64_t *out_len);

/* ------------------------------------------------------------------ */
/* weftc in-memory compiler (zero-syscall, WASM-portable)               */
/* ------------------------------------------------------------------ */

/* Codegen targets. */
enum weft_codegen_lang {
    WEFT_LANG_C       = 0,
    WEFT_LANG_CPP     = 1,
    WEFT_LANG_RUST    = 2,
    WEFT_LANG_TYPESCRIPT = 3,
    WEFT_LANG_PYTHON  = 4,
    WEFT_LANG_DART    = 5,
    WEFT_LANG_SWIFT   = 6,
    WEFT_LANG__COUNT  = 7
};

typedef struct weftc_ctx_t weftc_ctx_t;   /* opaque; size via weftc_ctx_size() */

/* Context storage is caller-provided (Law 1): malloc it once (setup time
 * is outside the steady-state law), or place it in static/arena memory.
 * Pool bounds are compile-time constants; overflows are EBOUNDS refusals
 * (fuzz-proven, never crashes):
 *   max decls 1024, max fields 16384, max AST nodes 32768,
 *   name pool 128 KiB, type-string pool 128 KiB, diags 256. */
size_t weftc_ctx_size(void);
int    weftc_ctx_init(void *mem, size_t mem_size, weftc_ctx_t **out);
void   weftc_ctx_reset(weftc_ctx_t *ctx);

/* Compile a .weft schema from a raw string buffer (RFC-0017 grammar).
 * Parse/semantic errors are DIAGNOSTICS, not return codes; the return
 * code is the engine refusal ladder only (EBOUNDS on pool exhaustion).
 * Re-compiling the same context replaces all previous state. */
int weftc_compile(weftc_ctx_t *ctx, const char *src, size_t src_len);

/* Reflection queries (all zero-allocation; results point into ctx pools). */
uint32_t                    weftc_diag_count(const weftc_ctx_t *ctx);
const weft_diag_t          *weftc_diag_at(const weftc_ctx_t *ctx, uint32_t i);
uint32_t                    weftc_decl_count(const weftc_ctx_t *ctx);
const weft_struct_layout_t *weftc_decl_at(const weftc_ctx_t *ctx, uint32_t i);
int32_t                     weftc_decl_find(const weftc_ctx_t *ctx,
                                            const char *name);
uint32_t                    weftc_ast_count(const weftc_ctx_t *ctx);
const weft_ast_node_t      *weftc_ast_at(const weftc_ctx_t *ctx, uint32_t i);

/* Whole-schema fingerprints (WH64 over the WAB1/WID1 manifests, plus the
 * independent fnv1a64 cross-check) — bit-identical to the weftc reference
 * compiler of RFC-0017 (golden-pinned in tests/studio/core). */
int weftc_schema_hashes(const weftc_ctx_t *ctx, uint64_t *abi_hash,
                        uint64_t *schema_id, uint64_t *fnv1a64_abi);
int weftc_endianness_big(const weftc_ctx_t *ctx);

/* Multi-target code preview for the LAST compile. Writes a
 * NUL-terminated preview into out; EBOUNDS when cap is too small. */
int weftc_codegen(const weftc_ctx_t *ctx, int lang, char *out,
                  size_t cap, size_t *out_len);

/* ------------------------------------------------------------------ */
/* weft-lsp headless engine (JSON-RPC 2.0 over memory buffers)          */
/* ------------------------------------------------------------------ */

typedef struct weft_lsp_ctx_t weft_lsp_ctx_t;  /* opaque; size via weft_lsp_ctx_size() */

size_t weft_lsp_ctx_size(void);
int    weft_lsp_ctx_init(void *mem, size_t size, weft_lsp_ctx_t **out);

/* Transport seam: the engine never touches stdio itself — the host (or
 * the wasm32 shim) pumps bytes through these callbacks. read_fn returns
 * 0 at EOF; write_fn returns bytes written. serve_stdio blocks until
 * read_fn EOF or the exit notification, applying Content-Length framing. */
typedef long (*weft_lsp_read_fn)(void *user, char *buf, unsigned long len);
typedef long (*weft_lsp_write_fn)(void *user, const char *buf,
                                  unsigned long len);
int weft_lsp_serve(weft_lsp_ctx_t *ctx,
                   weft_lsp_read_fn read_fn, void *read_user,
                   weft_lsp_write_fn write_fn, void *write_user);

/* One JSON-RPC message in, up to two messages out:
 *   resp: the response (requests only), empty (*resp_len == 0) otherwise
 *   notif: server notifications (publishDiagnostics), empty when none
 * Responses/notifications are complete JSON-RPC messages WITHOUT
 * Content-Length framing. Return: engine refusal ladder + EPARSE for
 * malformed JSON-RPC (an error RESPONSE is still emitted in that case). */
int weft_lsp_handle(weft_lsp_ctx_t *ctx, const char *req, size_t req_len,
                    char *resp, size_t resp_cap, size_t *resp_len,
                    char *notif, size_t notif_cap, size_t *notif_len);

int weft_lsp_exited(const weft_lsp_ctx_t *ctx);   /* 1 after exit        */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFT_STUDIO_H */
