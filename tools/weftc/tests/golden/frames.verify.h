/* weftc 0.1.0 verify header — generated from "tests/golden/frames.weft"
 * schema_id 0x5b31c5b9f9f7b3eb  abi_hash 0x0e7280ce8f313931  fnv1a64 0x94a2f711e9ad2f16
 *
 * DO NOT EDIT — regenerate with:
 *     weftc compile "tests/golden/frames.weft"
 *
 * Usage (Engineer 2 workflow): include this header AFTER the
 * generated struct definitions. The assertions pin the frozen
 * layout at consumer compile time — ABI drift becomes a hard
 * build error, at zero runtime cost.
 *
 * Decl and field names are used verbatim; the .weft grammar only
 * admits [A-Za-z_][A-Za-z0-9_]* and .weft keywords never shadow
 * these uses. C keyword collisions are a codegen mapping concern.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#  if __cplusplus < 201103L
#    error "weftc verify header requires C++11 or later"
#  endif
#  define WEFT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define WEFT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#  error "weftc verify header requires C11 or later"
#endif

#define WEFT_SCHEMA_ID    0x5b31c5b9f9f7b3ebull
#define WEFT_ABI_HASH     0x0e7280ce8f313931ull
#define WEFT_FNV1A64_ABI 0x94a2f711e9ad2f16ull
#define WEFT_ENDIANNESS_BIG 0

/* enum Kind — 1 B, align 1 */
#define WEFT_ABI_HASH_Kind 0xf7714909a63a66adull
WEFT_STATIC_ASSERT(sizeof(Kind) == 1,
    "weftc: `Kind` size drifted (expected 1 B)");

/* bitflags Caps — 4 B, align 4 */
#define WEFT_ABI_HASH_Caps 0x70dfc168b7e518ccull
WEFT_STATIC_ASSERT(sizeof(Caps) == 4,
    "weftc: `Caps` size drifted (expected 4 B)");

/* struct Vec3 — 12 B, align 4 */
#define WEFT_ABI_HASH_Vec3 0x61ddb568acf95b0full
WEFT_STATIC_ASSERT(sizeof(Vec3) == 12,
    "weftc: `Vec3` size drifted (expected 12 B)");
WEFT_STATIC_ASSERT(offsetof(Vec3, x) == 0,
    "weftc: `Vec3.x` offset drifted (expected 0)");
WEFT_STATIC_ASSERT(offsetof(Vec3, y) == 4,
    "weftc: `Vec3.y` offset drifted (expected 4)");
WEFT_STATIC_ASSERT(offsetof(Vec3, z) == 8,
    "weftc: `Vec3.z` offset drifted (expected 8)");

/* struct Header — 24 B, align 8 */
#define WEFT_ABI_HASH_Header 0x8427797a68caa357ull
WEFT_STATIC_ASSERT(sizeof(Header) == 24,
    "weftc: `Header` size drifted (expected 24 B)");
WEFT_STATIC_ASSERT(offsetof(Header, seq) == 0,
    "weftc: `Header.seq` offset drifted (expected 0)");
WEFT_STATIC_ASSERT(offsetof(Header, kind) == 8,
    "weftc: `Header.kind` offset drifted (expected 8)");
WEFT_STATIC_ASSERT(offsetof(Header, stamp) == 16,
    "weftc: `Header.stamp` offset drifted (expected 16)");

/* struct TelemetryMsg — 16 B, align 8 */
#define WEFT_ABI_HASH_TelemetryMsg 0x5f12f453dd569495ull
WEFT_STATIC_ASSERT(sizeof(TelemetryMsg) == 16,
    "weftc: `TelemetryMsg` size drifted (expected 16 B)");
WEFT_STATIC_ASSERT(offsetof(TelemetryMsg, temperature) == 0,
    "weftc: `TelemetryMsg.temperature` offset drifted (expected 0)");
WEFT_STATIC_ASSERT(offsetof(TelemetryMsg, pressure) == 8,
    "weftc: `TelemetryMsg.pressure` offset drifted (expected 8)");
WEFT_STATIC_ASSERT(offsetof(TelemetryMsg, flags) == 12,
    "weftc: `TelemetryMsg.flags` offset drifted (expected 12)");
WEFT_STATIC_ASSERT(offsetof(TelemetryMsg, humidity) == 13,
    "weftc: `TelemetryMsg.humidity` offset drifted (expected 13)");

/* struct CachelineFrame — 64 B, align 64 */
#define WEFT_ABI_HASH_CachelineFrame 0x79f04e9440822e70ull
WEFT_STATIC_ASSERT(sizeof(CachelineFrame) == 64,
    "weftc: `CachelineFrame` size drifted (expected 64 B)");
WEFT_STATIC_ASSERT(offsetof(CachelineFrame, ctrl) == 0,
    "weftc: `CachelineFrame.ctrl` offset drifted (expected 0)");
WEFT_STATIC_ASSERT(offsetof(CachelineFrame, payload) == 4,
    "weftc: `CachelineFrame.payload` offset drifted (expected 4)");

/* struct BigFrame — 136 B, align 8 */
#define WEFT_ABI_HASH_BigFrame 0x94c196087085eef4ull
WEFT_STATIC_ASSERT(sizeof(BigFrame) == 136,
    "weftc: `BigFrame` size drifted (expected 136 B)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, hdr) == 0,
    "weftc: `BigFrame.hdr` offset drifted (expected 0)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, pos) == 24,
    "weftc: `BigFrame.pos` offset drifted (expected 24)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, vel) == 36,
    "weftc: `BigFrame.vel` offset drifted (expected 36)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, caps) == 48,
    "weftc: `BigFrame.caps` offset drifted (expected 48)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, name) == 52,
    "weftc: `BigFrame.name` offset drifted (expected 52)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, samples) == 84,
    "weftc: `BigFrame.samples` offset drifted (expected 84)");
WEFT_STATIC_ASSERT(offsetof(BigFrame, blob) == 120,
    "weftc: `BigFrame.blob` offset drifted (expected 120)");
