/*
 * weft_adapters.h - Weft Pillar 6: weft-adapters frozen C-ABI.
 *
 * Scope   : NASDAQ TotalView-ITCH 5.0 / OUCH 5.0 in-place parsers,
 *           MoldUDP64 message-block walker, SBE (Simple Binary Encoding)
 *           schema-driven transcoder with direct-to-Weft projections,
 *           SIMD checksum & frame-integrity engine (CRC-32C / Adler-32).
 *
 * Laws (Pillar 6 directive, binding on this entire header):
 *   Law 1  ZERO-ALLOCATION ON PARSE & TRANSCODE
 *          No entry point below performs heap allocation (malloc/calloc/
 *          realloc/new). All decode state lives in caller-provided storage
 *          (stack structs or statically aligned arenas). Wire data is
 *          parsed in place; only bounded scalar transcodes into 64-byte
 *          cache-line projections occur.
 *   Law 2  BIT-EXACT 64-BYTE CACHE-LINE PACKING
 *          Every transcoded financial message maps to weft_itch_msg_t /
 *          weft_ouch_msg_t: exactly 64 bytes, natural alignment for all
 *          primitive scalar fields, explicit padding, and _Static_assert
 *          checks on every struct size and field offset (see the frozen
 *          assert table below - ABI is byte-frozen for FFI binding).
 *          Batches are declared with WEFT_ALIGNED64 to guarantee the
 *          64-byte stride lands on cache-line boundaries.
 *   Law 3  CROSS-ARCHITECTURE PORTABILITY
 *          Compiles clean under  gcc/clang  -std=c11 -Wall -Wextra
 *          -Werror -pedantic  on x86_64 (SSE4.2/AVX2) and aarch64
 *          (ARMv8 CRC32/NEON). Wire loads use memcpy() (never cast
 *          dereferences), so no misaligned 64-bit loads are ever emitted.
 *   Law 4  STRICT FROZEN C-ABI
 *          This header is the single source of truth. Engineer 2 (native
 *          rmw_weft / Vision DMA) and Engineer 3 (managed SDKs / L3 order
 *          book UI) bind directly against these tags, structs, error
 *          codes and signatures. Field offsets are ABI; do not reorder.
 *
 * ABI version : 1 (WEFT_ADAPTERS_ABI_VERSION)
 * Normative   : docs/reports/D-61-ADAPTERS-CORE-AUDIT.md
 *
 * Conventions:
 *   - All multi-byte ITCH/OUCH/MoldUDP64 wire fields are BIG-endian.
 *   - All SBE wire fields (message header, blocks, groups, var-lengths)
 *     are LITTLE-endian per the SBE default encoding.
 *   - All decoded values in structs are HOST-endian.
 *   - Prices are kept as raw integer ticks (4 decimal places, i.e. the
 *     wire value); use weft_itch_price_d() for display floating point.
 *   - Every function is fail-closed: negative weft_adapter_status on any
 *     anomaly, zero partial state on refusal paths.
 *
 * Weft Pillar 6 (weft-adapters). Frozen ABI; changes require a new
 * WEFT_ADAPTERS_ABI_VERSION and a migration note in the D-report.
 */
#ifndef WEFT_ADAPTERS_H
#define WEFT_ADAPTERS_H

#include <stddef.h>   /* size_t, offsetof */
#include <stdint.h>   /* fixed-width integers */
#include <string.h>   /* memcmp/memcpy for inline helpers */
#include <stdbool.h>  /* bool (C99+, C++ native) */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Portability shims (keep the header includable from C++ FFI tooling) */
/* ------------------------------------------------------------------ */

#if defined(__cplusplus)
#  define WEFT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#  define WEFT_ALIGNED64               alignas(64)
#  define WEFT_ALIGNOF(t)              alignof(t)
#  define WEFT_RESTRICT
#else
#  define WEFT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  define WEFT_ALIGNED64               _Alignas(64)
#  define WEFT_ALIGNOF(t)              _Alignof(t)
#  define WEFT_RESTRICT                 restrict
#endif

#define WEFT_ADAPTERS_ABI_VERSION 1

/* ================================================================== */
/* SECTION 1 - STATUS LADDER (fail-closed, negative on anomaly)       */
/* ================================================================== */

enum weft_adapter_status {
    WEFT_ADAPTER_OK      =  0, /* success                                     */
    WEFT_ADAPTER_EINVAL  = -1, /* NULL args / invalid flags                  */
    WEFT_ADAPTER_EBADMSG = -2, /* unknown type, invalid field, bad framing   */
    WEFT_ADAPTER_ETRUNC  = -3, /* buffer shorter than the message            */
    WEFT_ADAPTER_EALIGN  = -4, /* output projection alignment violation      */
    WEFT_ADAPTER_ENOSUP  = -5, /* known message type, unsupported here       */
    WEFT_ADAPTER_ECRC    = -6, /* checksum / frame integrity failure          */
    WEFT_ADAPTER_ESCHEMA = -7, /* SBE schema/descriptor violation            */
    WEFT_ADAPTER_EBOUNDS = -8  /* SBE group/var capacity or bounds violation  */
};

/* Runtime ABI probe for managed bindings (Engineer 3). */
int weft_adapters_abi_version(void);

/* ================================================================== */
/* SECTION 2 - NASDAQ TotalView-ITCH 5.0                              */
/* ================================================================== */

