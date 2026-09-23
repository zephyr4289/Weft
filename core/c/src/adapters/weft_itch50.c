/*
 * weft_itch50.c - Weft Pillar 6: NASDAQ TotalView-ITCH 5.0 & OUCH 5.0
 *                 high-speed in-place parser + MoldUDP64 walker.
 *
 * Law 1 (zero-allocation): every routine below runs on caller-provided
 *   storage only; there is not a single allocation site in this file.
 * Law 2 (bit-exact packing): all writes land in the frozen 64-byte
 *   weft_itch_msg_t / weft_ouch_msg_t projections (offsets pinned by
 *   _Static_assert in weft_adapters.h).
 * Law 3 (portability): wire loads go through memcpy() + compiler
 *   builtins (__builtin_bswap16/32/64); no cast-dereferences of
 *   misaligned integers; no architecture-specific code in this file.
 *
 * Validation policy (fail-closed, both strict and lenient modes):
 *   - unknown message type                    -> EBADMSG
 *   - buffer shorter than oracle message size -> ETRUNC
 *   - timestamp >= 24h                        -> EBADMSG
 *   - enumerated chars (side/state/action/...)-> EBADMSG
 *   - symbol/space-padded fields non-printable-> EBADMSG
 *   - share quantities == 0 on volume msgs    -> EBADMSG
 *   - LENPREFIX framing size mismatch         -> EBADMSG (strict)
 * All validation happens on the WIRE bytes BEFORE the projection is
 * written, honoring the frozen contract: on any negative status the
 * output message is left untouched.
 */
#include "weft_adapters.h"

#include <string.h>

/* ================================================================== */
/* Big-endian wire loaders (memcpy-based, alignment-safe)             */
/* ================================================================== */

static inline uint16_t weft_load_be16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return v;
#else
    return __builtin_bswap16(v);
#endif
}

static inline uint32_t weft_load_be32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return v;
#else
    return __builtin_bswap32(v);
#endif
}

static inline uint64_t weft_load_be64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

/* 6-byte big-endian nanoseconds-since-midnight (ITCH/OUCH timestamp). */
static inline uint64_t weft_load_be48(const uint8_t *p)
{
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
           ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
           ((uint64_t)p[4] << 8)  |  (uint64_t)p[5];
}

/* ================================================================== */
/* Wire-size oracles (compile-time frozen tables)                     */
/* ================================================================== */

static const uint8_t g_itch_wire_size[256] = {
    ['S'] = 12, /* System Event           */
    ['R'] = 39, /* Stock Directory        */
    ['H'] = 25, /* Stock Trading Action   */
    ['Y'] = 20, /* Reg SHO                */
    ['L'] = 26, /* Market Participant Pos */
    ['V'] = 35, /* MWCB Decline Level     */
    ['W'] = 12, /* MWCB Status            */
    ['K'] = 28, /* IPO Quoting Period     */
    ['J'] = 47, /* LULD Auction Collar    */
    ['A'] = 36, /* Add Order              */
    ['F'] = 40, /* Add Order with MPID    */
    ['E'] = 31, /* Order Executed         */
    ['C'] = 36, /* Order Executed w/Price */
    ['X'] = 23, /* Order Cancel           */
    ['D'] = 19, /* Order Delete           */
    ['U'] = 35, /* Order Replace          */
    ['P'] = 44, /* Trade (non-cross)      */
    ['Q'] = 40, /* Cross Trade            */
    ['I'] = 50  /* NOII                   */
};

size_t weft_itch50_wire_size(uint8_t msg_type)
{
    return (size_t)g_itch_wire_size[msg_type];
}

static const uint8_t g_ouch_wire_size[256] = {
    ['S'] =  8, /* System Event            */
    ['A'] = 46, /* Order Accepted          */
    ['E'] = 27, /* Order Executed          */
    ['C'] = 32, /* Order Executed w/Price  */
    ['X'] = 19, /* Order Canceled          */
    ['U'] = 31  /* Order Replaced          */
};

