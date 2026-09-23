/*
 * test_util.h - Weft Pillar 6 shared test harness (header-only).
 *
 * Single source of truth for the GOLDEN fixtures: the tables below
 * feed both the wire ENCODERS (synthetic feeds, PCAP) and the decoded
 * VERIFIERS (bit-exact field checks). Any drift between encoder and
 * parser shows up as a failed check with a field-level reason.
 *
 * Everything is static inline (single TU per test binary, no unused-
 * function warnings). Tests must include this FIRST (it may define
 * _POSIX_C_SOURCE for clock_gettime).
 */
#ifndef WEFT_TEST_UTIL_H
#define WEFT_TEST_UTIL_H

#if !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE) && \
    !defined(_GNU_SOURCE) && !defined(_BSD_SOURCE) && !defined(_DEFAULT_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "weft_adapters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

/* ================================================================== */
/* Check accounting                                                   */
/* ================================================================== */

static long g_checks = 0;
static long g_fails  = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_fails;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_I64(a, b)                                                 \
    do {                                                                \
        long long va_ = (long long)(a), vb_ = (long long)(b);           \
        ++g_checks;                                                     \
        if (va_ != vb_) {                                               \
            ++g_fails;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n",    \
                    __FILE__, __LINE__, #a, #b, va_, vb_);              \
        }                                                               \
    } while (0)

#define CHECK_U64X(a, b)                                                \
    do {                                                                \
        uint64_t va_ = (uint64_t)(a), vb_ = (uint64_t)(b);              \
        ++g_checks;                                                     \
        if (va_ != vb_) {                                               \
            ++g_fails;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s == %s (0x%016" PRIX64       \
                    " vs 0x%016" PRIX64 ")\n",                          \
                    __FILE__, __LINE__, #a, #b, va_, vb_);              \
        }                                                               \
    } while (0)

#define REPORT_AND_EXIT(name)                                           \
    do {                                                                \
        printf("TEST %s: %s checks=%ld failures=%ld\n", (name),         \
               g_fails ? "FAIL" : "PASS", g_checks, g_fails);           \
        return g_fails ? 1 : 0;                                         \
    } while (0)

/* ================================================================== */
/* Timing + PRNG                                                      */
/* ================================================================== */

static inline double weft_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static inline uint64_t weft_prng(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static inline long weft_env_cycles(const char *key, long dflt)
{
    const char *v = getenv(key);
    return (v && *v) ? strtol(v, NULL, 10) : dflt;
}

/* ================================================================== */
/* Byte put/get helpers                                               */
/* ================================================================== */

static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void put_be48(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 40);
    p[1] = (uint8_t)(v >> 32);
    p[2] = (uint8_t)(v >> 24);
    p[3] = (uint8_t)(v >> 16);
    p[4] = (uint8_t)(v >> 8);
    p[5] = (uint8_t)v;
}

static inline void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ================================================================== */
/* GOLDEN value table (normative, synthetic NASDAQ-style session)     */
/* ================================================================== */

typedef struct golden_msg {
    uint8_t  type;
    uint16_t locate;
    uint16_t tracking;
    uint64_t ts;
    uint64_t u64a, u64b, u64c;
    uint32_t u32a, u32b, u32c;
    uint8_t  c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12;
    uint8_t  stock[8];
    uint8_t  text4a[4];
    uint8_t  text4b[4];
    uint8_t  text2[2];
} golden_msg_t;

#define GOLDEN_LOCATE    0x1A2Bu
#define GOLDEN_TRACKING  0x00C7u
#define GOLDEN_TS_BASE   0x112233445566ull

static const golden_msg_t g_golden_itch[] = {
    /* 0: 'S' start of messages */
    { .type = 'S', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE, .c1 = 'O' },
    /* 1: 'R' stock directory (AAPL) */
    { .type = 'R', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 1000, .stock = "AAPL    ",
      .c1 = 'Q', .c2 = 'N', .u32a = 100u, .c3 = 'N', .c4 = 'C',
      .text2 = { 'C', ' ' }, .c5 = 'P', .c6 = 'N', .c7 = 'N',
      .c8 = '1', .c9 = 'N', .u32b = 5u, .c10 = 'N' },
    /* 2: 'H' trading action */
    { .type = 'H', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 2000, .stock = "AAPL    ",
      .c1 = 'T', .c2 = ' ', .text4a = "T1  " },
    /* 3: 'Y' reg SHO */
    { .type = 'Y', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 3000, .stock = "GOOGL   ", .c1 = '1' },
    /* 4: 'L' market participant position */
    { .type = 'L', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 4000, .text4a = "NSDQ",
      .stock = "AAPL    ", .c1 = 'Y', .c2 = 'N', .c3 = 'A' },
    /* 5: 'V' MWCB decline levels */
    { .type = 'V', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 5000,
      .u64a = 0x1111111111111111ull, .u64b = 0x2222222222222222ull,
      .u64c = 0x3333333333333333ull },
    /* 6: 'W' MWCB status */
    { .type = 'W', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 6000, .c1 = '1' },
    /* 7: 'K' IPO quoting period */
    { .type = 'K', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 7000, .stock = "ZVZZT   ",
      .u32a = 34200u, .c1 = 'A', .u32b = 100000000u },
    /* 8: 'J' LULD auction collar */
    { .type = 'J', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 8000, .stock = "AAPL    ",
      .u64a = 1234500000ull, .u64b = 1300000000ull,
      .u64c = 1100000000ull, .u32a = 300u },
    /* 9: 'A' add order */
    { .type = 'A', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 9000, .u64a = 0x0100000000000001ull,
      .c1 = 'B', .u32a = 300u, .stock = "AAPL    ", .u32b = 123450000u },
    /* 10: 'F' add order with MPID */
    { .type = 'F', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 10000, .u64a = 0x0100000000000002ull,
      .c1 = 'S', .u32a = 200u, .stock = "GOOGL   ",
      .u32b = 99999999u, .text4a = "NSDQ" },
    /* 11: 'E' order executed */
    { .type = 'E', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 11000, .u64a = 0x0100000000000001ull,
      .u32a = 100u, .u64b = 0x0000000000AB0001ull },
    /* 12: 'C' order executed with price */
    { .type = 'C', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 12000, .u64a = 0x0100000000000001ull,
      .u32a = 50u, .u64b = 0x0000000000AB0002ull, .c1 = 'N',
      .u32b = 123450001u },
    /* 13: 'X' order cancel */
    { .type = 'X', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 13000, .u64a = 0x0100000000000001ull,
      .u32a = 100u },
    /* 14: 'U' order replace */
    { .type = 'U', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 14000, .u64a = 0x0100000000000001ull,
      .u64b = 0x0100000000000003ull, .u32a = 400u, .u32b = 123449999u },
    /* 15: 'D' order delete */
    { .type = 'D', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 15000, .u64a = 0x0100000000000002ull },
    /* 16: 'P' trade (non-cross) */
    { .type = 'P', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 16000, .u64a = 0x0200000000000001ull,
      .c1 = 'B', .u32a = 500u, .stock = "AAPL    ",
      .u32b = 123450002u, .u64b = 0x0000000000AB0003ull },
    /* 17: 'Q' cross trade */
    { .type = 'Q', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 17000, .u64a = 1000000ull,
      .stock = "GOOGL   ", .u32a = 99999998u,
      .u64b = 0x0000000000AB0004ull, .c1 = 'O' },
    /* 18: 'I' NOII */
    { .type = 'I', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 18000, .u64a = 5000ull, .u64b = 1200ull,
      .c1 = 'B', .stock = "AAPL    ", .u32a = 123450003u,
      .u32b = 123450004u, .u32c = 123450005u, .c2 = 'O', .c3 = 'L' },
    /* 19: 'A' extra volume message */
    { .type = 'A', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 19000, .u64a = 0x0100000000000004ull,
      .c1 = 'S', .u32a = 700u, .stock = "MSFT    ", .u32b = 456700000u },
    /* 20: 'E' extra volume message */
    { .type = 'E', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 20000, .u64a = 0x0100000000000004ull,
      .u32a = 300u, .u64b = 0x0000000000AB0005ull },
    /* 21: 'D' extra volume message */
    { .type = 'D', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 21000, .u64a = 0x0100000000000003ull },
    /* 22: 'S' end of messages */
    { .type = 'S', .locate = GOLDEN_LOCATE, .tracking = GOLDEN_TRACKING,
      .ts = GOLDEN_TS_BASE + 22000, .c1 = 'C' }
};

#define GOLDEN_ITCH_COUNT (sizeof(g_golden_itch) / sizeof(g_golden_itch[0]))

/* ================================================================== */
/* ITCH wire encoder (golden -> wire bytes)                           */
/* ================================================================== */

static inline size_t itch_encode_msg(uint8_t *o, const golden_msg_t *g)
{
    o[0] = g->type;
    put_be16(o + 1, g->locate);
    put_be16(o + 3, g->tracking);
    put_be48(o + 5, g->ts);

    switch (g->type) {
    case 'S':
        o[11] = g->c1;
        return 12;
    case 'R':
        memcpy(o + 11, g->stock, 8);
        o[19] = g->c1;   /* market category      */
        o[20] = g->c2;   /* financial status     */
        put_be32(o + 21, g->u32a);  /* round lot size */
        o[25] = g->c3;   /* round lots only      */
        o[26] = g->c4;   /* issue classification */
        memcpy(o + 27, g->text2, 2);
        o[29] = g->c5;   /* authenticity         */
        o[30] = g->c6;   /* short sale threshold */
        o[31] = g->c7;   /* IPO flag             */
        o[32] = g->c8;   /* LULD tier            */
        o[33] = g->c9;   /* ETP flag             */
        put_be32(o + 34, g->u32b);  /* ETP leverage  */
        o[38] = g->c10;  /* inverse indicator    */
        return 39;
    case 'H':
        memcpy(o + 11, g->stock, 8);
        o[19] = g->c1;   /* trading state */
        o[20] = g->c2;   /* reserved      */
        memcpy(o + 21, g->text4a, 4);
        return 25;
    case 'Y':
        memcpy(o + 11, g->stock, 8);
        o[19] = g->c1;
        return 20;
    case 'L':
        memcpy(o + 11, g->text4a, 4);  /* MPID  */
        memcpy(o + 15, g->stock, 8);
        o[23] = g->c1;   /* primary MM     */
        o[24] = g->c2;   /* MM mode        */
        o[25] = g->c3;   /* participant st */
        return 26;
    case 'V':
        put_be64(o + 11, g->u64a);
        put_be64(o + 19, g->u64b);
        put_be64(o + 27, g->u64c);
        return 35;
    case 'W':
        o[11] = g->c1;
        return 12;
    case 'K':
        memcpy(o + 11, g->stock, 8);
        put_be32(o + 19, g->u32a);  /* release time (ssm) */
        o[23] = g->c1;              /* qualifier          */
        put_be32(o + 24, g->u32b);  /* IPO price          */
        return 28;
    case 'J':
        memcpy(o + 11, g->stock, 8);
        put_be64(o + 19, g->u64a);
        put_be64(o + 27, g->u64b);
        put_be64(o + 35, g->u64c);
        put_be32(o + 43, g->u32a);  /* extension */
        return 47;
    case 'A':
        put_be64(o + 11, g->u64a);  /* order ref */
        o[19] = g->c1;              /* buy/sell  */
        put_be32(o + 20, g->u32a);  /* shares    */
        memcpy(o + 24, g->stock, 8);
        put_be32(o + 32, g->u32b);  /* price     */
        return 36;
    case 'F':
        put_be64(o + 11, g->u64a);
        o[19] = g->c1;
        put_be32(o + 20, g->u32a);
        memcpy(o + 24, g->stock, 8);
        put_be32(o + 32, g->u32b);
        memcpy(o + 36, g->text4a, 4);  /* MPID */
        return 40;
    case 'E':
        put_be64(o + 11, g->u64a);
        put_be32(o + 19, g->u32a);
        put_be64(o + 23, g->u64b);
        return 31;
    case 'C':
        put_be64(o + 11, g->u64a);
        put_be32(o + 19, g->u32a);
        put_be64(o + 23, g->u64b);
        o[31] = g->c1;              /* printable */
        put_be32(o + 32, g->u32b);  /* exec px   */
        return 36;
    case 'X':
        put_be64(o + 11, g->u64a);
        put_be32(o + 19, g->u32a);
        return 23;
    case 'D':
        put_be64(o + 11, g->u64a);
        return 19;
    case 'U':
        put_be64(o + 11, g->u64a);
        put_be64(o + 19, g->u64b);
        put_be32(o + 27, g->u32a);
        put_be32(o + 31, g->u32b);
        return 35;
    case 'P':
        put_be64(o + 11, g->u64a);
        o[19] = g->c1;
        put_be32(o + 20, g->u32a);
        memcpy(o + 24, g->stock, 8);
        put_be32(o + 32, g->u32b);
        put_be64(o + 36, g->u64b);
        return 44;
    case 'Q':
        put_be64(o + 11, g->u64a);  /* shares (8B) */
        memcpy(o + 19, g->stock, 8);
        put_be32(o + 27, g->u32a);  /* cross price  */
        put_be64(o + 31, g->u64b);  /* match number */
        o[39] = g->c1;              /* cross type   */
        return 40;
    case 'I':
        put_be64(o + 11, g->u64a);  /* paired      */
        put_be64(o + 19, g->u64b);  /* imbalance   */
        o[27] = g->c1;              /* direction   */
        memcpy(o + 28, g->stock, 8);
        put_be32(o + 36, g->u32a);  /* far         */
        put_be32(o + 40, g->u32b);  /* near        */
        put_be32(o + 44, g->u32c);  /* current ref */
        o[48] = g->c2;              /* cross type  */
        o[49] = g->c3;              /* variation   */
        return 50;
    default:
        return 0;
    }
}

static inline size_t itch_golden_build_fixed(uint8_t *out, size_t cap)
{
    size_t off = 0, i;
    for (i = 0; i < GOLDEN_ITCH_COUNT; ++i) {
        size_t n = itch_encode_msg(out + off, &g_golden_itch[i]);
        CHECK(n == weft_itch50_wire_size(g_golden_itch[i].type));
        CHECK(off + n <= cap);
        off += n;
    }
    return off;
}

static inline size_t itch_golden_build_lenprefixed(uint8_t *out, size_t cap)
{
    size_t off = 0, i;
    for (i = 0; i < GOLDEN_ITCH_COUNT; ++i) {
        size_t n = itch_encode_msg(out + off + 2, &g_golden_itch[i]);
        CHECK(n == weft_itch50_wire_size(g_golden_itch[i].type));
        CHECK(off + 2 + n <= cap);
        put_be16(out + off, (uint16_t)n);
        off += 2 + n;
    }
    return off;
}

/* ================================================================== */
/* ITCH decoded-message verifier (bit-exact, field-level)             */
/* ================================================================== */

static inline void verify_itch_hdr(const golden_msg_t *g,
                                   const weft_itch_msg_t *m)
{
    CHECK_U64X(m->hdr.msg_type, g->type);
    CHECK_U64X(m->hdr.stock_locate, g->locate);
    CHECK_U64X(m->hdr.tracking_number, g->tracking);
    CHECK_U64X(m->hdr.timestamp_ns, g->ts);
}

static inline int verify_itch(const golden_msg_t *g, const weft_itch_msg_t *m)
{
    verify_itch_hdr(g, m);
    switch (g->type) {
    case 'S':
        CHECK_U64X(m->u.sys.event_code, g->c1);
        break;
    case 'R':
        CHECK(memcmp(m->u.dir.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.dir.market_category, g->c1);
        CHECK_U64X(m->u.dir.financial_status, g->c2);
        CHECK_U64X(m->u.dir.round_lot_size, g->u32a);
        CHECK_U64X(m->u.dir.round_lots_only, g->c3);
        CHECK_U64X(m->u.dir.issue_classification, g->c4);
        CHECK(memcmp(m->u.dir.issue_subtype, g->text2, 2) == 0);
        CHECK_U64X(m->u.dir.authenticity, g->c5);
        CHECK_U64X(m->u.dir.short_sale_threshold, g->c6);
        CHECK_U64X(m->u.dir.ipo_flag, g->c7);
        CHECK_U64X(m->u.dir.luld_tier, g->c8);
        CHECK_U64X(m->u.dir.etp_flag, g->c9);
        CHECK_U64X(m->u.dir.etp_leverage_factor, g->u32b);
        CHECK_U64X(m->u.dir.inverse_indicator, g->c10);
        break;
    case 'H':
        CHECK(memcmp(m->u.act.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.act.trading_state, g->c1);
        CHECK_U64X(m->u.act.reserved, g->c2);
        CHECK(memcmp(m->u.act.reason, g->text4a, 4) == 0);
        break;
    case 'Y':
        CHECK(memcmp(m->u.sho.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.sho.reg_sho_action, g->c1);
        break;
    case 'L':
        CHECK(memcmp(m->u.par.mpid, g->text4a, 4) == 0);
        CHECK(memcmp(m->u.par.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.par.primary_mm, g->c1);
        CHECK_U64X(m->u.par.mm_mode, g->c2);
        CHECK_U64X(m->u.par.participant_state, g->c3);
        break;
    case 'V':
        CHECK_U64X(m->u.mwb.level1, g->u64a);
        CHECK_U64X(m->u.mwb.level2, g->u64b);
        CHECK_U64X(m->u.mwb.level3, g->u64c);
        break;
    case 'W':
        CHECK_U64X(m->u.mws.breached_level, g->c1);
        break;
    case 'K':
        CHECK(memcmp(m->u.ipo.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.ipo.release_time, g->u32a);
        CHECK_U64X(m->u.ipo.release_qualifier, g->c1);
        CHECK_U64X(m->u.ipo.ipo_price_raw, g->u32b);
        break;
    case 'J':
        CHECK(memcmp(m->u.lul.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.lul.reference_price, g->u64a);
        CHECK_U64X(m->u.lul.upper_collar, g->u64b);
        CHECK_U64X(m->u.lul.lower_collar, g->u64c);
        CHECK_U64X(m->u.lul.extension, g->u32a);
        break;
    case 'A':
        CHECK_U64X(m->u.add.order_ref, g->u64a);
        CHECK_U64X(m->u.add.buy_sell, g->c1);
        CHECK_U64X(m->u.add.shares, g->u32a);
        CHECK(memcmp(m->u.add.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.add.price_raw, g->u32b);
        break;
    case 'F':
        CHECK_U64X(m->u.addm.order_ref, g->u64a);
        CHECK_U64X(m->u.addm.buy_sell, g->c1);
        CHECK_U64X(m->u.addm.shares, g->u32a);
        CHECK(memcmp(m->u.addm.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.addm.price_raw, g->u32b);
        CHECK(memcmp(m->u.addm.mpid, g->text4a, 4) == 0);
        break;
    case 'E':
        CHECK_U64X(m->u.exe.order_ref, g->u64a);
        CHECK_U64X(m->u.exe.executed_shares, g->u32a);
        CHECK_U64X(m->u.exe.match_number, g->u64b);
        break;
    case 'C':
        CHECK_U64X(m->u.exp.order_ref, g->u64a);
        CHECK_U64X(m->u.exp.executed_shares, g->u32a);
        CHECK_U64X(m->u.exp.match_number, g->u64b);
        CHECK_U64X(m->u.exp.printable, g->c1);
        CHECK_U64X(m->u.exp.execution_price, g->u32b);
        break;
    case 'X':
        CHECK_U64X(m->u.cxl.order_ref, g->u64a);
        CHECK_U64X(m->u.cxl.cancelled_shares, g->u32a);
        break;
    case 'D':
        CHECK_U64X(m->u.del.order_ref, g->u64a);
        break;
    case 'U':
        CHECK_U64X(m->u.rpl.original_order_ref, g->u64a);
        CHECK_U64X(m->u.rpl.new_order_ref, g->u64b);
        CHECK_U64X(m->u.rpl.shares, g->u32a);
        CHECK_U64X(m->u.rpl.price_raw, g->u32b);
        break;
    case 'P':
        CHECK_U64X(m->u.trd.order_ref, g->u64a);
        CHECK_U64X(m->u.trd.buy_sell, g->c1);
        CHECK_U64X(m->u.trd.shares, g->u32a);
        CHECK(memcmp(m->u.trd.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.trd.price_raw, g->u32b);
        CHECK_U64X(m->u.trd.match_number, g->u64b);
        break;
    case 'Q':
        CHECK_U64X(m->u.crs.shares, g->u64a);
        CHECK(memcmp(m->u.crs.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.crs.cross_price_raw, g->u32a);
        CHECK_U64X(m->u.crs.match_number, g->u64b);
        CHECK_U64X(m->u.crs.cross_type, g->c1);
        break;
    case 'I':
        CHECK_U64X(m->u.noi.paired_shares, g->u64a);
        CHECK_U64X(m->u.noi.imbalance_shares, g->u64b);
        CHECK_U64X(m->u.noi.imbalance_direction, g->c1);
        CHECK(memcmp(m->u.noi.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.noi.far_price_raw, g->u32a);
        CHECK_U64X(m->u.noi.near_price_raw, g->u32b);
        CHECK_U64X(m->u.noi.current_ref_price_raw, g->u32c);
        CHECK_U64X(m->u.noi.cross_type, g->c2);
        CHECK_U64X(m->u.noi.price_variation, g->c3);
        break;
    default:
        CHECK(0 && "verifier: unknown golden type");
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* OUCH 5.0 golden table + encoder + verifier                         */
/* ================================================================== */

static const golden_msg_t g_golden_ouch[] = {
    /* 0: 'S' start of messages */
    { .type = 'S', .ts = GOLDEN_TS_BASE + 100, .c1 = 'S' },
    /* 1: 'A' order accepted */
    { .type = 'A', .ts = GOLDEN_TS_BASE + 200,
      .u64a = 0x01000000000000AAull, .c1 = 'B', .u32a = 400u,
      .stock = "AAPL    ", .u32b = 123450000u, .text4a = "DAY ",
      .text4b = "NSDQ", .c2 = 'Y', .c3 = 'L', .c4 = 'A', .c5 = 'Y',
      .c6 = 'O', .c7 = '1' },
    /* 2: 'E' order executed */
    { .type = 'E', .ts = GOLDEN_TS_BASE + 300,
      .u64a = 0x01000000000000AAull, .u32a = 100u,
      .u64b = 0x0000000000CD0001ull },
    /* 3: 'C' order executed with price */
    { .type = 'C', .ts = GOLDEN_TS_BASE + 400,
      .u64a = 0x01000000000000AAull, .u32a = 150u,
      .u64b = 0x0000000000CD0002ull, .c1 = 'N', .u32b = 123450003u },
    /* 4: 'X' order canceled */
    { .type = 'X', .ts = GOLDEN_TS_BASE + 500,
      .u64a = 0x01000000000000AAull, .u32a = 150u },
    /* 5: 'U' order replaced */
    { .type = 'U', .ts = GOLDEN_TS_BASE + 600,
      .u64a = 0x01000000000000AAull, .u64b = 0x01000000000000BBull,
      .u32a = 300u, .u32b = 123449000u },
    /* 6: 'S' end of messages */
    { .type = 'S', .ts = GOLDEN_TS_BASE + 700, .c1 = 'C' }
};

#define GOLDEN_OUCH_COUNT (sizeof(g_golden_ouch) / sizeof(g_golden_ouch[0]))

static inline size_t ouch_encode_msg(uint8_t *o, const golden_msg_t *g)
{
    /* o points at the TYPE byte; the 2-byte length prefix is prepended
     * by the caller (SoupBinTCP payload framing). */
    o[0] = g->type;
    put_be48(o + 1, g->ts);
    switch (g->type) {
    case 'S':
        o[7] = g->c1;
        return 8;
    case 'A':
        put_be64(o + 7, g->u64a);   /* order ref    */
        o[15] = g->c1;              /* buy/sell     */
        put_be32(o + 16, g->u32a);  /* shares       */
        memcpy(o + 20, g->stock, 8);
        put_be32(o + 28, g->u32b);  /* price        */
        memcpy(o + 32, g->text4a, 4); /* time in force */
        memcpy(o + 36, g->text4b, 4); /* firm          */
        o[40] = g->c2;              /* display      */
        o[41] = g->c3;              /* order state  */
        o[42] = g->c4;              /* capacity     */
        o[43] = g->c5;              /* ISO sweep    */
        o[44] = g->c6;              /* cross type   */
        o[45] = g->c7;              /* customer     */
        return 46;
    case 'E':
        put_be64(o + 7, g->u64a);
        put_be32(o + 15, g->u32a);
        put_be64(o + 19, g->u64b);
        return 27;
    case 'C':
        put_be64(o + 7, g->u64a);
        put_be32(o + 15, g->u32a);
        put_be64(o + 19, g->u64b);
        o[27] = g->c1;
        put_be32(o + 28, g->u32b);
        return 32;
    case 'X':
        put_be64(o + 7, g->u64a);
        put_be32(o + 15, g->u32a);
        return 19;
    case 'U':
        put_be64(o + 7, g->u64a);
        put_be64(o + 15, g->u64b);
        put_be32(o + 23, g->u32a);
        put_be32(o + 27, g->u32b);
        return 31;
    default:
        return 0;
    }
}

static inline size_t ouch_golden_build(uint8_t *out, size_t cap)
{
    size_t off = 0, i;
    for (i = 0; i < GOLDEN_OUCH_COUNT; ++i) {
        size_t n = ouch_encode_msg(out + off + 2, &g_golden_ouch[i]);
        CHECK(n == weft_ouch50_wire_size(g_golden_ouch[i].type));
        CHECK(off + 2 + n <= cap);
        put_be16(out + off, (uint16_t)n);
        off += 2 + n;
    }
    return off;
}

static inline int verify_ouch(const golden_msg_t *g, const weft_ouch_msg_t *m)
{
    CHECK_U64X(m->hdr.msg_type, g->type);
    CHECK_U64X(m->hdr.timestamp_ns, g->ts);
    switch (g->type) {
    case 'S':
        CHECK_U64X(m->u.sys.event_code, g->c1);
        break;
    case 'A':
        CHECK_U64X(m->u.acc.order_ref, g->u64a);
        CHECK_U64X(m->u.acc.buy_sell, g->c1);
        CHECK_U64X(m->u.acc.shares, g->u32a);
        CHECK(memcmp(m->u.acc.stock, g->stock, 8) == 0);
        CHECK_U64X(m->u.acc.price_raw, g->u32b);
        CHECK(memcmp(m->u.acc.time_in_force, g->text4a, 4) == 0);
        CHECK(memcmp(m->u.acc.firm, g->text4b, 4) == 0);
        CHECK_U64X(m->u.acc.display, g->c2);
        CHECK_U64X(m->u.acc.order_state, g->c3);
        CHECK_U64X(m->u.acc.capacity, g->c4);
        CHECK_U64X(m->u.acc.intermarket_sweep, g->c5);
        CHECK_U64X(m->u.acc.cross_type, g->c6);
        CHECK_U64X(m->u.acc.customer_type, g->c7);
        break;
    case 'E':
        CHECK_U64X(m->u.exe.order_ref, g->u64a);
        CHECK_U64X(m->u.exe.executed_shares, g->u32a);
        CHECK_U64X(m->u.exe.match_number, g->u64b);
        break;
    case 'C':
        CHECK_U64X(m->u.exp.order_ref, g->u64a);
        CHECK_U64X(m->u.exp.executed_shares, g->u32a);
        CHECK_U64X(m->u.exp.match_number, g->u64b);
        CHECK_U64X(m->u.exp.printable, g->c1);
        CHECK_U64X(m->u.exp.execution_price, g->u32b);
        break;
    case 'X':
        CHECK_U64X(m->u.cxl.order_ref, g->u64a);
        CHECK_U64X(m->u.cxl.cancelled_shares, g->u32a);
        break;
    case 'U':
        CHECK_U64X(m->u.rpl.original_order_ref, g->u64a);
        CHECK_U64X(m->u.rpl.new_order_ref, g->u64b);
        CHECK_U64X(m->u.rpl.shares, g->u32a);
        CHECK_U64X(m->u.rpl.price_raw, g->u32b);
        break;
    default:
        CHECK(0 && "verifier: unknown OUCH golden type");
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* SBE canonical-Weft-MD golden builder                               */
/* ================================================================== */

typedef struct sbe_golden_entry {
    uint64_t price;
    uint64_t order_ref;
    uint32_t shares;
    uint8_t  buy_sell;
    uint8_t  action;
} sbe_golden_entry_t;

static inline size_t sbe_md_build(uint8_t *o, size_t cap,
                                  uint64_t ts, uint32_t secid, uint32_t seq,
                                  uint8_t flags,
                                  const sbe_golden_entry_t *ent, uint8_t nent,
                                  const char *sym)
{
    size_t symlen = sym ? strlen(sym) : 0;
    size_t total = 8u + 24u + 2u + (size_t)nent * 32u + 1u + symlen;
    uint8_t i;

    CHECK(total <= cap);
    if (total > cap) { return 0; }

    put_le16(o + 0, 24u);      /* block length */
    put_le16(o + 2, 1u);       /* template id  */
    put_le16(o + 4, 0x5746u);  /* schema id    */
    put_le16(o + 6, 0u);       /* version      */
    put_le64(o + 8, ts);
    put_le32(o + 16, secid);
    put_le32(o + 20, seq);
    o[24] = flags;
    memset(o + 25, 0, 7);      /* fixed-block pad */

    o[32] = 32u;               /* group block length */
    o[33] = nent;              /* num in group       */
    for (i = 0; i < nent; ++i) {
        uint8_t *e = o + 34u + (size_t)i * 32u;
        put_le64(e + 0, ent[i].price);
        put_le64(e + 8, ent[i].order_ref);
        put_le32(e + 16, ent[i].shares);
        e[20] = ent[i].buy_sell;
        e[21] = ent[i].action;
        memset(e + 22, 0, 10); /* entry pad */
    }

    o[34u + (size_t)nent * 32u] = (uint8_t)symlen;
    if (symlen > 0u) {
        memcpy(o + 34u + (size_t)nent * 32u + 1u, sym, symlen);
    }
    return total;
}

/* ================================================================== */
/* MoldUDP64 + PCAP construction                                      */
/* ================================================================== */

static inline size_t mold_build_packet(uint8_t *o, size_t cap,
                                       const char *session10, uint32_t seq,
                                       const golden_msg_t *msgs, size_t count)
{
    size_t off = 16, i;
    CHECK(16 + count * 52u <= cap);
    memcpy(o, session10, 10);
    put_be32(o + 10, seq);
    put_be16(o + 14, (uint16_t)count);
    for (i = 0; i < count; ++i) {
        size_t n = itch_encode_msg(o + off + 2, &msgs[i]);
        CHECK(n == weft_itch50_wire_size(msgs[i].type));
        put_be16(o + off, (uint16_t)n);
        off += 2 + n;
    }
    return off;
}

static inline uint16_t ipv4_checksum(const uint8_t *hdr, size_t len)
{
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)hdr[i] << 8) | (uint32_t)hdr[i + 1];
    }
    if (len & 1u) {
        sum += (uint32_t)hdr[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

/* Wrap one UDP payload into Ethernet/IP/UDP + PCAP packet record. */
static inline size_t pcap_wrap_packet(uint8_t *o, size_t cap,
                                      const uint8_t *payload, size_t plen,
                                      uint16_t ip_id, uint32_t ts_sec,
                                      uint32_t ts_usec)
{
    size_t udp_off = 14 + 20;
    size_t pkt_len;
    uint8_t *pk;

    pkt_len = udp_off + 8 + plen;
    CHECK(16u + pkt_len <= cap);

    /* PCAP packet record header comes FIRST in the file */
    put_le32(o + 0, ts_sec);
    put_le32(o + 4, ts_usec);
    put_le32(o + 8, (uint32_t)pkt_len);
    put_le32(o + 12, (uint32_t)pkt_len);

    pk = o + 16;

    /* Ethernet */
    memset(pk, 0, 6);
    memset(pk + 6, 0, 6);
    pk[6] = 0x02;  /* locally administered src MAC */
    pk[12] = 0x08;
    pk[13] = 0x00; /* ethertype IPv4 */

    /* IPv4 */
    pk[14] = 0x45;              /* v4, IHL 5            */
    pk[15] = 0x00;              /* TOS                  */
    put_be16(pk + 16, (uint16_t)(20u + 8u + plen));
    put_be16(pk + 18, ip_id);
    put_be16(pk + 20, 0x4000u); /* DF                   */
    pk[22] = 64;                /* TTL                  */
    pk[23] = 17;                /* protocol UDP         */
    put_be16(pk + 24, 0);       /* checksum (fill next) */
    pk[26] = 10; pk[27] = 0; pk[28] = 0; pk[29] = 1;    /* 10.0.0.1 */
    pk[30] = 10; pk[31] = 0; pk[32] = 0; pk[33] = 2;    /* 10.0.0.2 */
    put_be16(pk + 24, ipv4_checksum(pk + 14, 20));

    /* UDP */
    put_be16(pk + udp_off, 54321u);          /* source port */
    put_be16(pk + udp_off + 2, 40001u);      /* dest port   */
    put_be16(pk + udp_off + 4, (uint16_t)(8u + plen));
    put_be16(pk + udp_off + 6, 0);           /* checksum 0  */
    memcpy(pk + udp_off + 8, payload, plen);

    return 16u + pkt_len;
}

static inline int write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { return -1; }
    if (len != 0u && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static inline long read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    int err;
    if (!f) { return -1; }
    n = fread(buf, 1, cap, f);
    err = ferror(f);
    fclose(f);
    return err ? -1 : (long)n;
}

/* Build and write ALL golden fixtures into dir; also write the
 * checksum sidecar (crc32c + adler32 per file, computed with the
 * pillar's own engine). Deterministic: same bytes every run. */
static inline int golden_write_fixtures(const char *dir)
{
    static uint8_t buf[262144];
    char path[512];
    int rc = 0;

    /* 1. ITCH fixed-framing stream */
    {
        size_t n = itch_golden_build_fixed(buf, sizeof buf);
        snprintf(path, sizeof path, "%s/itch50_golden_fixed.bin", dir);
        rc |= write_file(path, buf, n);
    }
    /* 2. ITCH length-prefixed stream (MoldUDP64 message blocks) */
    {
        size_t n = itch_golden_build_lenprefixed(buf, sizeof buf);
        snprintf(path, sizeof path, "%s/itch50_golden_len.bin", dir);
        rc |= write_file(path, buf, n);
    }
    /* 3. OUCH 5.0 stream */
    {
        size_t n = ouch_golden_build(buf, sizeof buf);
        snprintf(path, sizeof path, "%s/ouch50_golden.bin", dir);
        rc |= write_file(path, buf, n);
    }
    /* 4. SBE golden stream: 3 canonical MD messages */
    {
        static const sbe_golden_entry_t e1[] = {
            { 123450000ull, 0x0100000000000001ull, 300u, 'B', 'A' },
            { 99999999ull,  0x0100000000000002ull, 200u, 'S', 'A' }
        };
        static const sbe_golden_entry_t e2[] = { { 0, 0, 0, 0, 0 } };
        static const sbe_golden_entry_t e3[] = {
            { 456700000ull, 0x0100000000000004ull, 700u, 'S', 'A' },
            { 456700100ull, 0x0100000000000005ull, 100u, 'B', 'A' },
            { 456700200ull, 0x0100000000000006ull, 900u, 'B', 'A' }
        };
        size_t off = 0;
        off += sbe_md_build(buf + off, sizeof buf - off, GOLDEN_TS_BASE,
                            0x00C0FFEEu, 1u, 0x5Au, e1, 2, "AAPL");
        off += sbe_md_build(buf + off, sizeof buf - off,
                            GOLDEN_TS_BASE + 1000, 0x00C0FFEFu, 2u, 0x11u,
                            e2, 0, NULL);
        off += sbe_md_build(buf + off, sizeof buf - off,
                            GOLDEN_TS_BASE + 2000, 0x00C0FFF0u, 3u, 0x22u,
                            e3, 3, "MSFT");
        snprintf(path, sizeof path, "%s/sbe_golden.bin", dir);
        rc |= write_file(path, buf, off);
    }
    /* 5. Golden PCAP: 3 UDP/MoldUDP64 packets carrying the session */
    {
        size_t off = 0;
        size_t n;
        static const char sess[10] = "WEFTGOLDEN";

        /* pcap global header */
        put_le32(buf + 0, 0xA1B2C3D4u);
        put_le16(buf + 4, 2);
        put_le16(buf + 6, 4);
        put_le32(buf + 8, 0);
        put_le32(buf + 12, 0);
        put_le32(buf + 16, 65535u);
        put_le32(buf + 20, 1u);
        off = 24;

        {
            static uint8_t mold[65536];
            n = mold_build_packet(mold, sizeof mold, sess, 1u,
                                  g_golden_itch, 9);
            off += pcap_wrap_packet(buf + off, sizeof buf - off, mold, n,
                                    1u, 1760000000u, 0u);
        }
        {
            static uint8_t mold[65536];
            n = mold_build_packet(mold, sizeof mold, sess, 10u,
                                  g_golden_itch + 9, 8);
            off += pcap_wrap_packet(buf + off, sizeof buf - off, mold, n,
                                    2u, 1760000000u, 1000u);
        }
        {
            static uint8_t mold[65536];
            n = mold_build_packet(mold, sizeof mold, sess, 18u,
                                  g_golden_itch + 17, 6);
            off += pcap_wrap_packet(buf + off, sizeof buf - off, mold, n,
                                    3u, 1760000000u, 2000u);
        }
        snprintf(path, sizeof path, "%s/itch50_golden.pcap", dir);
        rc |= write_file(path, buf, off);
    }
    /* 6. checksum sidecar */
    {
        static const char *files[] = {
            "itch50_golden_fixed.bin", "itch50_golden_len.bin",
            "ouch50_golden.bin", "sbe_golden.bin", "itch50_golden.pcap"
        };
        size_t i;
        FILE *f;
        snprintf(path, sizeof path, "%s/checksums.txt", dir);
        f = fopen(path, "w");
        if (!f) { return -1; }
        for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
            char fpath[512];
            static uint8_t fb[262144];
            long n;
            snprintf(fpath, sizeof fpath, "%s/%s", dir, files[i]);
            n = read_file(fpath, fb, sizeof fb);
            if (n < 0) { fclose(f); return -1; }
            fprintf(f, "%s crc32c=%08x adler32=%08x size=%ld\n",
                    files[i],
                    (unsigned)weft_adapter_crc32c(fb, (size_t)n),
                    (unsigned)weft_adapter_adler32(fb, (size_t)n),
                    n);
        }
        fclose(f);
    }
    return rc;
}

#endif /* WEFT_TEST_UTIL_H */