/* Message type tags. Value == wire message-type byte (ASCII). */
enum weft_itch_tag {
    WEFT_ITCH_TAG_SYSTEM_EVENT        = 0x53, /* 'S' 12B  mandatory */
    WEFT_ITCH_TAG_STOCK_DIRECTORY     = 0x52, /* 'R' 39B  mandatory */
    WEFT_ITCH_TAG_STOCK_TRADING_ACTION= 0x48, /* 'H' 25B  mandatory */
    WEFT_ITCH_TAG_REG_SHO             = 0x59, /* 'Y' 20B  mandatory */
    WEFT_ITCH_TAG_MARKET_PARTICIPANT  = 0x4C, /* 'L' 26B  extended  */
    WEFT_ITCH_TAG_MWCB_DECLINE        = 0x56, /* 'V' 35B  extended  */
    WEFT_ITCH_TAG_MWCB_STATUS         = 0x57, /* 'W' 12B  extended  */
    WEFT_ITCH_TAG_IPO_QUOTING         = 0x4B, /* 'K' 28B  extended  */
    WEFT_ITCH_TAG_LULD_COLLAR         = 0x4A, /* 'J' 47B  extended  */
    WEFT_ITCH_TAG_ADD_ORDER           = 0x41, /* 'A' 36B  mandatory */
    WEFT_ITCH_TAG_ADD_ORDER_MPID      = 0x46, /* 'F' 40B  mandatory */
    WEFT_ITCH_TAG_ORDER_EXECUTED      = 0x45, /* 'E' 31B  mandatory */
    WEFT_ITCH_TAG_ORDER_EXEC_PRICE    = 0x43, /* 'C' 36B  mandatory */
    WEFT_ITCH_TAG_ORDER_CANCEL        = 0x58, /* 'X' 23B  mandatory */
    WEFT_ITCH_TAG_ORDER_DELETE        = 0x44, /* 'D' 19B  mandatory */
    WEFT_ITCH_TAG_ORDER_REPLACE       = 0x55, /* 'U' 35B  mandatory */
    WEFT_ITCH_TAG_TRADE               = 0x50, /* 'P' 44B  mandatory */
    WEFT_ITCH_TAG_CROSS_TRADE         = 0x51, /* 'Q' 40B  mandatory */
    WEFT_ITCH_TAG_NOII                = 0x49  /* 'I' 50B  mandatory */
};

/* Decode behaviour flags (default 0 = strict fixed-framing). */
enum weft_itch_flags {
    /* Stream carries a 2-byte BIG-endian length prefix before every
     * message (MoldUDP64 message-block framing). Consumed bytes include
     * the prefix. Strict policy: prefix must equal the oracle size. */
    WEFT_ITCH_F_LENPREFIX = 0x1u,
    /* Lenient: with LENPREFIX, accept prefix >= oracle size (decode the
     * oracle fields, consume 2+prefix) instead of failing EBADMSG on
     * oversized prefixes. Field-level validation stays ON. */
    WEFT_ITCH_F_LENIENT   = 0x2u
};

/* Strict timestamp ceiling: nanoseconds in one trading day. */
#define WEFT_ITCH_NS_PER_DAY 86400000000000ull

/* Wire-size oracle: exact ITCH 5.0 message size (incl. 11-byte header)
 * for a known type, 0 for unknown types. Pure table lookup. */
size_t weft_itch50_wire_size(uint8_t msg_type);

/* ------------------------------------------------------------------ */
/* ITCH projections - 16-byte common header + 48-byte body = 64 bytes */
/* ------------------------------------------------------------------ */

/* Common ITCH 5.0 header (wire: type@0, locate@1(BE16), tracking@3(BE16),
 * timestamp@5(BE48)). Projection decodes to host order. */
typedef struct weft_itch_hdr {
    uint8_t  msg_type;        /* +0  wire type byte (== weft_itch_tag)   */
    uint8_t  _pad0;           /* +1  explicit padding                    */
    uint16_t stock_locate;    /* +2  host order                          */
    uint16_t tracking_number; /* +4  host order                          */
    uint8_t  _pad1[2];        /* +6  explicit padding                    */
    uint64_t timestamp_ns;    /* +8  6-byte BE ns-since-midnight, host   */
} weft_itch_hdr_t;            /* = 16                                    */

WEFT_STATIC_ASSERT(sizeof(weft_itch_hdr_t) == 16, "itch hdr size 16");
WEFT_STATIC_ASSERT(offsetof(weft_itch_hdr_t, msg_type)        == 0,  "itch hdr off 0");
WEFT_STATIC_ASSERT(offsetof(weft_itch_hdr_t, stock_locate)    == 2,  "itch hdr off 2");
WEFT_STATIC_ASSERT(offsetof(weft_itch_hdr_t, tracking_number) == 4,  "itch hdr off 4");
WEFT_STATIC_ASSERT(offsetof(weft_itch_hdr_t, timestamp_ns)    == 8,  "itch hdr off 8");

/* 'S' System Event (wire 12). */
typedef struct weft_itch_system_body {
    uint8_t event_code;       /* +0  'O','S','Q','M','E','C'             */
    uint8_t _pad0[47];
} weft_itch_system_body_t;

/* 'R' Stock Directory (wire 39). */
typedef struct weft_itch_stock_directory_body {
    uint8_t  stock[8];              /* +0  space-padded symbol           */
    uint8_t  market_category;       /* +8                                */
    uint8_t  financial_status;      /* +9                                */
    uint16_t _pad0;                 /* +10 explicit padding              */
    uint32_t round_lot_size;        /* +12                               */
    uint8_t  round_lots_only;       /* +16                               */
    uint8_t  issue_classification;  /* +17                               */
    uint8_t  issue_subtype[2];      /* +18                               */
    uint8_t  authenticity;          /* +20                               */
    uint8_t  short_sale_threshold;  /* +21                               */
    uint8_t  ipo_flag;              /* +22                               */
    uint8_t  luld_tier;             /* +23                               */
    uint8_t  etp_flag;              /* +24                               */
    uint8_t  _pad1[3];              /* +25 explicit padding              */
    uint32_t etp_leverage_factor;   /* +28                               */
    uint8_t  inverse_indicator;     /* +32                               */
    uint8_t  _pad2[15];             /* +33 explicit padding              */
} weft_itch_stock_directory_body_t;

/* 'H' Stock Trading Action (wire 25). */
typedef struct weft_itch_trading_action_body {
    uint8_t stock[8];         /* +0                                       */
    uint8_t trading_state;    /* +8  'H','P','Q','T'                      */
    uint8_t reserved;         /* +9                                       */
    uint8_t reason[4];        /* +10 ASCII                                */
    uint8_t _pad0[34];
} weft_itch_trading_action_body_t;

