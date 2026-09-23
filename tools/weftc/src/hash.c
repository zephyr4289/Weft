/* hash.c — WH64 and the canonical manifests (RFC-0017 §5).
 *
 * The schema fingerprint is the zero-cost handshake: two processes (or a
 * C library and a TS DataView reader) exchange one u64 and know their
 * memory views are byte-compatible. Requirements, in priority order:
 *
 *   1. DETERMINISTIC — pure integer math, explicit little-endian loads,
 *      no host state, no UB (C11 uint64_t arithmetic is exact on every
 *      conforming host, 32- or 64-bit). Same bytes in, same u64 out.
 *   2. MUTATION-SENSITIVE — any semantic change to a schema (field type,
 *      offset, size, order, endianness, names for schema_id) flips bits
 *      across the whole word (avalanche, tested).
 *   3. DOMAINS SEPARATED — whole-schema ABI, whole-schema ID, and
 *      per-decl manifests all start with distinct magic bytes, so a hash
 *      of one can never equal a hash of another by construction.
 *
 * WH64 is a keyed-free wyhash-style mixer: 8-byte LE chunks folded with
 * add + splitmix-finalizer avalanche; tail bytes folded LE; length mixed
 * at both ends. FNV-1a-64 is carried alongside as an INDEPENDENT second
 * fingerprint — a cross-check that catches implementation bugs in either
 * producer, not collisions.
 *
 * Manifest encodings (all LE):
 *   abi  manifest  "WAB1" — structural identity of the whole schema
 *   id   manifest  "WID1" — abi manifest + names (semantic source identity)
 *   decl manifest  "WDC1" — structural identity of ONE decl (the number a
 *                          wire handshake actually exchanges)
 *
 * The abi manifest is name-free for the hashed decl's own fields/variants
 * (renaming a field keeps wire compatibility) but keeps referenced TYPE
 * names inside type strings — renaming a referenced struct is a schema
 * change worth flagging. The id manifest adds every name.
 *
 * STABILITY CONTRACT: these encodings are frozen at IR version 1. Any
 * change to WH64 or to a manifest encoding bumps WEFTC_IR_VERSION and
 * invalidates every published hash — by design, loudly.
 */
