/*
 * weft_sbe.c - Weft Pillar 6: SBE (Simple Binary Encoding) direct-to-
 *              Weft schema-driven transcoder.
 *
 * Law 1 (zero-allocation): the decoder walks the wire buffer with
 *   bounded arithmetic only; groups and var fields stay ZERO-COPY
 *   (pointers into the source); fixed scalars are transcoded into the
 *   caller's weft_sbe_view_t (stack/storage-provided).
 * Law 2: the Weft-MD projection emits frozen 64-byte weft_itch_msg_t
 *   add-order-shaped messages (offsets pinned in weft_adapters.h).
 * Law 3: LE wire loads via memcpy + __builtin_bswap* on BE hosts only.
 *
 * Frozen policy decisions (D-61 section 2):
 *   - SBE message header: block_length u16, template_id u16,
 *     schema_id u16, version u16 - all little-endian (SBE default).
 *   - Group dimension encoding pinned to u8 block_length + u8
 *     num_in_group (a legal SBE dimension encoding; schemas using
 *     other encodings transcode at the seam).
 *   - Var-field length prefix pinned to u8.
 *   - template/schema/version/block_length must match the schema
 *     EXACTLY (frozen v0 policy; forward-compat block growth is a
 *     declared follow-up).
 */
#include "weft_adapters.h"

#include <string.h>

/* ================================================================== */
/* Little-endian wire loaders (SBE default encoding)                  */
/* ================================================================== */

static inline uint16_t weft_load_le16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return __builtin_bswap16(v);
#else
    return v;
#endif
}

static inline uint32_t weft_load_le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return __builtin_bswap32(v);
#else
    return v;
#endif
}

static inline uint64_t weft_load_le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return __builtin_bswap64(v);
#else
    return v;
#endif
}

/* Decode one scalar by kind at p; signed kinds sign-extend into the
 * u64 slot (callers cast back according to scalar_kind[]). */