/* 'Y' Reg SHO Restriction (wire 20). */
typedef struct weft_itch_reg_sho_body {
    uint8_t stock[8];         /* +0                                       */
    uint8_t reg_sho_action;   /* +8  '0','1','2'                          */
    uint8_t _pad0[39];
} weft_itch_reg_sho_body_t;

/* 'L' Market Participant Position (wire 26). */
typedef struct weft_itch_market_participant_body {
    uint8_t mpid[4];          /* +0                                       */
    uint8_t stock[8];         /* +4  byte array - no alignment constraint */
    uint8_t primary_mm;       /* +12 'Y','N'                              */
    uint8_t mm_mode;          /* +13 'N','P','S','M'                      */
    uint8_t participant_state;/* +14 'A','E','W','S','D','X'              */
    uint8_t _pad0[33];
} weft_itch_market_participant_body_t;

/* 'A' Add Order (wire 36) and 'F' Add Order with MPID (wire 40). */
typedef struct weft_itch_add_order_body {
    uint64_t order_ref;       /* +0                                       */
    uint8_t  buy_sell;        /* +8  'B' buy / 'S' sell                   */
    uint8_t  _pad0[3];        /* +9  explicit padding                     */
    uint32_t shares;          /* +12                                      */
    uint8_t  stock[8];        /* +16                                      */
    uint32_t price_raw;       /* +24 4-decimal price ticks                */
    uint8_t  _pad1[20];       /* +28 explicit padding                     */
} weft_itch_add_order_body_t;

typedef struct weft_itch_add_order_mpid_body {
    uint64_t order_ref;       /* +0                                       */
    uint8_t  buy_sell;        /* +8                                       */
    uint8_t  _pad0[3];        /* +9                                       */
    uint32_t shares;          /* +12                                      */
    uint8_t  stock[8];        /* +16                                      */
    uint32_t price_raw;       /* +24                                      */
    uint8_t  mpid[4];         /* +28                                      */
    uint8_t  _pad1[16];       /* +32 explicit padding                     */
} weft_itch_add_order_mpid_body_t;

/* 'E' Order Executed (wire 31). */
typedef struct weft_itch_order_executed_body {
    uint64_t order_ref;       /* +0                                       */
    uint64_t match_number;    /* +8                                       */
    uint32_t executed_shares; /* +16                                      */
    uint8_t  _pad0[28];
} weft_itch_order_executed_body_t;

/* 'C' Order Executed With Price (wire 36). */
typedef struct weft_itch_order_exec_price_body {
    uint64_t order_ref;       /* +0                                       */
    uint64_t match_number;    /* +8                                       */
    uint32_t executed_shares; /* +16                                      */
    uint32_t execution_price; /* +20 4-decimal ticks                      */
    uint8_t  printable;       /* +24 'N','Y'                              */
    uint8_t  _pad0[23];
} weft_itch_order_exec_price_body_t;

/* 'X' Order Cancel (wire 23). */
typedef struct weft_itch_order_cancel_body {
    uint64_t order_ref;       /* +0                                       */
    uint32_t cancelled_shares;/* +8                                       */
    uint8_t  _pad0[36];
} weft_itch_order_cancel_body_t;

/* 'D' Order Delete (wire 19). */
typedef struct weft_itch_order_delete_body {
    uint64_t order_ref;       /* +0                                       */
    uint8_t  _pad0[40];
} weft_itch_order_delete_body_t;

/* 'U' Order Replace (wire 35). */
typedef struct weft_itch_order_replace_body {
    uint64_t original_order_ref; /* +0                                    */
    uint64_t new_order_ref;      /* +8                                    */
    uint32_t shares;             /* +16                                   */
    uint32_t price_raw;          /* +20                                   */
    uint8_t  _pad0[24];
} weft_itch_order_replace_body_t;

/* 'P' Trade - non-cross (wire 44). */
typedef struct weft_itch_trade_body {
    uint64_t order_ref;       /* +0                                       */
    uint64_t match_number;    /* +8                                       */
    uint8_t  stock[8];        /* +16                                      */
    uint32_t shares;          /* +24                                      */
    uint32_t price_raw;       /* +28                                      */
    uint8_t  buy_sell;        /* +32 'B','S'                              */
    uint8_t  _pad0[15];
} weft_itch_trade_body_t;

/* 'Q' Cross Trade (wire 40). */
typedef struct weft_itch_cross_trade_body {
    uint64_t shares;          /* +0  8-byte cross size                    */
    uint64_t match_number;    /* +8                                       */
    uint8_t  stock[8];        /* +16                                      */
    uint32_t cross_price_raw; /* +24                                      */
    uint8_t  cross_type;      /* +28 'O','C','H','I'                      */
    uint8_t  _pad0[19];
} weft_itch_cross_trade_body_t;

/* 'I' Net Order Imbalance Indicator (wire 50). */
typedef struct weft_itch_noii_body {
    uint64_t paired_shares;        /* +0                                 */
    uint64_t imbalance_shares;     /* +8                                 */
    uint8_t  stock[8];             /* +16                                */
    uint32_t far_price_raw;        /* +24                                */
    uint32_t near_price_raw;       /* +28                                */
    uint32_t current_ref_price_raw;/* +32                                */
    uint8_t  imbalance_direction;  /* +36 'B','S','N','O'                */
    uint8_t  cross_type;           /* +37 'O','C','H','I'                */
    uint8_t  price_variation;      /* +38 'L','M','H','C','O'            */
    uint8_t  _pad0[9];
} weft_itch_noii_body_t;

/* 'V' MWCB Decline Level (wire 35). */
typedef struct weft_itch_mwcb_body {
    uint64_t level1;          /* +0                                       */
    uint64_t level2;          /* +8                                       */
    uint64_t level3;          /* +16                                      */
    uint8_t  _pad0[24];
} weft_itch_mwcb_body_t;