#include "weftc.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* WH64                                                                 */
/* ------------------------------------------------------------------ */
static uint64_t mix64(uint64_t z)
{
    z ^= z >> 30; z *= 0xbf58476d1ce4e5b9ull;
    z ^= z >> 27; z *= 0x94d049bb133111ebull;
    z ^= z >> 31;
    return z;
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

uint64_t wh64(const uint8_t *data, size_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ mix64((uint64_t)len);
    size_t i = 0;
    for (; len - i >= 8; i += 8)
        h = mix64(h + le64(data + i) + 0x165667B19E3779F9ull);
    if (i < len) { /* tail: 1..7 bytes, little-endian fold */
        uint64_t t = 0;
        for (size_t j = i; j < len; j++)
            t |= (uint64_t)data[j] << (8 * (j - i));
        h = mix64(h ^ (t + 0xC2B2AE3D27D4EB4Full));
    }
    return mix64(h);
}

uint64_t fnv1a64(const uint8_t *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Canonical manifests                                                  */
/* ------------------------------------------------------------------ */
/* Kind codes are part of the frozen encoding. */
#define DECL_KIND_STRUCT    0u
#define DECL_KIND_ENUM      1u
#define DECL_KIND_BITFLAGS  2u

static uint8_t decl_kind_code(DeclKind k)
{
    return k == DECL_STRUCT ? DECL_KIND_STRUCT
         : k == DECL_ENUM ? DECL_KIND_ENUM : DECL_KIND_BITFLAGS;
}

static uint8_t prim_code(PrimKind k)
{
    /* PRIM_U8 == 1 .. PRIM_BOOL == 12: the enum order is the encoding */
    return (uint8_t)k;
}

/* Struct flags byte: bit0 packed, bit1 reordered (ABI view — both are
 * layout/codegen-meaningful), bit2 has_align, bit3 has_simd (ID view
 * only: declared intent). Attr VALUES ride after align when the bit is
 * set, in the ID manifest only — the ABI manifest records EFFECTS
 * (sizes/aligns/offsets), so a no-op @align(1) never changes a wire
 * hash (RFC-0017 §5.1). */
static uint8_t struct_flags(const DeclLayout *dl, int with_attrs)
{
    uint8_t f = 0;
    if (dl->packed) f |= 1u << 0;
    if (dl->reordered) f |= 1u << 1;
    if (with_attrs && dl->has_align_attr) f |= 1u << 2;
    if (with_attrs && dl->has_simd_attr) f |= 1u << 3;
    return f;
}

/* One decl's structural record. `with_names`/`with_attrs` select the
 * manifest flavor: the ABI manifest is effect-only (name-free for the
 * hashed decl's own fields/variants, no attribute spellings); the ID
 * manifest adds every name and the declared attributes. */
static void emit_decl_record(const WeftUnit *u, size_t i, ByteBuf *b,
                             int with_names, int with_attrs)
{
    const DeclLayout *dl = &u->layouts[i];
    bb_u8(b, decl_kind_code(dl->kind));
    if (dl->kind == DECL_STRUCT) {
        bb_u8(b, struct_flags(dl, with_attrs));
        bb_u64le(b, dl->size);
        bb_u64le(b, dl->align);
        if (with_attrs && dl->has_align_attr) bb_u64le(b, dl->align_attr);
        if (with_attrs && dl->has_simd_attr) bb_u64le(b, dl->simd_attr);
        bb_u32le(b, (uint32_t)dl->nfields);
        for (size_t f = 0; f < dl->nfields; f++) {
            const FieldLayout *fl = &dl->fields[f];
            if (with_names) bb_str(b, fl->name);
            bb_u64le(b, fl->offset);
            bb_u64le(b, fl->size);
            bb_u64le(b, fl->align);
            bb_str(b, fl->type_str);
            if (dl->reordered) bb_u32le(b, fl->orig_index);
        }
        bb_u32le(b, (uint32_t)dl->nholes);
        for (size_t h = 0; h < dl->nholes; h++) {
            bb_u64le(b, dl->holes[h].offset);
            bb_u64le(b, dl->holes[h].size);
        }
    } else {
        bb_u8(b, prim_code(dl->backing));
        bb_u64le(b, dl->size);
        bb_u64le(b, dl->align);
        if (with_attrs && dl->has_align_attr) bb_u64le(b, dl->align_attr);
        bb_u32le(b, (uint32_t)dl->nvariants);
        for (size_t v = 0; v < dl->nvariants; v++) {
            if (with_names) bb_str(b, dl->variants[v].name);
            bb_i64le(b, dl->variants[v].value);
        }
    }
}

static void emit_header(ByteBuf *b, const char magic[4], const WeftUnit *u,
                        uint32_t ndecls)
{
    bb_bytes(b, magic, 4);
    bb_u8(b, WEFTC_IR_VERSION);
    bb_u8(b, u->endian_big ? 1 : 0);
    bb_u32le(b, ndecls);
}

void weft_build_abi_manifest(const WeftUnit *u, ByteBuf *out)
{
    emit_header(out, "WAB1", u, (uint32_t)u->ndecls);
    for (size_t i = 0; i < u->ndecls; i++)
        emit_decl_record(u, i, out, 0, 0);
}

void weft_build_id_manifest(const WeftUnit *u, ByteBuf *out)
{
    emit_header(out, "WID1", u, (uint32_t)u->ndecls);
    for (size_t i = 0; i < u->ndecls; i++) {
        const DeclLayout *dl = &u->layouts[i];
        bb_str(out, dl->name); /* the "id" addition: names */
        emit_decl_record(u, i, out, 1, 1);
    }
}

/* Per-decl structural manifest — the number a wire handshake exchanges
 * for one message type ("is your `Frame` my `Frame`?"). Effect-only,
 * like the whole-schema ABI manifest. */
static void build_decl_manifest(const WeftUnit *u, size_t i, ByteBuf *out)
{
    emit_header(out, "WDC1", u, 1);
    emit_decl_record(u, i, out, 0, 0);
}

/* ------------------------------------------------------------------ */
/* weft_hash_all                                                        */
/* ------------------------------------------------------------------ */
void weft_hash_all(WeftUnit *u)
{
    if (!u->has_layout) return;

    ByteBuf abi, id;
    bb_init(&abi, u->ar);
    bb_init(&id, u->ar);
    weft_build_abi_manifest(u, &abi);
    weft_build_id_manifest(u, &id);

    for (size_t i = 0; i < u->ndecls; i++) {
        ByteBuf one;
        bb_init(&one, u->ar);
        build_decl_manifest(u, i, &one);
        u->layouts[i].abi_hash = wh64(one.data, one.len);
    }

    u->abi_hash   = wh64(abi.data, abi.len);
    u->schema_id  = wh64(id.data, id.len);
    u->fnv_debug  = fnv1a64(abi.data, abi.len);
}