static int32_t weft_sbe_read_scalar(const uint8_t *p, uint16_t size,
                                    uint16_t kind, uint64_t *out)
{
    switch (kind) {
    case WEFT_SBE_K_U8:
        if (size != 1u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = (uint64_t)p[0];
        break;
    case WEFT_SBE_K_I8:
        if (size != 1u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = (uint64_t)(int64_t)(int8_t)p[0];
        break;
    case WEFT_SBE_K_U16:
        if (size != 2u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = (uint64_t)weft_load_le16(p);
        break;
    case WEFT_SBE_K_I16:
        if (size != 2u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = (uint64_t)(int64_t)(int16_t)weft_load_le16(p);
        break;
    case WEFT_SBE_K_U32:
        if (size != 4u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = (uint64_t)weft_load_le32(p);
        break;
    case WEFT_SBE_K_I32:
        if (size != 4u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = (uint64_t)(int64_t)(int32_t)weft_load_le32(p);
        break;
    case WEFT_SBE_K_U64:
        if (size != 8u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = weft_load_le64(p);
        break;
    case WEFT_SBE_K_I64:
        if (size != 8u) { return WEFT_ADAPTER_ESCHEMA; }
        *out = weft_load_le64(p);
        break;
    default:
        return WEFT_ADAPTER_ESCHEMA; /* CHARS is not a scalar */
    }
    return WEFT_ADAPTER_OK;
}

/* ================================================================== */
/* Public decode API                                                  */
/* ================================================================== */

int32_t weft_sbe_decode_header(const uint8_t *buf, size_t len,
                               uint16_t *block_length, uint16_t *template_id,
                               uint16_t *schema_id, uint16_t *version)
{
    if (!buf || !block_length || !template_id || !schema_id || !version) {
        return WEFT_ADAPTER_EINVAL;
    }
    if (len < 8u) { return WEFT_ADAPTER_ETRUNC; }
    *block_length = weft_load_le16(buf);
    *template_id  = weft_load_le16(buf + 2);
    *schema_id    = weft_load_le16(buf + 4);
    *version      = weft_load_le16(buf + 6);
    return WEFT_ADAPTER_OK;
}

int32_t weft_sbe_decode(const uint8_t *buf, size_t len,
                        const weft_sbe_schema_t *s,
                        weft_sbe_view_t *v)
{
    size_t pos = 8u;
    uint16_t i, g, k;

    if (!buf || !s || !v) { return WEFT_ADAPTER_EINVAL; }

    memset(v, 0, sizeof *v);

    /* ---- schema structural validation (capacity + block bounds) --- */
    if (s->num_fixed > WEFT_SBE_MAX_FIXED ||
        s->num_groups > WEFT_SBE_MAX_GROUPS ||
        s->num_var    > WEFT_SBE_MAX_VAR) {
        v->status = WEFT_ADAPTER_ESCHEMA;
        return WEFT_ADAPTER_ESCHEMA;
    }
    for (i = 0; i < s->num_fixed; ++i) {
        const weft_sbe_field_desc_t *f = &s->fixed[i];
        if ((size_t)f->offset + (size_t)f->size > (size_t)s->block_length ||
            f->slot >= WEFT_SBE_MAX_FIXED) {
            v->status = WEFT_ADAPTER_ESCHEMA;
            return WEFT_ADAPTER_ESCHEMA;
        }
    }
    for (g = 0; g < s->num_groups; ++g) {
        const weft_sbe_group_desc_t *gd = &s->groups[g];
        if (gd->num_fields > WEFT_SBE_MAX_GROUP_FIELDS) {
            v->status = WEFT_ADAPTER_ESCHEMA;
            return WEFT_ADAPTER_ESCHEMA;
        }
        for (i = 0; i < gd->num_fields; ++i) {
            const weft_sbe_field_desc_t *f =
                &s->group_fields[gd->first_field + i];
            if ((size_t)f->offset + (size_t)f->size > (size_t)gd->block_length ||
                f->slot != 0u) { /* slot reserved 0 for group fields */
                v->status = WEFT_ADAPTER_ESCHEMA;
                return WEFT_ADAPTER_ESCHEMA;
            }
        }
    }

    /* ---- message header ---- */
    if (len < 8u) {
        v->status = WEFT_ADAPTER_ETRUNC;
        return WEFT_ADAPTER_ETRUNC;
    }
    v->block_length = weft_load_le16(buf);
    v->template_id  = weft_load_le16(buf + 2);
    v->schema_id    = weft_load_le16(buf + 4);
    v->version      = weft_load_le16(buf + 6);

    if (v->template_id != s->template_id ||
        v->schema_id   != s->schema_id   ||
        v->version     != s->version     ||
        v->block_length != s->block_length) {
        v->status = WEFT_ADAPTER_ESCHEMA;
        return WEFT_ADAPTER_ESCHEMA;
    }
    if (len - pos < (size_t)s->block_length) {
        v->status = WEFT_ADAPTER_ETRUNC;
        return WEFT_ADAPTER_ETRUNC;
    }

    /* ---- fixed block scalars ---- */
    for (i = 0; i < s->num_fixed; ++i) {
        const weft_sbe_field_desc_t *f = &s->fixed[i];
        uint64_t val = 0;
        int32_t st = weft_sbe_read_scalar(buf + pos + f->offset,
                                          f->size, f->kind, &val);
        if (st != WEFT_ADAPTER_OK) {
            v->status = st;
            return st;
        }
        v->scalar[f->slot]      = val;
        v->scalar_kind[f->slot] = f->kind;
    }
    v->num_fixed = s->num_fixed;
    pos += (size_t)s->block_length;
    v->schema = s;

    /* ---- repeating groups (dimension: u8 block_length + u8 num) ---- */
    for (g = 0; g < s->num_groups; ++g) {
        uint8_t gblk, gnum;
        size_t bytes;
        if (len - pos < 2u) {
            v->status = WEFT_ADAPTER_ETRUNC;
            return WEFT_ADAPTER_ETRUNC;
        }
        gblk = buf[pos];
        gnum = buf[pos + 1];
        pos += 2u;
        if ((uint16_t)gblk != s->groups[g].block_length) {
            v->status = WEFT_ADAPTER_EBADMSG;
            return WEFT_ADAPTER_EBADMSG;
        }
        /* u8 num_in_group (<= 255) always fits the view capacity
         * (WEFT_SBE_MAX_GROUP_ENTRIES = 4096); EBOUNDS stays
         * enforced by the accessors and the projection. */
        bytes = (size_t)gnum * (size_t)gblk;
        if (len - pos < bytes) {
            v->status = WEFT_ADAPTER_ETRUNC;
            return WEFT_ADAPTER_ETRUNC;
        }
        v->group[g].entries      = buf + pos;
        v->group[g].block_length = s->groups[g].block_length;
        v->group[g].count        = gnum;
        pos += bytes;
    }
    v->num_groups = s->num_groups;

    /* ---- trailing var-length fields (u8 length + bytes) ---- */
    for (k = 0; k < s->num_var; ++k) {
        uint8_t vlen;
        if (len - pos < 1u) {
            v->status = WEFT_ADAPTER_ETRUNC;
            return WEFT_ADAPTER_ETRUNC;
        }
        vlen = buf[pos];
        pos += 1u;
        if (len - pos < (size_t)vlen) {
            v->status = WEFT_ADAPTER_ETRUNC;
            return WEFT_ADAPTER_ETRUNC;
        }
        v->var[k].data   = buf + pos;
        v->var[k].length = vlen;
        pos += (size_t)vlen;
    }
    v->num_var = s->num_var;

    v->total_size = (uint32_t)pos;
    v->status     = WEFT_ADAPTER_OK;
    return WEFT_ADAPTER_OK;
}

int32_t weft_sbe_group_entry(const weft_sbe_view_t *v,
                             uint32_t group_idx, uint32_t entry_idx,
                             uint32_t field_idx, uint64_t *value_out)
{
    const weft_sbe_group_desc_t *gd;
    const weft_sbe_field_desc_t *f;
    const uint8_t *p;

    if (!v || !value_out || v->status != WEFT_ADAPTER_OK || !v->schema) {
        return WEFT_ADAPTER_EINVAL;
    }
    if (group_idx >= (uint32_t)v->num_groups) { return WEFT_ADAPTER_EBOUNDS; }
    if (entry_idx >= (uint32_t)v->group[group_idx].count) {
        return WEFT_ADAPTER_EBOUNDS;
    }
    gd = &v->schema->groups[group_idx];
    if (field_idx >= (uint32_t)gd->num_fields) { return WEFT_ADAPTER_EBOUNDS; }

    f = &v->schema->group_fields[gd->first_field + field_idx];
    if ((size_t)f->offset + (size_t)f->size > (size_t)gd->block_length) {
        return WEFT_ADAPTER_ESCHEMA;
    }
    p = v->group[group_idx].entries +
        (size_t)entry_idx * (size_t)v->group[group_idx].block_length +
        f->offset;
    return weft_sbe_read_scalar(p, f->size, f->kind, value_out);
}

int32_t weft_sbe_group_bytes(const weft_sbe_view_t *v,
                             uint32_t group_idx, uint32_t entry_idx,
                             uint32_t field_idx,
                             const uint8_t **data_out, uint16_t *len_out)
{
    const weft_sbe_group_desc_t *gd;
    const weft_sbe_field_desc_t *f;
    const uint8_t *p;

    if (!v || !data_out || !len_out || v->status != WEFT_ADAPTER_OK || !v->schema) {
        return WEFT_ADAPTER_EINVAL;
    }
    if (group_idx >= (uint32_t)v->num_groups) { return WEFT_ADAPTER_EBOUNDS; }
    if (entry_idx >= (uint32_t)v->group[group_idx].count) {
        return WEFT_ADAPTER_EBOUNDS;
    }
    gd = &v->schema->groups[group_idx];
    if (field_idx >= (uint32_t)gd->num_fields) { return WEFT_ADAPTER_EBOUNDS; }

    f = &v->schema->group_fields[gd->first_field + field_idx];
    if (f->kind != (uint16_t)WEFT_SBE_K_CHARS) {
        return WEFT_ADAPTER_ESCHEMA;
    }
    if ((size_t)f->offset + (size_t)f->size > (size_t)gd->block_length) {
        return WEFT_ADAPTER_ESCHEMA;
    }
    p = v->group[group_idx].entries +
        (size_t)entry_idx * (size_t)v->group[group_idx].block_length +
        f->offset;
    *data_out = p;
    *len_out  = f->size;
    return WEFT_ADAPTER_OK;
}

/* ================================================================== */
/* Canonical "Weft Market Data" template (frozen)                     */
/* ================================================================== */

static const weft_sbe_field_desc_t g_md_fixed[] = {
    {  0, 8, (uint16_t)WEFT_SBE_K_U64, 0 }, /* transact_time_ns */
    {  8, 4, (uint16_t)WEFT_SBE_K_U32, 1 }, /* security_id       */
    { 12, 4, (uint16_t)WEFT_SBE_K_U32, 2 }, /* seq_num           */
    { 16, 1, (uint16_t)WEFT_SBE_K_U8,  3 }  /* md_flags          */
};

static const weft_sbe_field_desc_t g_md_group_fields[] = {
    {  0, 8, (uint16_t)WEFT_SBE_K_U64, 0 }, /* price_raw */
    {  8, 8, (uint16_t)WEFT_SBE_K_U64, 0 }, /* order_ref */
    { 16, 4, (uint16_t)WEFT_SBE_K_U32, 0 }, /* shares    */
    { 20, 1, (uint16_t)WEFT_SBE_K_U8,  0 }, /* buy_sell  */
    { 21, 1, (uint16_t)WEFT_SBE_K_U8,  0 }  /* action    */
};

static const weft_sbe_group_desc_t g_md_groups[] = {
    { 32, 5, 0, 0 } /* order_entries: 32B/entry, 5 fields, fields@0 */
};

static const weft_sbe_schema_t g_md_schema = {
    1u,        /* template_id  */
    0x5746u,   /* schema_id "WF" */
    0u,        /* version      */
    24u,       /* block_length */
    4u,        /* num_fixed    */
    1u,        /* num_groups   */
    1u,        /* num_var      */
    0u,        /* _pad0        */
    g_md_fixed,
    g_md_groups,
    g_md_group_fields
};

const weft_sbe_schema_t *weft_sbe_schema_md(void)
{
    return &g_md_schema;
}

/* ================================================================== */
/* DIRECT-TO-WEFT PROJECTION: canonical MD -> 64B add-order messages  */
/* ================================================================== */

size_t weft_sbe_md_to_itch_add(const weft_sbe_view_t *view,
                               weft_itch_msg_t *WEFT_RESTRICT out,
                               size_t out_max, int32_t *status)
{
    size_t i, limit, n;
    const uint8_t *sym = NULL;
    uint16_t symlen = 0;
    uint64_t ts, secid;

    if (status) { *status = WEFT_ADAPTER_OK; }

    if (!view || !out || out_max == 0u) {
        if (status) { *status = WEFT_ADAPTER_EINVAL; }
        return 0;
    }
    if (((uintptr_t)out) & 63u) {
        if (status) { *status = WEFT_ADAPTER_EALIGN; }
        return 0;
    }
    if (view->status != WEFT_ADAPTER_OK || !view->schema ||
        view->schema != weft_sbe_schema_md()) {
        if (status) { *status = WEFT_ADAPTER_ESCHEMA; }
        return 0;
    }

    /* var[0] = symbol (space-padded to 8 in the projection) */
    if (view->num_var > 0) {
        sym    = view->var[0].data;
        symlen = view->var[0].length;
    }
    if (symlen > 8u) {
        if (status) { *status = WEFT_ADAPTER_EBOUNDS; }
        return 0;
    }

    ts    = view->scalar[0];
    secid = view->scalar[1];
    if (ts >= WEFT_ITCH_NS_PER_DAY) {
        if (status) { *status = WEFT_ADAPTER_EBADMSG; }
        return 0;
    }

    n     = (size_t)view->group[0].count;
    limit = (n < out_max) ? n : out_max; /* clean stop at capacity */

    for (i = 0; i < limit; ++i) {
        uint64_t price = 0, ref = 0, shares = 0, side = 0;
        weft_itch_msg_t *m = &out[i];

        if (weft_sbe_group_entry(view, 0u, (uint32_t)i, 0u, &price) != WEFT_ADAPTER_OK ||
            weft_sbe_group_entry(view, 0u, (uint32_t)i, 1u, &ref)   != WEFT_ADAPTER_OK ||
            weft_sbe_group_entry(view, 0u, (uint32_t)i, 2u, &shares)!= WEFT_ADAPTER_OK ||
            weft_sbe_group_entry(view, 0u, (uint32_t)i, 3u, &side)  != WEFT_ADAPTER_OK) {
            if (status) { *status = WEFT_ADAPTER_ESCHEMA; }
            return i;
        }
        if (price > 0xFFFFFFFFull || ref == 0u || shares == 0u ||
            shares > 0xFFFFFFFFull ||
            (side != (uint64_t)'B' && side != (uint64_t)'S')) {
            if (status) { *status = WEFT_ADAPTER_EBADMSG; }
            return i;
        }

        memset(m, 0, sizeof *m);           /* deterministic pad lanes */
        m->hdr.msg_type        = (uint8_t)WEFT_ITCH_TAG_ADD_ORDER;
        m->hdr.stock_locate    = (uint16_t)(secid & 0xFFFFu);
        m->hdr.tracking_number = 0u;
        m->hdr.timestamp_ns    = ts;
        m->u.add.order_ref     = ref;
        m->u.add.buy_sell      = (uint8_t)side;
        m->u.add.shares        = (uint32_t)shares;
        m->u.add.price_raw     = (uint32_t)price;
        memset(m->u.add.stock, (uint8_t)' ', 8); /* ITCH space padding */
        if (sym && symlen > 0u) {
            memcpy(m->u.add.stock, sym, (size_t)symlen);
        }
    }

    return limit;
}