/* 'W' MWCB Status (wire 12). */
typedef struct weft_itch_mwcb_status_body {
    uint8_t breached_level;   /* +0  '1','2','3'                          */
    uint8_t _pad0[47];
} weft_itch_mwcb_status_body_t;

/* 'K' IPO Quoting Period Update (wire 28). */
typedef struct weft_itch_ipo_body {
    uint8_t  stock[8];        /* +0                                       */
    uint32_t release_time;    /* +8  seconds since midnight              */
    uint8_t  release_qualifier;/* +12 'A','B','C','O','P','Q','R','S'     */
    uint8_t  _pad0[3];        /* +13 explicit padding                     */
    uint32_t ipo_price_raw;   /* +16                                      */
    uint8_t  _pad1[28];
} weft_itch_ipo_body_t;

/* 'J' LULD Auction Collar (wire 47). */
typedef struct weft_itch_luld_body {
    uint8_t  stock[8];        /* +0                                       */
    uint64_t reference_price; /* +8                                       */
    uint64_t upper_collar;    /* +16                                      */
    uint64_t lower_collar;    /* +24                                      */
    uint32_t extension;       /* +32 seconds                              */
    uint8_t  _pad0[12];
} weft_itch_luld_body_t;

/* Frozen assert table: every body is exactly 48 bytes, naturally aligned. */
WEFT_STATIC_ASSERT(sizeof(weft_itch_system_body_t)             == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_stock_directory_body_t)    == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_trading_action_body_t)     == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_reg_sho_body_t)            == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_market_participant_body_t) == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_add_order_body_t)          == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_add_order_mpid_body_t)     == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_order_executed_body_t)     == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_order_exec_price_body_t)   == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_order_cancel_body_t)       == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_order_delete_body_t)       == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_order_replace_body_t)      == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_trade_body_t)              == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_cross_trade_body_t)        == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_noii_body_t)               == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_mwcb_body_t)               == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_mwcb_status_body_t)        == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_ipo_body_t)                == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_itch_luld_body_t)               == 48, "body 48");

/* Scalar natural-alignment proofs (spot pins per struct, per Law 2). */
WEFT_STATIC_ASSERT(offsetof(weft_itch_add_order_body_t, order_ref)  == 0,  "align u64 @0");
WEFT_STATIC_ASSERT(offsetof(weft_itch_add_order_body_t, shares)    == 12, "align u32 @12");
WEFT_STATIC_ASSERT(offsetof(weft_itch_add_order_body_t, price_raw) == 24, "align u32 @24");
WEFT_STATIC_ASSERT(offsetof(weft_itch_add_order_mpid_body_t, mpid) == 28, "mpid @28");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_executed_body_t, match_number)      == 8,  "align u64 @8");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_executed_body_t, executed_shares)   == 16, "align u32 @16");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_exec_price_body_t, execution_price) == 20, "align u32 @20");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_cancel_body_t, cancelled_shares)    == 8,  "align u32 @8");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_replace_body_t, new_order_ref)      == 8,  "align u64 @8");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_replace_body_t, shares)             == 16, "align u32 @16");
WEFT_STATIC_ASSERT(offsetof(weft_itch_order_replace_body_t, price_raw)          == 20, "align u32 @20");
WEFT_STATIC_ASSERT(offsetof(weft_itch_trade_body_t, match_number)  == 8,  "align u64 @8");
WEFT_STATIC_ASSERT(offsetof(weft_itch_trade_body_t, shares)        == 24, "align u32 @24");
WEFT_STATIC_ASSERT(offsetof(weft_itch_trade_body_t, price_raw)     == 28, "align u32 @28");
WEFT_STATIC_ASSERT(offsetof(weft_itch_cross_trade_body_t, shares)  == 0,  "align u64 @0");
WEFT_STATIC_ASSERT(offsetof(weft_itch_cross_trade_body_t, cross_price_raw) == 24, "align u32 @24");
WEFT_STATIC_ASSERT(offsetof(weft_itch_noii_body_t, paired_shares)  == 0,  "align u64 @0");
WEFT_STATIC_ASSERT(offsetof(weft_itch_noii_body_t, far_price_raw)  == 24, "align u32 @24");
WEFT_STATIC_ASSERT(offsetof(weft_itch_noii_body_t, current_ref_price_raw) == 32, "align u32 @32");
WEFT_STATIC_ASSERT(offsetof(weft_itch_mwcb_body_t, level1)         == 0,  "align u64 @0");
WEFT_STATIC_ASSERT(offsetof(weft_itch_mwcb_body_t, level3)         == 16, "align u64 @16");
WEFT_STATIC_ASSERT(offsetof(weft_itch_stock_directory_body_t, round_lot_size)    == 12, "align u32 @12");
WEFT_STATIC_ASSERT(offsetof(weft_itch_stock_directory_body_t, etp_leverage_factor) == 28, "align u32 @28");
WEFT_STATIC_ASSERT(offsetof(weft_itch_ipo_body_t, release_time)    == 8,  "align u32 @8");
WEFT_STATIC_ASSERT(offsetof(weft_itch_ipo_body_t, ipo_price_raw)   == 16, "align u32 @16");
WEFT_STATIC_ASSERT(offsetof(weft_itch_luld_body_t, reference_price)== 8,  "align u64 @8");
WEFT_STATIC_ASSERT(offsetof(weft_itch_luld_body_t, extension)      == 32, "align u32 @32");

/* The frozen 64-byte ITCH projection. Union body offset 16, size 64,
 * natural alignment 8; batches use WEFT_ALIGNED64 for the 64B stride. */