size_t weft_ouch50_wire_size(uint8_t msg_type)
{
    return (size_t)g_ouch_wire_size[msg_type];
}

/* ================================================================== */
/* Field validators (operate on raw wire bytes, zero decode cost)     */
/* ================================================================== */

static inline int weft_v_side(uint8_t c)
{
    return c == (uint8_t)'B' || c == (uint8_t)'S';
}

static inline int weft_v_yn(uint8_t c)
{
    return c == (uint8_t)'Y' || c == (uint8_t)'N';
}

static inline int weft_v_event(uint8_t c)
{
    return c == (uint8_t)'O' || c == (uint8_t)'S' || c == (uint8_t)'Q' ||
           c == (uint8_t)'M' || c == (uint8_t)'E' || c == (uint8_t)'C';
}

static inline int weft_v_regsho(uint8_t c)
{
    return c >= (uint8_t)'0' && c <= (uint8_t)'2';
}

static inline int weft_v_trading_state(uint8_t c)
{
    return c == (uint8_t)'H' || c == (uint8_t)'P' ||
           c == (uint8_t)'Q' || c == (uint8_t)'T';
}

static inline int weft_v_mwcb_level(uint8_t c)
{
    return c >= (uint8_t)'1' && c <= (uint8_t)'3';
}

static inline int weft_v_cross_type(uint8_t c)
{
    return c == (uint8_t)'O' || c == (uint8_t)'C' ||
           c == (uint8_t)'H' || c == (uint8_t)'I';
}

static inline int weft_v_imbalance_dir(uint8_t c)
{
    return c == (uint8_t)'B' || c == (uint8_t)'S' ||
           c == (uint8_t)'N' || c == (uint8_t)'O';
}

static inline int weft_v_printable(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (p[i] < 0x20u || p[i] > 0x7Eu) { return 0; }
    }
    return 1;
}

static inline int weft_v_ts(const uint8_t *p)
{
    /* 6-byte BE timestamp must be < 24h (trading-day bound). */
    return weft_load_be48(p) < WEFT_ITCH_NS_PER_DAY;
}

/* ================================================================== */
/* ITCH 5.0 per-type decode core                                      */
/* ================================================================== */

/* Decode from msg start (type byte at m[0]); all validation precedes
 * any write into *o. `need` is the oracle wire size (pre-looked-up). */
