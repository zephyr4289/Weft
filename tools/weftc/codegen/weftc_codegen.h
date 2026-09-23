// weftc_codegen.h — shared infrastructure for the weftc native codegen backends
// (Pillar 1: C11 / Rust / WGSL / GLSL emitters, targets of Project weftc).
//
// Owns everything the four backends share:
//   §1  arena + strbuf      — bump allocation + growable text buffer
//   §2  JSON                — strict subset parser (objects, arrays, strings with
//                             escapes, integers, doubles, bool, null) with
//                             line/column error reporting
//   §3  IR model            — weft-ir JSON v1 (the contract Engineer 1's compiler
//                             core emits into; see README §IR)
//   §4  type table          — canonical C layout (size/align) per type
//   §5  verification        — layout laws: offsets, alignment, size, ordering,
//                             schema header, bitfields, identifiers, cycles
//   §6  naming              — C/Rust identifier derivation + cross-language
//                             keyword rejection
//   §7  schema id           — canonical layout signature + FNV-1a-64 (auto mode)
//
// The four Laws of the mission briefing constrain the GENERATED code; this
// file is the engine that enforces them before a single byte is emitted:
//   Law 1 zero dynamic allocation on the read path   -> emitters may only
//          project memory (casts/unaligned reads); no malloc anywhere in output
//   Law 2 strict no_std / Pure C11                   -> generated C needs only
//          <stdint.h> <stdbool.h> <stddef.h> <stdalign.h> <assert.h>;
//          generated Rust needs only `core`
//   Law 3 unaligned access safety                    -> every generated cast
//          validates alignment; packed readers are byte-assembly (C) or
//          read_unaligned (Rust) — no UB on any architecture
//   Law 4 deterministic GPU layouts                  -> WGSL/GLSL emitters emit
//          explicit padding and REFUSE (with a precise reason) any layout they
//          cannot reproduce byte-for-byte; refusal is total, never partial
//
// The generator itself is plain C11 + libc only. It is deterministic: two runs
// over the same IR produce byte-identical output (gated by tests/run_tests.sh).

#ifndef WEFTC_CODEGEN_H
#define WEFTC_CODEGEN_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEFTC_CODEGEN_VERSION "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// §1 arena + strbuf
// ---------------------------------------------------------------------------

typedef struct weft_arena_chunk weft_arena_chunk;
struct weft_arena_chunk {
    weft_arena_chunk* next;
    size_t used;
    size_t cap;
    // data follows
};

typedef struct {
    weft_arena_chunk* head;
    size_t total;
} weft_arena;

void* weft_arena_alloc(weft_arena* a, size_t n, size_t align);
char* weft_arena_strdup(weft_arena* a, const char* s);
char* weft_arena_strndup(weft_arena* a, const char* s, size_t n); // always NUL-terminates

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} strbuf;

void   wsb_init(strbuf* b);
void   wsb_free(strbuf* b);
void   wsb_putc(strbuf* b, char c);
void   wsb_puts(strbuf* b, const char* s);
void   wsb_write(strbuf* b, const char* s, size_t n);
void   wsb_printf(strbuf* b, const char* fmt, ...);
size_t wsb_len(const strbuf* b);

// ---------------------------------------------------------------------------
// §2 JSON (strict subset: the weft-ir exchange format)
// ---------------------------------------------------------------------------

typedef enum {
    JV_NULL,
    JV_BOOL,
    JV_INT,   // integer that fits int64_t
    JV_NUM,   // everything else numeric (double)
    JV_STR,
    JV_ARR,
    JV_OBJ
} jv_kind;

typedef struct jv jv;
struct jv {
    jv_kind kind;
    bool boolean;
    int64_t i;
    double num;
    const char* str;      // JV_STR (arena-owned, NUL-terminated, UTF-8)
    jv** items;           // JV_ARR
    uint32_t nitems;
    const char** keys;    // JV_OBJ
    jv** vals;            // JV_OBJ
    uint32_t nkeys;
};

// Parses `text` (len bytes). On failure returns NULL and fills err/line/col.
jv* jv_parse(weft_arena* a, const char* text, size_t len,
             char err[256], unsigned* line, unsigned* col);

// Object access. jv_get returns NULL when the key is absent.
jv*         jv_get(const jv* obj, const char* key);
const char* jv_get_str(const jv* obj, const char* key, const char* dflt);
// Strict integer getters: return false when the key is absent or not an
// integer in [min, max]. Used by the IR loader so every IR error is precise.
bool jv_get_u32(const jv* obj, const char* key, uint32_t min, uint32_t max, uint32_t* out);
bool jv_get_bool(const jv* obj, const char* key, bool dflt, bool* out);

// ---------------------------------------------------------------------------
// §3 IR model — weft-ir JSON v1
// ---------------------------------------------------------------------------

typedef enum {
    WT_U8, WT_I8, WT_U16, WT_I16, WT_U32, WT_I32, WT_U64, WT_I64,
    WT_F16, WT_F32, WT_F64, WT_BOOL,
    WT_VEC2F32, WT_VEC3F32, WT_VEC4F32,
    WT_VEC2F16, WT_VEC3F16, WT_VEC4F16,
    WT_ARRAY,   // element kind in elem_kind (never nested arrays in v1)
    WT_STRUCT   // struct_ref / struct_type
} weft_type_kind;