typedef struct weft_itch_msg {
    weft_itch_hdr_t hdr;                       /* +0  .. +15            */
    union {                                    /* +16 .. +63            */
        weft_itch_system_body_t             sys;
        weft_itch_stock_directory_body_t    dir;
        weft_itch_trading_action_body_t     act;
        weft_itch_reg_sho_body_t            sho;
        weft_itch_market_participant_body_t par;
        weft_itch_add_order_body_t          add;
        weft_itch_add_order_mpid_body_t     addm;
        weft_itch_order_executed_body_t     exe;
        weft_itch_order_exec_price_body_t   exp;
        weft_itch_order_cancel_body_t       cxl;
        weft_itch_order_delete_body_t       del;
        weft_itch_order_replace_body_t      rpl;
        weft_itch_trade_body_t              trd;
        weft_itch_cross_trade_body_t        crs;
        weft_itch_noii_body_t               noi;
        weft_itch_mwcb_body_t               mwb;
        weft_itch_mwcb_status_body_t        mws;
        weft_itch_ipo_body_t                ipo;
        weft_itch_luld_body_t               lul;
    } u;
} weft_itch_msg_t;

WEFT_STATIC_ASSERT(sizeof(weft_itch_msg_t)    == 64, "itch msg size 64 (cache line)");
WEFT_STATIC_ASSERT(offsetof(weft_itch_msg_t, hdr) == 0,  "itch hdr @0");
WEFT_STATIC_ASSERT(offsetof(weft_itch_msg_t, u)   == 16, "itch body @16");
WEFT_STATIC_ASSERT(WEFT_ALIGNOF(weft_itch_msg_t) == 8,  "itch natural align 8");
WEFT_STATIC_ASSERT(sizeof(weft_itch_msg_t) % 64 == 0, "itch 64B batch stride");

/* ================================================================== */
/* SECTION 3 - ITCH 5.0 decode API                                    */
/* ================================================================== */

/* Decode ONE message from buf[0..avail).
 *
 *   out       : 64B projection destination (natural 8B alignment minimum).
 *   msg_size  : optional out, total bytes consumed (incl. length prefix
 *               when WEFT_ITCH_F_LENPREFIX).
 *   returns   : WEFT_ADAPTER_OK or negative status. On any negative
 *               status the projection is left untouched (fail-closed).
 *
 * Strict (flags=0) policy: unknown type -> EBADMSG; wire size enforced
 * by the oracle table; timestamp must be < 24h; side/state/action chars
 * validated per spec; symbol bytes must be printable ASCII. */
int32_t weft_itch50_decode_one(const uint8_t *WEFT_RESTRICT buf,
                               size_t avail,
                               uint32_t flags,
                               weft_itch_msg_t *WEFT_RESTRICT out,
                               size_t *msg_size);

/* Decode a stream of back-to-back messages into a 64B-aligned batch.
 *
 *   out       : batch array, MUST be 64-byte aligned (WEFT_ALIGNED64 or
 *               aligned_alloc(64, n*64)) -> else WEFT_ADAPTER_EALIGN.
 *   out_max   : batch capacity; decoding stops cleanly at capacity with
 *               status OK and *consumed = bytes fully decoded.
 *   status    : first error encountered (WEFT_ADAPTER_OK on success);
 *               on error, *consumed = offset of the offending message
 *               and the return value counts messages decoded BEFORE
 *               the failure (caller may trust those, fail-closed).
 *   returns   : number of decoded messages (0..out_max).
 */
size_t weft_itch50_decode_batch(const uint8_t *WEFT_RESTRICT buf,
                                size_t len,
                                uint32_t flags,
                                weft_itch_msg_t *WEFT_RESTRICT out,
                                size_t out_max,
                                int32_t *status,
                                size_t *consumed);

/* Inline helpers (zero-call, FFI-safe arithmetic). */
static inline double weft_itch_price_d(uint32_t price_raw)
{
    return (double)price_raw / 10000.0; /* 4-decimal ITCH price ticks */
}

static inline int weft_itch_stock_eq(const uint8_t stock[8], const char *sym)
{
    size_t i = 0;
    while (i < 8u && sym[i] != '\0') {
        if (stock[i] != (uint8_t)sym[i]) { return 0; }
        ++i;
    }
    while (i < 8u) {
        if (stock[i] != (uint8_t)' ') { return 0; }
        ++i;
    }
    return 1;
}

/* ================================================================== */
/* SECTION 4 - MoldUDP64 downstream packet walker                     */
/* ================================================================== */

typedef struct weft_mold_view {
    uint8_t        session[10];  /* 10-byte session id                    */
    uint32_t       sequence;     /* first sequence number in packet       */
    uint16_t       count;        /* message blocks declared in packet     */
    uint16_t       next_index;   /* blocks consumed so far                */
    const uint8_t *blocks;       /* start of message blocks (after 16B)   */
    const uint8_t *cursor;       /* current read cursor                   */
    const uint8_t *end;          /* one-past-packet                       */
} weft_mold_view_t;

/* Open a MoldUDP64 downstream packet: 10B session + 4B BE sequence +
 * 2B BE message count, then message blocks. Validates the 16-byte
 * header and that the declared block stream fits in len. */
int32_t weft_mold64_open(const uint8_t *pkt, size_t len,
                         weft_mold_view_t *view);

/* Next message block: 2B BE length prefix + message bytes.
 * Returns 1 and sets msg_out/len_out on success; 0 when the packet is
 * exhausted (or a zero-length end-of-session block is hit); negative
 * status (ETRUNC/EBADMSG) on malformed framing. Zero-copy: *msg_out
 * points into the source packet. */
int32_t weft_mold64_next(weft_mold_view_t *view,
                         const uint8_t **msg_out, uint16_t *len_out);

/* ================================================================== */
/* SECTION 5 - NASDAQ OUCH 5.0 (core subset, pinned layouts)          */
/* ================================================================== */

/* Outbound (market -> client) messages supported by this pillar.
 * Wire framing: 2-byte BE length prefix (SoupBinTCP payload) +
 * 1-byte type + body. The length prefix INCLUDES the type byte.
 *
 * NOTE (declared boundary): layouts below are pinned per the NASDAQ
 * OUCH 5.0 spec as cross-referenced for this pillar. Spec-sheet
 * cross-validation against an official capture is the declared
 * follow-up for Engineer 2's rmw_weft integration seam. */
