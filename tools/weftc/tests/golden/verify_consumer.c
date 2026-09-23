/* verify_consumer.c — the Engineer-2 side of the RFC-0017 handshake demo.
 *
 * Hand-written C11 definitions of tests/golden/frames.weft, laid out to
 * the IR contract. Including the generated verify header pins every
 * sizeof/offsetof at THIS compilation: if the schema and this C code ever
 * drift apart, the build breaks — zero runtime cost, zero serialization.
 *
 * run.sh compiles this file (must PASS), then a field-swapped drift
 * variant (must FAIL to compile).
 */
#include <stdint.h>
#include <stddef.h>

/* enum Kind : u8 { Render = 0, Present = 1, Telemetry = 2 } */
typedef uint8_t Kind;
#define Kind_Render   ((Kind)0)
#define Kind_Present  ((Kind)1)
#define Kind_Telemetry ((Kind)2)

/* bitflags Caps : u32 */
typedef uint32_t Caps;
#define Caps_None      ((Caps)0x00000000u)
#define Caps_FloatMath ((Caps)0x00000001u)
#define Caps_SimdBlend ((Caps)0x00000002u)
#define Caps_GpuUpload ((Caps)0x00000004u)

/* struct Vec3 — 12 B, align 4 */
typedef struct Vec3 {
    float x;   /* @0 */
    float y;   /* @4 */
    float z;   /* @8 */
} Vec3;

/* struct Header — 24 B, align 8 */
typedef struct Header {
    uint64_t seq;   /* @0  */
    Kind     kind;  /* @8  */
    uint64_t stamp; /* @16 */
} Header;

/* struct TelemetryMsg — 16 B, align 8 (@optimize(packing) order) */
typedef struct TelemetryMsg {
    double temperature; /* @0  */
    float  pressure;    /* @8  */
    uint8_t flags;      /* @12 */
    uint8_t humidity;   /* @13 */
} TelemetryMsg;

/* struct CachelineFrame — 64 B, align 64 (@align(64)) */
typedef struct CachelineFrame {
    _Alignas(64) uint32_t ctrl; /* @0 — raises struct alignment to 64 */
    uint8_t payload[60];        /* @4 */
} CachelineFrame;

/* span<T> = { u64 offset, u64 len } into a side buffer */
typedef struct WeftSpan {
    uint64_t offset;
    uint64_t len;
} WeftSpan;

/* struct BigFrame — 136 B, align 8 */
typedef struct BigFrame {
    Header  hdr;       /* @0   */
    Vec3    pos;       /* @24  */
    Vec3    vel;       /* @36  */
    Caps    caps;      /* @48  */
    char    name[32];  /* @52  str[32] */
    float   samples[8];/* @84  [f32; 8] */
    WeftSpan blob;     /* @120 span<u8> */
} BigFrame;

#include "frames.verify.h"

/* one translation unit, no main: the _Static_asserts ARE the test */