static int32_t weft_itch_decode_core(const uint8_t *WEFT_RESTRICT m,
                                     size_t need,
                                     weft_itch_msg_t *WEFT_RESTRICT o)
{
    const uint8_t t = m[0];

    /* ---- validation pass (wire bytes only, no writes) ---- */
    if (!weft_v_ts(m + 5)) { return WEFT_ADAPTER_EBADMSG; }

    switch (t) {
    case 'S':
        if (!weft_v_event(m[11])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'R':
        if (!weft_v_printable(m + 11, 8)) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'H':
        if (!weft_v_printable(m + 11, 8) ||
            !weft_v_trading_state(m[19]) ||
            !weft_v_printable(m + 21, 4)) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'Y':
        if (!weft_v_printable(m + 11, 8) ||
            !weft_v_regsho(m[19])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'L':
        if (!weft_v_printable(m + 11, 4) ||
            !weft_v_printable(m + 15, 8) ||
            !weft_v_yn(m[23])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'V':
    case 'W':
        if (t == 'W' && !weft_v_mwcb_level(m[11])) {
            return WEFT_ADAPTER_EBADMSG;
        }
        break;
    case 'K':
        if (!weft_v_printable(m + 11, 8)) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'J':
        if (!weft_v_printable(m + 11, 8)) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'A':
    case 'F':
        if (!weft_v_side(m[19]) ||
            weft_load_be32(m + 20) == 0u ||
            !weft_v_printable(m + 24, 8)) { return WEFT_ADAPTER_EBADMSG; }
        if (t == 'F' && !weft_v_printable(m + 36, 4)) {
            return WEFT_ADAPTER_EBADMSG;
        }
        break;
    case 'E':
        if (weft_load_be32(m + 19) == 0u) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'C':
        if (weft_load_be32(m + 19) == 0u ||
            !weft_v_yn(m[31])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'X':
        if (weft_load_be32(m + 19) == 0u) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'D':
        break;
    case 'U':
        if (weft_load_be32(m + 27) == 0u) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'P':
        if (!weft_v_side(m[19]) ||
            weft_load_be32(m + 20) == 0u ||
            !weft_v_printable(m + 24, 8)) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'Q':
        if (weft_load_be64(m + 11) == 0u ||
            !weft_v_printable(m + 19, 8) ||
            !weft_v_cross_type(m[39])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'I':
        if (!weft_v_printable(m + 28, 8) ||
            !weft_v_imbalance_dir(m[27]) ||
            !weft_v_cross_type(m[48])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    default:
        return WEFT_ADAPTER_EBADMSG; /* unreachable: table gates unknown */
    }

    /* ---- projection pass (validated; writes pinned offsets) ---- */
    o->hdr.msg_type        = t;
    o->hdr.stock_locate    = weft_load_be16(m + 1);
    o->hdr.tracking_number = weft_load_be16(m + 3);
    o->hdr.timestamp_ns    = weft_load_be48(m + 5);

    switch (t) {
    case 'S':
        o->u.sys.event_code = m[11];
        break;
    case 'R':
        memcpy(o->u.dir.stock, m + 11, 8);
        o->u.dir.market_category     = m[19];
        o->u.dir.financial_status    = m[20];
        o->u.dir.round_lot_size      = weft_load_be32(m + 21);
        o->u.dir.round_lots_only     = m[25];
        o->u.dir.issue_classification= m[26];
        memcpy(o->u.dir.issue_subtype, m + 27, 2);
        o->u.dir.authenticity        = m[29];
        o->u.dir.short_sale_threshold= m[30];
        o->u.dir.ipo_flag            = m[31];
        o->u.dir.luld_tier           = m[32];
        o->u.dir.etp_flag            = m[33];
        o->u.dir.etp_leverage_factor = weft_load_be32(m + 34);
        o->u.dir.inverse_indicator   = m[38];
        break;
    case 'H':
        memcpy(o->u.act.stock, m + 11, 8);
        o->u.act.trading_state = m[19];
        o->u.act.reserved      = m[20];
        memcpy(o->u.act.reason, m + 21, 4);
        break;
    case 'Y':
        memcpy(o->u.sho.stock, m + 11, 8);
        o->u.sho.reg_sho_action = m[19];
        break;
    case 'L':
        memcpy(o->u.par.mpid, m + 11, 4);
        memcpy(o->u.par.stock, m + 15, 8);
        o->u.par.primary_mm        = m[23];
        o->u.par.mm_mode           = m[24];
        o->u.par.participant_state = m[25];
        break;
    case 'V':
        o->u.mwb.level1 = weft_load_be64(m + 11);
        o->u.mwb.level2 = weft_load_be64(m + 19);
        o->u.mwb.level3 = weft_load_be64(m + 27);
        break;
    case 'W':
        o->u.mws.breached_level = m[11];
        break;
    case 'K':
        memcpy(o->u.ipo.stock, m + 11, 8);
        o->u.ipo.release_time      = weft_load_be32(m + 19);
        o->u.ipo.release_qualifier = m[23];
        o->u.ipo.ipo_price_raw     = weft_load_be32(m + 24);
        break;
    case 'J':
        memcpy(o->u.lul.stock, m + 11, 8);
        o->u.lul.reference_price = weft_load_be64(m + 19);
        o->u.lul.upper_collar    = weft_load_be64(m + 27);
        o->u.lul.lower_collar    = weft_load_be64(m + 35);
        o->u.lul.extension       = weft_load_be32(m + 43);
        break;
    case 'A':
    case 'F':
        if (t == 'A') {
            o->u.add.order_ref = weft_load_be64(m + 11);
            o->u.add.buy_sell = m[19];
            o->u.add.shares   = weft_load_be32(m + 20);
            memcpy(o->u.add.stock, m + 24, 8);
            o->u.add.price_raw = weft_load_be32(m + 32);
        } else {
            o->u.addm.order_ref = weft_load_be64(m + 11);
            o->u.addm.buy_sell = m[19];
            o->u.addm.shares   = weft_load_be32(m + 20);
            memcpy(o->u.addm.stock, m + 24, 8);
            o->u.addm.price_raw = weft_load_be32(m + 32);
            memcpy(o->u.addm.mpid, m + 36, 4);
        }
        break;
    case 'E':
        o->u.exe.order_ref       = weft_load_be64(m + 11);
        o->u.exe.match_number    = weft_load_be64(m + 23);
        o->u.exe.executed_shares = weft_load_be32(m + 19);
        break;
    case 'C':
        o->u.exp.order_ref       = weft_load_be64(m + 11);
        o->u.exp.match_number    = weft_load_be64(m + 23);
        o->u.exp.executed_shares = weft_load_be32(m + 19);
        o->u.exp.execution_price = weft_load_be32(m + 32);
        o->u.exp.printable       = m[31];
        break;
    case 'X':
        o->u.cxl.order_ref        = weft_load_be64(m + 11);
        o->u.cxl.cancelled_shares = weft_load_be32(m + 19);
        break;
    case 'D':
        o->u.del.order_ref = weft_load_be64(m + 11);
        break;
    case 'U':
        o->u.rpl.original_order_ref = weft_load_be64(m + 11);
        o->u.rpl.new_order_ref      = weft_load_be64(m + 19);
        o->u.rpl.shares             = weft_load_be32(m + 27);
        o->u.rpl.price_raw          = weft_load_be32(m + 31);
        break;
    case 'P':
        o->u.trd.order_ref    = weft_load_be64(m + 11);
        o->u.trd.buy_sell     = m[19];
        o->u.trd.shares       = weft_load_be32(m + 20);
        memcpy(o->u.trd.stock, m + 24, 8);
        o->u.trd.price_raw    = weft_load_be32(m + 32);
        o->u.trd.match_number = weft_load_be64(m + 36);
        break;
    case 'Q':
        o->u.crs.shares          = weft_load_be64(m + 11);
        memcpy(o->u.crs.stock, m + 19, 8);
        o->u.crs.cross_price_raw = weft_load_be32(m + 27);
        o->u.crs.match_number    = weft_load_be64(m + 31);
        o->u.crs.cross_type      = m[39];
        break;
    case 'I':
        o->u.noi.paired_shares         = weft_load_be64(m + 11);
        o->u.noi.imbalance_shares      = weft_load_be64(m + 19);
        o->u.noi.imbalance_direction   = m[27];
        memcpy(o->u.noi.stock, m + 28, 8);
        o->u.noi.far_price_raw         = weft_load_be32(m + 36);
        o->u.noi.near_price_raw        = weft_load_be32(m + 40);
        o->u.noi.current_ref_price_raw = weft_load_be32(m + 44);
        o->u.noi.cross_type            = m[48];
        o->u.noi.price_variation       = m[49];
        break;
    default:
        return WEFT_ADAPTER_EBADMSG; /* keep the switch total for -Werror */
    }

    (void)need;
    return WEFT_ADAPTER_OK;
}

/* ================================================================== */
/* ITCH 5.0 public decode API                                         */
/* ================================================================== */

static int32_t weft_itch_decode_frame(const uint8_t *WEFT_RESTRICT buf,
                                      size_t avail, uint32_t flags,
                                      weft_itch_msg_t *WEFT_RESTRICT out,
                                      size_t *msg_size)
{
    size_t off = 0;
    size_t wire_len = 0;
    uint8_t type;
    size_t need;

    if (flags & WEFT_ITCH_F_LENPREFIX) {
        uint16_t prefix;
        if (avail < 2u) { return WEFT_ADAPTER_ETRUNC; }
        prefix = weft_load_be16(buf);
        off = 2u;
        if (prefix < 1u) { return WEFT_ADAPTER_EBADMSG; }
        wire_len = (size_t)prefix;
        if (avail <= off) { return WEFT_ADAPTER_ETRUNC; }
    } else {
        if (avail < 1u) { return WEFT_ADAPTER_ETRUNC; }
    }

    type = buf[off];
    need = (size_t)g_itch_wire_size[type];
    if (need == 0u) { return WEFT_ADAPTER_EBADMSG; }

    if (flags & WEFT_ITCH_F_LENPREFIX) {
        if (wire_len < need) { return WEFT_ADAPTER_EBADMSG; }
        if (wire_len > need && !(flags & WEFT_ITCH_F_LENIENT)) {
            return WEFT_ADAPTER_EBADMSG;
        }
    }
    if (avail - off < need) { return WEFT_ADAPTER_ETRUNC; }

    {
        int32_t st = weft_itch_decode_core(buf + off, need, out);
        if (st != WEFT_ADAPTER_OK) { return st; }
    }
    if (msg_size) {
        *msg_size = off + ((flags & WEFT_ITCH_F_LENPREFIX) ? wire_len : need);
    }
    return WEFT_ADAPTER_OK;
}

int32_t weft_itch50_decode_one(const uint8_t *WEFT_RESTRICT buf,
                               size_t avail, uint32_t flags,
                               weft_itch_msg_t *WEFT_RESTRICT out,
                               size_t *msg_size)
{
    if (!buf || !out) { return WEFT_ADAPTER_EINVAL; }
    if (((uintptr_t)out) & 7u) { return WEFT_ADAPTER_EALIGN; }
    if (flags & ~(uint32_t)(WEFT_ITCH_F_LENPREFIX | WEFT_ITCH_F_LENIENT)) {
        return WEFT_ADAPTER_EINVAL;
    }
    return weft_itch_decode_frame(buf, avail, flags, out, msg_size);
}

size_t weft_itch50_decode_batch(const uint8_t *WEFT_RESTRICT buf,
                                size_t len, uint32_t flags,
                                weft_itch_msg_t *WEFT_RESTRICT out,
                                size_t out_max, int32_t *status,
                                size_t *consumed)
{
    size_t off = 0;
    size_t count = 0;

    if (status)   { *status = WEFT_ADAPTER_OK; }
    if (consumed) { *consumed = 0; }

    if (!buf || !out || out_max == 0u) {
        if (status) { *status = WEFT_ADAPTER_EINVAL; }
        return 0;
    }
    if (((uintptr_t)out) & 63u) {
        if (status) { *status = WEFT_ADAPTER_EALIGN; }
        return 0;
    }
    if (flags & ~(uint32_t)(WEFT_ITCH_F_LENPREFIX | WEFT_ITCH_F_LENIENT)) {
        if (status) { *status = WEFT_ADAPTER_EINVAL; }
        return 0;
    }

    while (count < out_max && off < len) {
        size_t sz = 0;
        int32_t st = weft_itch_decode_frame(buf + off, len - off, flags,
                                            &out[count], &sz);
        if (st != WEFT_ADAPTER_OK) {
            if (status)   { *status = st; }
            if (consumed) { *consumed = off; }
            return count;
        }
        off += sz;
        ++count;
    }

    if (consumed) { *consumed = off; }
    return count;
}

/* ================================================================== */
/* MoldUDP64 downstream packet walker                                 */
/* ================================================================== */

int32_t weft_mold64_open(const uint8_t *pkt, size_t len,
                         weft_mold_view_t *view)
{
    if (!pkt || !view) { return WEFT_ADAPTER_EINVAL; }
    if (len < 16u)     { return WEFT_ADAPTER_ETRUNC; }

    memcpy(view->session, pkt, 10);
    view->sequence   = weft_load_be32(pkt + 10);
    view->count      = weft_load_be16(pkt + 14);
    view->next_index = 0;
    view->blocks     = pkt + 16;
    view->cursor     = pkt + 16;
    view->end        = pkt + len;
    return WEFT_ADAPTER_OK;
}

int32_t weft_mold64_next(weft_mold_view_t *view,
                         const uint8_t **msg_out, uint16_t *len_out)
{
    uint16_t blk;
    size_t remaining;

    if (!view || !msg_out || !len_out) { return WEFT_ADAPTER_EINVAL; }
    if (view->next_index >= view->count) { return 0; }
    if ((size_t)(view->end - view->cursor) < 2u) { return WEFT_ADAPTER_ETRUNC; }

    blk = weft_load_be16(view->cursor);
    if (blk == 0u) {
        /* MoldUDP64 end-of-session marker. */
        view->next_index = view->count;
        return 0;
    }

    remaining = (size_t)(view->end - view->cursor) - 2u;
    if (remaining < (size_t)blk) { return WEFT_ADAPTER_ETRUNC; }

    *msg_out = view->cursor + 2;
    *len_out = blk;
    view->cursor += 2u + (size_t)blk;
    view->next_index++;
    return 1;
}

/* ================================================================== */
/* OUCH 5.0 decode (length-prefixed SoupBinTCP payload framing)       */
/* ================================================================== */

static int32_t weft_ouch_decode_core(const uint8_t *WEFT_RESTRICT m,
                                     uint8_t type,
                                     weft_ouch_msg_t *WEFT_RESTRICT o)
{
    /* m points at the TYPE byte (m[0]); 6-byte timestamp at m[1..6];
     * message body fields follow at m[7] onward. Wire offsets pinned:
     *   'S': event           m[7]
     *   'A': ref m[7] side m[15] shares m[16] stock m[20] price m[28]
     *        tif m[32] firm m[36] display m[40] state m[41] cap m[42]
     *        sweep m[43] cross m[44] customer m[45]
     *   'E': ref m[7] shares m[15] match m[19]
     *   'C': ref m[7] shares m[15] match m[19] printable m[27] price m[28]
     *   'X': ref m[7] shares m[15]
     *   'U': orig m[7] new m[15] shares m[23] price m[27]
     */

    /* ---- validation pass (wire bytes only, no writes) ---- */
    if (weft_load_be48(m + 1) >= WEFT_ITCH_NS_PER_DAY) {
        return WEFT_ADAPTER_EBADMSG;
    }
    switch (type) {
    case 'S':
        break;
    case 'A':
        if (!weft_v_side(m[15]) ||
            weft_load_be32(m + 16) == 0u ||
            !weft_v_printable(m + 20, 8) ||
            !weft_v_printable(m + 32, 4) || /* time in force  */
            !weft_v_printable(m + 36, 4) || /* firm           */
            !weft_v_yn(m[40])) {
            return WEFT_ADAPTER_EBADMSG;
        }
        break;
    case 'E':
        if (weft_load_be32(m + 15) == 0u) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'C':
        if (weft_load_be32(m + 15) == 0u ||
            !weft_v_yn(m[27])) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'X':
        if (weft_load_be32(m + 15) == 0u) { return WEFT_ADAPTER_EBADMSG; }
        break;
    case 'U':
        if (weft_load_be32(m + 23) == 0u) { return WEFT_ADAPTER_EBADMSG; }
        break;
    default:
        return WEFT_ADAPTER_EBADMSG;
    }

    /* ---- projection pass ---- */
    o->hdr.timestamp_ns = weft_load_be48(m + 1);
    o->hdr.msg_type     = type;

    switch (type) {
    case 'S':
        o->u.sys.event_code = m[7];
        break;
    case 'A':
        o->u.acc.order_ref = weft_load_be64(m + 7);
        o->u.acc.buy_sell  = m[15];
        o->u.acc.shares    = weft_load_be32(m + 16);
        memcpy(o->u.acc.stock, m + 20, 8);
        o->u.acc.price_raw = weft_load_be32(m + 28);
        memcpy(o->u.acc.time_in_force, m + 32, 4);
        memcpy(o->u.acc.firm, m + 36, 4);
        o->u.acc.display           = m[40];
        o->u.acc.order_state       = m[41];
        o->u.acc.capacity          = m[42];
        o->u.acc.intermarket_sweep = m[43];
        o->u.acc.cross_type        = m[44];
        o->u.acc.customer_type     = m[45];
        break;
    case 'E':
        o->u.exe.order_ref       = weft_load_be64(m + 7);
        o->u.exe.executed_shares = weft_load_be32(m + 15);
        o->u.exe.match_number    = weft_load_be64(m + 19);
        break;
    case 'C':
        o->u.exp.order_ref       = weft_load_be64(m + 7);
        o->u.exp.executed_shares = weft_load_be32(m + 15);
        o->u.exp.match_number    = weft_load_be64(m + 19);
        o->u.exp.printable       = m[27];
        o->u.exp.execution_price = weft_load_be32(m + 28);
        break;
    case 'X':
        o->u.cxl.order_ref        = weft_load_be64(m + 7);
        o->u.cxl.cancelled_shares = weft_load_be32(m + 15);
        break;
    case 'U':
        o->u.rpl.original_order_ref = weft_load_be64(m + 7);
        o->u.rpl.new_order_ref      = weft_load_be64(m + 15);
        o->u.rpl.shares             = weft_load_be32(m + 23);
        o->u.rpl.price_raw          = weft_load_be32(m + 27);
        break;
    default:
        return WEFT_ADAPTER_EBADMSG;
    }
    return WEFT_ADAPTER_OK;
}

int32_t weft_ouch50_decode_one(const uint8_t *WEFT_RESTRICT buf,
                               size_t avail,
                               weft_ouch_msg_t *WEFT_RESTRICT out,
                               size_t *msg_size)
{
    uint16_t prefix;
    uint8_t type;
    size_t need;

    if (!buf || !out) { return WEFT_ADAPTER_EINVAL; }
    if (((uintptr_t)out) & 7u) { return WEFT_ADAPTER_EALIGN; }
    if (avail < 3u) { return WEFT_ADAPTER_ETRUNC; }

    prefix = weft_load_be16(buf);
    if (prefix < 1u) { return WEFT_ADAPTER_EBADMSG; }
    if ((size_t)prefix > avail - 2u) { return WEFT_ADAPTER_ETRUNC; }

    type = buf[2];
    need = (size_t)g_ouch_wire_size[type];
    if (need == 0u) { return WEFT_ADAPTER_EBADMSG; }
    if ((size_t)prefix != need) { return WEFT_ADAPTER_EBADMSG; }

    {
        int32_t st = weft_ouch_decode_core(buf + 2, type, out);
        if (st != WEFT_ADAPTER_OK) { return st; }
    }
    if (msg_size) { *msg_size = 2u + (size_t)prefix; }
    return WEFT_ADAPTER_OK;
}

size_t weft_ouch50_decode_batch(const uint8_t *WEFT_RESTRICT buf,
                                size_t len,
                                weft_ouch_msg_t *WEFT_RESTRICT out,
                                size_t out_max, int32_t *status,
                                size_t *consumed)
{
    size_t off = 0;
    size_t count = 0;

    if (status)   { *status = WEFT_ADAPTER_OK; }
    if (consumed) { *consumed = 0; }

    if (!buf || !out || out_max == 0u) {
        if (status) { *status = WEFT_ADAPTER_EINVAL; }
        return 0;
    }
    if (((uintptr_t)out) & 63u) {
        if (status) { *status = WEFT_ADAPTER_EALIGN; }
        return 0;
    }

    while (count < out_max && off < len) {
        size_t sz = 0;
        int32_t st = weft_ouch50_decode_one(buf + off, len - off,
                                            &out[count], &sz);
        if (st != WEFT_ADAPTER_OK) {
            if (status)   { *status = st; }
            if (consumed) { *consumed = off; }
            return count;
        }
        off += sz;
        ++count;
    }

    if (consumed) { *consumed = off; }
    return count;
}

/* ABI probe for managed bindings. */
int weft_adapters_abi_version(void)
{
    return WEFT_ADAPTERS_ABI_VERSION;
}