enum weft_ouch_tag {
    WEFT_OUCH_TAG_SYSTEM_EVENT     = 0x53, /* 'S' wire  8 */
    WEFT_OUCH_TAG_ORDER_ACCEPTED   = 0x41, /* 'A' wire 46 */
    WEFT_OUCH_TAG_ORDER_EXECUTED   = 0x45, /* 'E' wire 27 */
    WEFT_OUCH_TAG_ORDER_EXEC_PRICE = 0x43, /* 'C' wire 32 */
    WEFT_OUCH_TAG_ORDER_CANCELED   = 0x58, /* 'X' wire 19 */
    WEFT_OUCH_TAG_ORDER_REPLACED   = 0x55  /* 'U' wire 31 */
};

typedef struct weft_ouch_hdr {
    uint64_t timestamp_ns;    /* +0  6-byte BE ns-since-midnight, host   */
    uint8_t  msg_type;        /* +8  == weft_ouch_tag                    */
    uint8_t  _pad0[7];        /* +9                                       */
} weft_ouch_hdr_t;            /* = 16                                     */

WEFT_STATIC_ASSERT(sizeof(weft_ouch_hdr_t) == 16, "ouch hdr size 16");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_hdr_t, timestamp_ns) == 0, "ouch hdr off 0");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_hdr_t, msg_type) == 8, "ouch hdr off 8");

typedef struct weft_ouch_system_body {
    uint8_t event_code;       /* 'S','E','C'                              */
    uint8_t _pad0[47];
} weft_ouch_system_body_t;

typedef struct weft_ouch_accepted_body {
    uint64_t order_ref;       /* +0                                       */
    uint32_t shares;          /* +8                                       */
    uint32_t price_raw;       /* +12 4-decimal ticks                      */
    uint8_t  stock[8];        /* +16                                      */
    uint8_t  buy_sell;        /* +24 'B','S'                              */
    uint8_t  time_in_force[4];/* +25 "DAY","IOC","FAK"...                 */
    uint8_t  firm[4];         /* +29                                      */
    uint8_t  display;         /* +33 'Y','N'                              */
    uint8_t  order_state;     /* +34 'L','R','D','S'                      */
    uint8_t  capacity;        /* +35 'A','P','R','M'                      */
    uint8_t  intermarket_sweep; /* +36 'Y','N'                            */
    uint8_t  cross_type;      /* +37 'O','C','H','I'                      */
    uint8_t  customer_type;   /* +38                                      */
    uint8_t  _pad0[9];
} weft_ouch_accepted_body_t;

typedef struct weft_ouch_executed_body {
    uint64_t order_ref;       /* +0                                       */
    uint64_t match_number;    /* +8                                       */
    uint32_t executed_shares; /* +16                                      */
    uint8_t  _pad0[28];
} weft_ouch_executed_body_t;

typedef struct weft_ouch_exec_price_body {
    uint64_t order_ref;       /* +0                                       */
    uint64_t match_number;    /* +8                                       */
    uint32_t executed_shares; /* +16                                      */
    uint32_t execution_price; /* +20                                      */
    uint8_t  printable;       /* +24 'Y','N'                              */
    uint8_t  _pad0[23];
} weft_ouch_exec_price_body_t;

typedef struct weft_ouch_canceled_body {
    uint64_t order_ref;       /* +0                                       */
    uint32_t cancelled_shares;/* +8                                       */
    uint8_t  _pad0[36];
} weft_ouch_canceled_body_t;

typedef struct weft_ouch_replaced_body {
    uint64_t original_order_ref; /* +0                                    */
    uint64_t new_order_ref;      /* +8                                    */
    uint32_t shares;             /* +16                                   */
    uint32_t price_raw;          /* +20                                   */
    uint8_t  _pad0[24];
} weft_ouch_replaced_body_t;

WEFT_STATIC_ASSERT(sizeof(weft_ouch_system_body_t)      == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_ouch_accepted_body_t)    == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_ouch_executed_body_t)    == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_ouch_exec_price_body_t)  == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_ouch_canceled_body_t)    == 48, "body 48");
WEFT_STATIC_ASSERT(sizeof(weft_ouch_replaced_body_t)    == 48, "body 48");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_accepted_body_t, price_raw)  == 12, "align u32 @12");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_accepted_body_t, stock)      == 16, "stock @16");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_accepted_body_t, firm)       == 29, "firm @29");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_executed_body_t, match_number) == 8, "align u64 @8");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_exec_price_body_t, execution_price) == 20, "align u32 @20");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_replaced_body_t, price_raw)  == 20, "align u32 @20");

typedef struct weft_ouch_msg {
    weft_ouch_hdr_t hdr;                    /* +0  .. +15               */
    union {                                 /* +16 .. +63               */
        weft_ouch_system_body_t     sys;
        weft_ouch_accepted_body_t   acc;
        weft_ouch_executed_body_t   exe;
        weft_ouch_exec_price_body_t exp;
        weft_ouch_canceled_body_t   cxl;
        weft_ouch_replaced_body_t   rpl;
    } u;
} weft_ouch_msg_t;

WEFT_STATIC_ASSERT(sizeof(weft_ouch_msg_t)  == 64, "ouch msg size 64 (cache line)");
WEFT_STATIC_ASSERT(offsetof(weft_ouch_msg_t, u) == 16, "ouch body @16");
WEFT_STATIC_ASSERT(sizeof(weft_ouch_msg_t) % 64 == 0, "ouch 64B batch stride");

/* Wire-size oracle for OUCH 5.0 message bodies (incl. type byte,
 * excl. the 2-byte length prefix). 0 for unsupported types. */
size_t weft_ouch50_wire_size(uint8_t msg_type);

/* Decode one length-prefixed OUCH message. Identical fail-closed
 * contract as weft_itch50_decode_one. */
int32_t weft_ouch50_decode_one(const uint8_t *WEFT_RESTRICT buf,
                               size_t avail,
                               weft_ouch_msg_t *WEFT_RESTRICT out,
                               size_t *msg_size);