typedef struct {
    const char* name;
    uint32_t lo, hi;      // inclusive bit range within the parent field
} weft_bits;

typedef struct weft_field weft_field;
struct weft_field {
    const char* name;
    weft_type_kind kind;
    weft_type_kind elem_kind;        // WT_ARRAY only
    const char* struct_ref;          // WT_STRUCT only (IR name)
    struct weft_struct* struct_type; // resolved pointer
    uint32_t count;                  // WT_ARRAY only
    uint32_t offset;                 // declared (from Eng 1's layout engine)
    uint32_t size;                   // computed, C semantics
    uint32_t align;                  // computed, C semantics
    const char* gpu_type;            // optional GPU override, e.g. "mat4x4<f32>"
    const char* doc;
    weft_bits* bits;                 // bitfield subfields (integer fields only)
    uint32_t nbits;
};

typedef struct weft_struct weft_struct;
struct weft_struct {
    const char* name;        // snake_case IR name (canonical identity)
    const char* c_name;      // derived "weft_<name>_t" or override
    const char* rust_name;   // derived CamelCase or override
    uint32_t align;          // declared struct alignment (>= natural member align)
    uint32_t size;           // declared total size (verified == roundUp(align, end))
    bool repr_align;         // true when align > natural (emits alignas/#[repr(align)])
    bool root;               // schema-headered, castable type (default true)
    uint32_t schema_width;   // 64 or 32 (root structs only)
    uint64_t schema_id;      // explicit or FNV-1a-64 of the canonical signature
    char schema_id_str[24];  // "0x%016llX" / "0x%08X" form for banners
    bool schema_auto;        // true when computed (non-cryptographic — see README)
    weft_field* fields;
    uint32_t nfields;
    const char* doc;
};

typedef struct {
    const char* module;      // module identity (comments + signature)
    weft_struct* ss;         // topologically sorted: dependencies first
    uint32_t nstructs;
    weft_arena* arena;       // backing arena (owned)
} weft_ir;

// Loads + verifies an IR file. Returns NULL on any error; `err` carries a
// single-line, context-rich message (path:line:col where available).
weft_ir* weft_ir_load(const char* path, char err[512]);

// ---------------------------------------------------------------------------
// §4 type table — canonical C layout
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t size;
    uint32_t align;
} weft_layout;

weft_layout weft_type_layout(const weft_field* f);
// Canonical spelling used in the schema signature and docs
// ("u16", "vec3<f32>", "[u16; 4]", struct name, ...).
void weft_type_canonical(const weft_field* f, char out[128]);

// ---------------------------------------------------------------------------
// §6 naming
// ---------------------------------------------------------------------------

// Fills c_name/rust_name defaults when absent; validates overrides.
// Returns false + message in err for invalid/reserved names.
bool weft_names_derive(weft_arena* a, weft_struct* s, char err[256]);
bool weft_ident_ok(const char* s);          // valid C identifier + not reserved
bool weft_snake_ok(const char* s);          // ^[a-z][a-z0-9_]*$ + not reserved
void weft_camel(const char* snake, char out[80]);  // telemetry_frame -> TelemetryFrame
void weft_upper(const char* snake, char out[160]); // telemetry_frame -> TELEMETRY_FRAME

// ---------------------------------------------------------------------------
// §7 schema signature + FNV-1a-64 (auto mode)
// ---------------------------------------------------------------------------

void     weft_schema_signature(const weft_struct* s, char out[512]);
uint64_t weft_fnv1a64(const char* s);

// ---------------------------------------------------------------------------
// emitter interface (implemented by c/, rust/, gpu/)
// ---------------------------------------------------------------------------

typedef struct {
    const char* out_dir;    // directory that receives the files
    const char* core_name;  // shared core stem (default "weft_projection_core")
    bool rust_bytemuck;     // emit #[cfg(feature = "bytemuck")] impls
    bool rust_zerocopy;     // emit #[cfg(feature = "zerocopy")] impls
} weft_emit_opts;

int weft_emit_c(const weft_ir* ir, const weft_emit_opts* o);
int weft_emit_rust(const weft_ir* ir, const weft_emit_opts* o);
int weft_emit_wgsl(const weft_ir* ir, const weft_emit_opts* o);
int weft_emit_glsl(const weft_ir* ir, const weft_emit_opts* o);

// shared helpers for emitters
void weft_warn(const char* fmt, ...);                    // "weftc-codegen: warning: ..."
int  weft_mkdir_p(const char* path);                     // 0 ok, -1 fail
int  weft_write_file(const char* path, const strbuf* b); // 0 ok, -1 fail

// pretty hex for banners
void weft_hex64(uint64_t v, char out[24]); // "0x8F4C1120A9B30012"
void weft_hex32(uint32_t v, char out[16]); // "0x8F4C1120"

#ifdef __cplusplus
}
#endif

#endif // WEFTC_CODEGEN_H