/* Batch variant over a SoupBinTCP payload stream. Same contract as
 * weft_itch50_decode_batch (out MUST be 64-byte aligned). */
size_t weft_ouch50_decode_batch(const uint8_t *WEFT_RESTRICT buf,
                                size_t len,
                                weft_ouch_msg_t *WEFT_RESTRICT out,
                                size_t out_max,
                                int32_t *status,
                                size_t *consumed);

/* ================================================================== */
/* SECTION 6 - SBE (Simple Binary Encoding) transcoder                */
/* ================================================================== */

/* Decoder capacity limits (frozen; exceeding them fails closed with
 * WEFT_ADAPTER_ESCHEMA / WEFT_ADAPTER_EBOUNDS). */
#define WEFT_SBE_MAX_FIXED         16
#define WEFT_SBE_MAX_GROUPS         4
#define WEFT_SBE_MAX_VAR            4
#define WEFT_SBE_MAX_GROUP_FIELDS   8
#define WEFT_SBE_MAX_GROUP_ENTRIES 4096

/* Scalar field kinds. */
enum weft_sbe_kind {
    WEFT_SBE_K_U8    = 1,
    WEFT_SBE_K_U16   = 2,
    WEFT_SBE_K_U32   = 3,
    WEFT_SBE_K_U64   = 4,
    WEFT_SBE_K_I8    = 5,
    WEFT_SBE_K_I16   = 6,
    WEFT_SBE_K_I32   = 7,
    WEFT_SBE_K_I64   = 8,
    WEFT_SBE_K_CHARS = 9   /* fixed-size byte array, zero-copy slice     */
};

/* Field descriptor: offset/size/kind within a fixed block or group
 * entry. `slot` is the destination index in weft_sbe_view_t::scalar[]
 * for fixed-block fields; MUST be 0 for group-entry fields (validated). */
typedef struct weft_sbe_field_desc {
    uint16_t offset;   /* within block (fixed) or entry (group)          */
    uint16_t size;     /* 1/2/4/8; N for CHARS                           */
    uint16_t kind;     /* enum weft_sbe_kind                             */
    uint16_t slot;     /* scalar slot (fixed fields only)                */
} weft_sbe_field_desc_t; /* = 8                                          */

/* Repeating-group descriptor.
 * Weft pins the SBE group dimension encoding to: u8 block_length +
 * u8 num_in_group (2 bytes, wire order), a legal SBE dimension
 * encoding; schemas using other dimension encodings must transcode at
 * the seam (declared in D-61 §2). */
typedef struct weft_sbe_group_desc {
    uint16_t block_length;  /* per-entry size declared by schema         */
    uint16_t num_fields;    /* fields per entry                           */
    uint16_t first_field;   /* index into schema->group_fields           */
    uint16_t _pad0;
} weft_sbe_group_desc_t;   /* = 8                                          */

/* Frozen schema descriptor (compile-time const, zero-alloc by design). */
typedef struct weft_sbe_schema {
    uint16_t template_id;
    uint16_t schema_id;
    uint16_t version;
    uint16_t block_length;      /* fixed block size                       */
    uint16_t num_fixed;
    uint16_t num_groups;
    uint16_t num_var;           /* trailing var-length fields             */
    uint16_t _pad0;
    const weft_sbe_field_desc_t *fixed;        /* num_fixed entries       */
    const weft_sbe_group_desc_t *groups;       /* num_groups entries      */
    const weft_sbe_field_desc_t *group_fields; /* flattened, in order     */
} weft_sbe_schema_t;       /* = 40                                          */

WEFT_STATIC_ASSERT(sizeof(weft_sbe_field_desc_t) == 8, "sbe field desc 8");
WEFT_STATIC_ASSERT(sizeof(weft_sbe_group_desc_t) == 8, "sbe group desc 8");
WEFT_STATIC_ASSERT(sizeof(weft_sbe_schema_t)     == 40, "sbe schema 40");
WEFT_STATIC_ASSERT(offsetof(weft_sbe_schema_t, fixed)        == 16, "sbe schema ptr @16");
WEFT_STATIC_ASSERT(offsetof(weft_sbe_schema_t, groups)       == 24, "sbe schema ptr @24");
WEFT_STATIC_ASSERT(offsetof(weft_sbe_schema_t, group_fields) == 32, "sbe schema ptr @32");

/* Decoded view. scalar[] holds host-order fixed-block values; groups
 * and var fields remain ZERO-COPY pointers into the source buffer. */
typedef struct weft_sbe_group_view {
    const uint8_t *entries;    /* first entry (zero-copy)                */
    uint16_t block_length;     /* per-entry size                         */
    uint16_t count;            /* decoded num_in_group                   */
} weft_sbe_group_view_t;

typedef struct weft_sbe_var_view {
    const uint8_t *data;       /* var-field bytes (zero-copy)            */
    uint16_t length;
} weft_sbe_var_view_t;

typedef struct weft_sbe_view {
    const weft_sbe_schema_t *schema;   /* schema that produced this view */
    uint16_t block_length;             /* from wire header               */
    uint16_t template_id;
    uint16_t schema_id;
    uint16_t version;
    uint32_t total_size;               /* bytes consumed (header+all)    */
    int32_t  status;                   /* WEFT_ADAPTER_OK on success     */
    uint16_t num_fixed;                /* scalars decoded                */
    uint16_t num_groups;
    uint16_t num_var;
    uint16_t _pad0;
    uint64_t scalar[WEFT_SBE_MAX_FIXED];     /* host-order values         */
    uint16_t scalar_kind[WEFT_SBE_MAX_FIXED];
    weft_sbe_group_view_t group[WEFT_SBE_MAX_GROUPS];
    weft_sbe_var_view_t  var[WEFT_SBE_MAX_VAR];
} weft_sbe_view_t;

/* Parse only the 8-byte SBE message header (all fields LE):
 * block_length, template_id, schema_id, version. Bounds-checked. */
int32_t weft_sbe_decode_header(const uint8_t *buf, size_t len,
                               uint16_t *block_length, uint16_t *template_id,
                               uint16_t *schema_id, uint16_t *version);

/* Full schema-driven decode with zero-copy slice bounds verification.
 * Policy (fail-closed): template/schema/version must match the schema
 * exactly; wire block_length must equal schema block_length; group
 * dimension block_length must match schema; group entry counts above
 * WEFT_SBE_MAX_GROUP_ENTRIES fail with EBOUNDS; all pointers bounded. */
int32_t weft_sbe_decode(const uint8_t *buf, size_t len,
                        const weft_sbe_schema_t *schema,
                        weft_sbe_view_t *view);

/* On-demand group entry field decode (zero-copy source, bounded).
 * field_idx indexes the group's own field list (0..num_fields-1). */
int32_t weft_sbe_group_entry(const weft_sbe_view_t *view,
                             uint32_t group_idx, uint32_t entry_idx,
                             uint32_t field_idx, uint64_t *value_out);

/* On-demand group entry CHARS slice (zero-copy pointer into source). */
int32_t weft_sbe_group_bytes(const weft_sbe_view_t *view,
                             uint32_t group_idx, uint32_t entry_idx,
                             uint32_t field_idx,
                             const uint8_t **data_out, uint16_t *len_out);

/* Canonical "Weft Market Data" template (frozen):
 *   schema_id 0x5746 ("WF"), template_id 1, version 0.
 *   fixed block (24B): transact_time_ns u64 @0, security_id u32 @8,
 *     seq_num u32 @12, md_flags u8 @16.
 *   group "order_entries" (32B/entry): price_raw u64 @0, order_ref u64
 *     @8, shares u32 @16, buy_sell u8 @20, action u8 @21.
 *   var[0]: symbol (u8 length + bytes, max 8 for projection). */
const weft_sbe_schema_t *weft_sbe_schema_md(void);

/* DIRECT-TO-WEFT TRANSCODER: project canonical-MD group entries into
 * ITCH add-order-shaped 64-byte messages (Law 2 layout). Symbol comes
 * from var[0] (space-padded to 8; len > 8 fails with EBOUNDS).
 * stock_locate := low 16 bits of security_id; tracking := 0.
 * Returns number of projected messages; status contract as batch APIs. */
size_t weft_sbe_md_to_itch_add(const weft_sbe_view_t *view,
                               weft_itch_msg_t *WEFT_RESTRICT out,
                               size_t out_max, int32_t *status);

/* ================================================================== */
/* SECTION 7 - SIMD checksum & frame-integrity engine                 */
/* ================================================================== */

enum weft_checksum_impl {
    WEFT_CKSUM_IMPL_SLICE8   = 0,  /* portable slicing-by-8              */
    WEFT_CKSUM_IMPL_X86_SSE42 = 1, /* __builtin_ia32_crc32* (SSE4.2)     */
    WEFT_CKSUM_IMPL_ARM_CRC32 = 2  /* __builtin_arm_crc32c* (ARMv8)      */
};

/* Selected implementation probes (for audit / D-61 scorecard). */
int         weft_adapter_crc32c_impl_id(void);
const char *weft_adapter_crc32c_impl_name(void);
const char *weft_adapter_adler32_impl_name(void);

/* CRC-32C (Castagnoli, iSCSI profile: init 0xFFFFFFFF, final xor
 * 0xFFFFFFFF, reflected). crc32c("123456789") == 0xE3069283.
 * Dispatch: hardware CRC when available, slicing-by-8 otherwise. */
uint32_t weft_adapter_crc32c(const void *data, size_t len);
uint32_t weft_adapter_crc32c_update(uint32_t crc, const void *data, size_t len);

/* Software reference (always slicing-by-8) - used by tests as the
 * oracle cross-check and available for split-stream verification. */
uint32_t weft_adapter_crc32c_sw(const void *data, size_t len);
uint32_t weft_adapter_crc32c_sw_update(uint32_t crc, const void *data, size_t len);

/* Adler-32 (RFC 1950). adler32("") == 1; adler32("Wikipedia") ==
 * 0x11E60398. SSE2-vectorized on x86_64, portable deferred-modulo
 * elsewhere. */
uint32_t weft_adapter_adler32(const void *data, size_t len);
uint32_t weft_adapter_adler32_update(uint32_t adler, const void *data, size_t len);
uint32_t weft_adapter_adler32_portable(uint32_t adler, const void *data, size_t len);

/* ---------------- Weft Adapter Frame (WAF) ------------------------- */
/* Wire format (all integers BIG-endian):
 *   +0  4B  magic  "WAF1" (0x57414631)
 *   +4  4B  payload_len
 *   +8  ..  payload (payload_len bytes)
 *   +8+payload_len  4B  CRC-32C over the payload
 * Frame scan validates magic, bounds and checksum in one pass. The
 * engine requires the frame header fields to sit on 4-byte-aligned
 * addresses (buf % 4 == 0) -> otherwise WEFT_ADAPTER_EALIGN. */
#define WEFT_WAF_MAGIC 0x57414631u /* "WAF1" big-endian */

typedef struct weft_waf_view {
    const uint8_t *payload;   /* zero-copy pointer into buf              */
    uint32_t payload_len;
    uint32_t crc_wire;        /* checksum as read from the wire          */
    uint32_t crc_calc;        /* recomputed checksum                     */
} weft_waf_view_t;

int32_t weft_adapter_waf_scan(const uint8_t *buf, size_t len,
                              weft_waf_view_t *out);

/* Encode a WAF frame into dst (must have 12 + payload_len bytes,
 * 4-byte aligned). Returns total bytes written (>= 12) or negative
 * status. */
int32_t weft_adapter_waf_emit(uint8_t *WEFT_RESTRICT dst, size_t dst_len,
                              const void *WEFT_RESTRICT payload,
                              size_t payload_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFT_ADAPTERS_H */
