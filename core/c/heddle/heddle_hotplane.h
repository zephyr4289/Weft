// heddle_hotplane.h — WHP2 (Weft Hot-Plane v2) unified hot-plane memory
// engine: lock-free two-store seqlock synchronization, signal-driven
// dirty masking, dual-mode (ring / state-plane) lane layouts, and the
// zero-copy WASM/native bridge surface. HOTPLANE-LAYOUT-V2 (normative).
//
// Scope (Pillar 4, Senior Systems Engineer 1 — core engine & memory
// architect, heddle-2.0 Unified Hot-Plane):
//   - 128-byte hot-plane header + 64-byte lane descriptors + 64-byte
//     aligned slot/cell layouts, strictly little-endian, operating
//     identically over POSIX mmap, SharedArrayBuffer, and WASM linear
//     memory. heddle-2.0 bridges 100k+ msgs/sec telemetry onto
//     60/120/240 Hz display refresh with ZERO GC pauses, zero copies
//     and zero widget re-render storms.
//   - Lock-free two-store seqlock: a producer session is published as
//     TWO ordered 64-bit stores (begin_seq enters flight, commit_seq
//     COMMITS — final store). Readers never lock, never block, never
//     allocate: they bracket payload reads between the two stores and
//     retry a BOUNDED number of times before refusing with
//     HEDDLE_E_SEQ_TORN (Law 4 — no unbounded spinloops, ever).
//   - Signal-driven dirty masking: a 64-bit atomic dirty mask (one bit
//     per lane) plus per-lane bounding-box registers record mutated
//     lanes/cell-ranges in single-digit nanoseconds so GPU/Canvas
//     shaders (Engineer 2's plane) skip un-mutated series instantly.
//   - Dual mode: WHP2_MODE_RING (continuous streaming — oscilloscopes,
//     audio waveforms, depth point-clouds; overwrite-oldest, consumer
//     resyncs via per-slot ordinals) and WHP2_MODE_STATE (direct-indexed
//     cell registers — order books, gauges, telemetry HUDs).
//
// Weft Core Laws (enforced here):
//   Law 1 (zero heap on hot path): the engine performs ZERO dynamic
//        allocation — ever. Plane memory is caller-provided (mmap,
//        SharedArrayBuffer, or WASM linear memory); the context is a
//        caller-owned stack struct. There is no malloc, no free, no
//        scratch heap, no hidden arena. Benchmarks prove heap-delta ==
//        0 across 1M+ steady-state operations (mallinfo2/sbrk probes).
//   Law 2 (bounded, deterministic): every retry loop has a
//        compile-time-visible bound; every wire field is explicit
//        little-endian; every structure is 64-byte cacheline
//        bracketed; all layouts are static-asserted at every
//        normative offset.
//   Law 3 (byte-frozen kernel): core/c/weft.{c,h} is untouched. The
//        hot-plane engine is a sibling of the kernel, not a
//        modification of it (0-diff proven in CI).
//   Law 4 (honest boundaries): every failure mode returns a UNIQUE
//        NAMED code from heddle_err_t; nothing fails silently; torn
//        reads are REFUSED, never returned as data.
//
// Threading / ownership contract (normative, HOTPLANE-LAYOUT-V2 §7):
//   Single-producer planes (default): exactly ONE producer thread
//   drives global sessions (hplane_commit_begin/end); all lanes'
//   lane-pairs are driven inside that global session.
//   Multi-producer planes (WHP2_F_MULTI_PRODUCER): each lane is owned
//   by exactly ONE producer thread which drives its per-lane session
//   (hplane_lane_begin/end). Cross-lane snapshot consistency is
//   per-lane consistent + globally monotonic epoch; strict cross-lane
//   snapshots require single-producer mode (documented boundary).
//   Consumers: any number of reader threads; the dirty HARVEST
//   (exchange) is single-render-thread by contract (Engineer 3's
//   orchestrator serializes frame access). Concurrent readers of
//   cells/slots/stats are always safe (two-store protocol).
//
// Memory model: all shared fields are plain uint16/32/64_t accessed
// via __atomic builtins (lock-free on x86-64/aarch64; i64 atomics on
// wasm32 require the threads/atomics feature — provided by
// Emscripten -pthread). Payload copies are chunked 8-byte __atomic
// relaxed loads/stores so every shared access is a data-race-free
// atomic operation (TSan-clean by construction); consistency is
// provided by the two-store protocol, never by multi-field atomicity.

#ifndef HEDDLE_HOTPLANE_H
#define HEDDLE_HOTPLANE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version / identity constants
// ---------------------------------------------------------------------------

#define WHP2_MAGIC              0x57485032u  /* hex glyphs 'W','H','P','2'  */
#define WHP2_LAYOUT_MAJOR       2u           /* heddle-2.0 hot-plane V2     */
#define WHP2_LAYOUT_MINOR       0u
#define WHP2_LAYOUT_REV         1u           /* layout revision within V2  */

#define WHP2_HEADER_SIZE        128u         /* plane header, 2 cachelines */
#define WHP2_LANE_DESC_SIZE     64u          /* lane descriptor, 1 line    */
#define WHP2_SLOT_HEADER_SIZE   16u          /* ring slot sync header      */
#define WHP2_MIN_SLOT_STRIDE    64u          /* slots are cacheline-sized  */
#define WHP2_MAX_LANES          64u          /* dirty mask width           */
#define WHP2_MIN_CAPACITY       2u           /* ring wrap needs >= 2       */
#define WHP2_MAX_CAPACITY       (1u << 20)
#define WHP2_MAX_SAMPLE_SIZE    4080u        /* keeps stride <= 4096       */
#define WHP2_MAX_LANE_STRIDE    (1u << 30)   /* 1 GiB per lane             */

/* Two-store publish protocol (normative, §4): session id S is stored
 * as TWO ordered 64-bit stores into ONE cacheline:
 *   store #1: begin_seq  = S   (session enters flight)
 *   store #2: commit_seq = S   (COMMIT — final store)
 * A frame is quiescent-consistent iff begin_seq == commit_seq. Session
 * ids increment by exactly 1 per session; the transient window between
 * the stores exposes begin==S+1 / commit==S, which readers detect and
 * refuse (bounded retries -> HEDDLE_E_SEQ_TORN). A producer crashing
 * between the two stores leaves a permanently in-flight pair -> honest
 * refusal, never torn data. Range: full u64 (2^64 sessions; at 1e9
 * sessions/sec that is 584 years). */
#define WHP2_DEFAULT_READ_RETRIES 64u

/* Endianness canary: stored little-endian; a big-endian host (or a
 * byte-swapped blob) decodes a different value -> HEDDLE_E_ENDIAN. */
#define WHP2_ENDIAN_CANARY     0x0DD0C0DEu

/* Static-config CRC: CRC-32/IEEE (poly 0xEDB88320 reflected,
 * init/xorout 0xFFFFFFFF) over header bytes [0x10,0x40). */
#define WHP2_STATIC_CRC_COVER_OFF 0x10u
#define WHP2_STATIC_CRC_COVER_LEN 0x30u

// ---------------------------------------------------------------------------
// Modes, stat kinds, flags
// ---------------------------------------------------------------------------

#define WHP2_MODE_RING         0u   /* continuous streaming (overwrite-oldest) */
#define WHP2_MODE_STATE        1u   /* direct-indexed cell registers (snapshot) */

#define WHP2_STAT_U64          0u   /* min/max/current compare raw u64          */
#define WHP2_STAT_I64          1u   /* sign-flip keyed two's complement order   */
#define WHP2_STAT_F64          2u   /* IEEE-754 total-order key (any NaN last)  */

/* Plane flags (header +0x3C). v2 core computes dirty mask, bbox and
 * lane stats unconditionally — they ARE the signal plane; the flags
 * exist for orchestrator policy and future revisions. */
#define WHP2_F_MULTI_PRODUCER  (1u << 0)  /* per-lane producer ownership      */
#define WHP2_F_BBOX            (1u << 1)  /* bounding boxes on (v2: always)   */
#define WHP2_F_STATS           (1u << 2)  /* per-lane stats on (v2: always)   */

/* Lane flags (lane descriptor +0x38). */
#define WHP2_LANE_F_ACTIVE     (1u << 0)  /* lane attached/owned              */
#define WHP2_LANE_F_BP_MARK    (1u << 1)  /* in-band backpressure: ring at/   */
                                          /* past the high-water wrap mark    */
#define WHP2_LANE_F_OVERRUN    (1u << 2)  /* overwrite observed >= 1 time     */

/* Bounding-box register: hi32 = min mutated index, lo32 = max mutated
 * index, since the last consumer harvest. EMPTY sentinel encodes
 * min=0xFFFFFFFF > max=0 (min>max is unreachable for real ranges). */
#define WHP2_BBOX_EMPTY        0xFFFFFFFF00000000ull
#define WHP2_BBOX_MIN_MASK     0xFFFFFFFF00000000ull
#define WHP2_BBOX_MAX_MASK     0x00000000FFFFFFFFull

// ---------------------------------------------------------------------------
// Error ladder (Law 4 — every failure has a unique, named code)
// ---------------------------------------------------------------------------

typedef enum {
    HEDDLE_OK               = 0,

    /* --- attach / validation ladder ------------------------------------ */
    HEDDLE_E_ARG            = 1,   /* null ptr / zero len / bad alignment  */
    HEDDLE_E_MAGIC          = 2,   /* magic != WHP2_MAGIC                  */
    HEDDLE_E_VERSION        = 3,   /* layout major != 2                    */
    HEDDLE_E_LAYOUT         = 4,   /* header size / layout rev / geometry  */
    HEDDLE_E_ENDIAN         = 5,   /* endian canary mismatch               */
    HEDDLE_E_CFG_CRC        = 6,   /* static config CRC-32/IEEE mismatch   */
    HEDDLE_E_REGION_SIZE    = 7,   /* buffer smaller than plane_size       */
    HEDDLE_E_CAPACITY       = 8,   /* capacity / sample / stride out of rng*/

    /* --- hot-path ladder ------------------------------------------------ */
    HEDDLE_E_LANE_OVERFLOW  = 9,   /* lane >= lane_count (or > 63)         */
    HEDDLE_E_PAYLOAD        = 10,  /* len == 0 or len > sample_size        */
    HEDDLE_E_MODE           = 11,  /* op not valid for this plane's mode   */
    HEDDLE_E_STATE          = 12,  /* session/role/slot state violation    */
    HEDDLE_E_SEQ_TORN       = 13,  /* two-store tear after bounded retries */
    HEDDLE_E_OVERRUN        = 14,  /* ring slot overwritten before consume */
    HEDDLE_E_NOT_PUBLISHED  = 15,  /* slot ordinal not yet written         */
    HEDDLE_E_UNSUPPORTED    = 16,  /* declared v2 boundary refused         */
} heddle_err_t;

const char *hplane_err_name(int err);   /* unique name per code, never NULL */

// ---------------------------------------------------------------------------
// Plane header — 128 bytes, 64-byte aligned (§3, normative)
// ---------------------------------------------------------------------------
// Cacheline 0 (offsets 0x00-0x3F): identity + static geometry. Immutable
// after create. static_crc32 covers bytes [0x10,0x40).
// Cacheline 1 (offsets 0x40-0x7F): the synchronization plane — the ONE
// deliberately shared cacheline (producer commit rate + consumer frame
// rate traffic; all cross-thread coordination lives here by design).
//
// All multibyte fields are LITTLE-ENDIAN on the wire (Law 2).

typedef struct __attribute__((aligned(64))) hplane_header {
    /* --- cacheline 0: identity + static geometry (immutable) ----------- */
    uint32_t magic;             /* +0x00 WHP2_MAGIC                          */
    uint16_t ver_major;         /* +0x04 WHP2_LAYOUT_MAJOR                   */
    uint16_t ver_minor;         /* +0x06 WHP2_LAYOUT_MINOR                   */
    uint32_t header_size;       /* +0x08 = 128, self-describing              */
    uint32_t static_crc32;      /* +0x0C CRC-32/IEEE over [0x10,0x40)        */
    uint32_t layout_rev;        /* +0x10 = 1 (layout rev within V2)          */
    uint32_t mode;              /* +0x14 WHP2_MODE_RING / WHP2_MODE_STATE    */
    uint32_t lane_count;        /* +0x18 1..64 (dirty mask width)            */
    uint32_t lane_stride;       /* +0x1C bytes per lane data region, 64-mult */
    uint32_t sample_size;       /* +0x20 1..4080                             */
    uint32_t slot_capacity;     /* +0x24 slots (ring) / cells (state) / lane */
    uint64_t plane_size;        /* +0x28 total region bytes                  */
    uint64_t create_stamp_ns;   /* +0x30 monotonic clock at create (diag)    */
    uint32_t stat_kind;         /* +0x38 WHP2_STAT_U64/I64/F64               */
    uint32_t flags;             /* +0x3C WHP2_F_*                            */

    /* --- cacheline 1: synchronization plane (mutable) ------------------ */
    uint64_t epoch;             /* +0x40 session epoch: multi-producer     */
                                /* planes fetch-add the lane-session total;*/
                                /* single-producer planes DERIVE it from   */
                                /* commit_seq on read (values coincide) so */
                                /* the commit path stays free of RMWs      */
    uint64_t begin_seq;         /* +0x48 two-store #1 — session enters flight*/
    uint64_t commit_seq;        /* +0x50 two-store #2 — COMMIT (final store) */
    uint64_t dirty_mask;        /* +0x58 active dirty mask, lane i = bit i   */
    uint64_t render_frame_id;   /* +0x60 consumer frame counter (frames done)*/
    uint64_t heartbeat_ns;      /* +0x68 last producer activity (diagnostics)*/

    uint32_t endian_canary;     /* +0x70 WHP2_ENDIAN_CANARY, LE             */
    uint32_t reserved0;         /* +0x74 zero                               */
    uint64_t dirty_transitions; /* +0x78 monotonic 0->1 mask transitions     */
} hplane_header_t;

/* Lane statistics snapshot (decoded from a lane descriptor, §5). */
typedef struct hplane_lane_stats {
    uint64_t min_raw;           /* raw bits; interpretation per stat_kind    */
    uint64_t max_raw;
    uint64_t current_raw;       /* state: latest sample; ring: head count    */
    uint64_t commit_count;      /* lane sessions committed (version counter) */
} hplane_lane_stats_t;

// ---------------------------------------------------------------------------
// Lane descriptor — 64 bytes, one per lane, array at offset 128 (§3.2)
// ---------------------------------------------------------------------------
// Producer-owned per lane (the lane's single owner writer); consumers
// read via the lane's own two-store pair. lane_current is DUAL-USE by
// mode: state planes hold the latest written sample (raw bits); ring
// planes hold the monotonic push count (head). Initial state (no data):
// min=UINT64_MAX, max=0, current=0, commit_count=0 — callers detect
// "no samples yet" via commit_count == 0.

typedef struct __attribute__((aligned(64))) hplane_lane_desc {
    uint64_t lane_min;          /* +0x00 running min (raw key)               */
    uint64_t lane_max;          /* +0x08 running max (raw key)               */
    uint64_t lane_current;      /* +0x10 latest sample (state) / head (ring) */
    uint64_t lane_begin_seq;    /* +0x18 per-lane two-store #1              */
    uint64_t lane_commit_seq;   /* +0x20 per-lane two-store #2 — COMMIT     */
    uint64_t lane_commit_cnt;   /* +0x28 lane sessions committed, monotonic  */
    uint64_t bbox_packed;       /* +0x30 hi32 min idx | lo32 max idx         */
    uint32_t lane_flags;        /* +0x38 WHP2_LANE_F_*                       */
    uint32_t lane_misc;         /* +0x3C hi16 owner_tag | lo16 overrun_count */
} hplane_lane_desc_t;

// ---------------------------------------------------------------------------
// Ring slot header — 16 bytes, payload at slot+0x10 (§3.3, normative)
// ---------------------------------------------------------------------------
// Slot i of lane L lives at 128 + lane_count*64 + L*lane_stride +
// i*slot_stride where slot_stride = max(64, align64(16 + sample_size)).
// The slot's two-store pair carries the PUSH ORDINAL n (1-based,
// monotonic per lane): push #n writes slot (n-1) % slot_capacity.
// A reader requesting ordinal n sees: commit==n -> exact frame;
// commit > n -> HEDDLE_E_OVERRUN (data lost to wrap); commit < n ->
// HEDDLE_E_NOT_PUBLISHED; begin != commit -> in-flight, bounded retry.

typedef struct __attribute__((aligned(8))) hplane_slot_header {
    uint64_t begin_seq;         /* +0x00 ordinal enters flight               */
    uint64_t commit_seq;        /* +0x08 ordinal COMMIT — final store        */
} hplane_slot_header_t;

/* Static layout proofs (compile-time, Law 2). */
_Static_assert(sizeof(hplane_header_t) == 128,       "WHP2 header must be 128B");
_Static_assert(sizeof(hplane_lane_desc_t) == 64,     "WHP2 lane desc must be 64B");
_Static_assert(sizeof(hplane_slot_header_t) == 16,   "WHP2 slot header must be 16B");
_Static_assert(offsetof(hplane_header_t, static_crc32)   == 0x0C, "crc @0x0C");
_Static_assert(offsetof(hplane_header_t, mode)          == 0x14, "mode @0x14");
_Static_assert(offsetof(hplane_header_t, lane_count)    == 0x18, "lanes @0x18");
_Static_assert(offsetof(hplane_header_t, plane_size)    == 0x28, "psize @0x28");
_Static_assert(offsetof(hplane_header_t, epoch)         == 0x40, "epoch @0x40");
_Static_assert(offsetof(hplane_header_t, begin_seq)     == 0x48, "begin @0x48");
_Static_assert(offsetof(hplane_header_t, commit_seq)    == 0x50, "commit @0x50");
_Static_assert(offsetof(hplane_header_t, dirty_mask)    == 0x58, "mask @0x58");
_Static_assert(offsetof(hplane_header_t, render_frame_id) == 0x60, "frame @0x60");
_Static_assert(offsetof(hplane_header_t, endian_canary) == 0x70, "canary @0x70");
_Static_assert(offsetof(hplane_header_t, dirty_transitions) == 0x78,
               "transitions @0x78");
_Static_assert(offsetof(hplane_lane_desc_t, lane_current)   == 0x10, "cur @0x10");
_Static_assert(offsetof(hplane_lane_desc_t, lane_begin_seq) == 0x18, "lbegin @0x18");
_Static_assert(offsetof(hplane_lane_desc_t, lane_commit_seq) == 0x20, "lcommit @0x20");
_Static_assert(offsetof(hplane_lane_desc_t, bbox_packed)     == 0x30, "bbox @0x30");
_Static_assert(offsetof(hplane_lane_desc_t, lane_flags)      == 0x38, "lflags @0x38");

// ---------------------------------------------------------------------------
// Little-endian store/load (Law 2 — explicit formatting; on LE hosts
// these compile to single moves, on BE hosts they byte-swap honestly)
// ---------------------------------------------------------------------------

static inline void hplane_le16_put(void *dst, uint16_t v)
{
    uint8_t *p = (uint8_t *)dst;
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}
static inline void hplane_le32_put(void *dst, uint32_t v)
{
    uint8_t *p = (uint8_t *)dst;
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline void hplane_le64_put(void *dst, uint64_t v)
{
    hplane_le32_put(dst, (uint32_t)v);
    hplane_le32_put((uint8_t *)dst + 4, (uint32_t)(v >> 32));
}
static inline uint16_t hplane_le16_get(const void *src)
{
    const uint8_t *p = (const uint8_t *)src;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t hplane_le32_get(const void *src)
{
    const uint8_t *p = (const uint8_t *)src;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t hplane_le64_get(const void *src)
{
    return (uint64_t)hplane_le32_get(src) |
           ((uint64_t)hplane_le32_get((const uint8_t *)src + 4) << 32);
}

/* Stride math (pure arithmetic, Law 2 — zero rounding surprises). */
static inline uint32_t hplane_align64(uint32_t x)
{
    return (x + 63u) & ~63u;
}
static inline uint32_t hplane_slot_stride(uint32_t sample_size)
{
    uint32_t s = hplane_align64(WHP2_SLOT_HEADER_SIZE + sample_size);
    return s < WHP2_MIN_SLOT_STRIDE ? WHP2_MIN_SLOT_STRIDE : s;
}
static inline uint32_t hplane_cell_stride(uint32_t sample_size)
{
    uint32_t s = hplane_align64(sample_size);
    return s < WHP2_MIN_SLOT_STRIDE ? WHP2_MIN_SLOT_STRIDE : s;
}

// ---------------------------------------------------------------------------
// CRC-32/IEEE (poly 0xEDB88320, init/xorout 0xFFFFFFFF, reflected)
// ---------------------------------------------------------------------------

uint32_t hplane_crc32(const void *data, size_t len);
/* Incremental (attach/create-time only; never on the hot path). */
uint32_t hplane_crc32_update(uint32_t crc, const void *data, size_t len);

// ---------------------------------------------------------------------------
// Plane configuration / sizing / lifecycle
// ---------------------------------------------------------------------------

typedef struct hplane_cfg {
    uint32_t mode;             /* WHP2_MODE_RING or WHP2_MODE_STATE         */
    uint32_t lane_count;       /* 1..WHP2_MAX_LANES                         */
    uint32_t sample_size;      /* 1..WHP2_MAX_SAMPLE_SIZE                   */
    uint32_t slot_capacity;    /* per lane; ring >= 2, state >= 1           */
    uint32_t stat_kind;        /* WHP2_STAT_U64 / I64 / F64                 */
    uint32_t flags;            /* WHP2_F_* (0 = single producer)            */
} hplane_cfg_t;

/* Total bytes a plane with this config occupies:
 *   128 (header) + lane_count*64 (descriptors) +
 *   lane_count * lane_stride (data regions). */
uint64_t hplane_plane_size(const hplane_cfg_t *cfg);

/* Roles for attach (context capability bits). */
#define WHP2_ROLE_PRODUCER  1u
#define WHP2_ROLE_CONSUMER  2u
#define WHP2_ROLE_BOTH      3u

/* The caller-owned context. NOT shared memory; no allocation anywhere.
 * One context per thread-role (a producer context is single-threaded
 * by contract; consumers may share a plane but each keeps its own). */
typedef struct hplane_ctx {
    uint8_t *mem;              /* plane base (aligned 64)                   */
    uint64_t mem_size;         /* validated region length                   */
    /* decoded static config (cached, immutable) */
    uint32_t mode;
    uint32_t lane_count;
    uint32_t lane_stride;
    uint32_t sample_size;
    uint32_t slot_capacity;
    uint32_t stat_kind;
    uint32_t flags;
    uint32_t role;
    /* producer session state (private to the owning thread) */
    uint64_t session;          /* open global session id, 0 = none          */
    uint64_t dirty_pending;    /* batched dirty bits for open session       */
    uint64_t touched_lanes;    /* lanes whose pair was opened this session  */
    uint64_t lane_session;     /* open per-lane session id, 0 = none        */
    uint32_t lane_open;        /* lane index of the open lane session       */
    uint64_t frame_seen;       /* render_frame_id at last bbox reset        */
    uint64_t last_commit;      /* last committed session id (owner cache)  */
} hplane_ctx_t;

/* Create: writes the normative header + lane descriptors into caller
 * memory (whole region zeroed first — deterministic; create-time only,
 * never on the hot path). Returns HEDDLE_OK or the validation ladder. */
int hplane_plane_create(void *mem, size_t len, const hplane_cfg_t *cfg,
                        uint64_t create_stamp_ns);

/* Attach: runs the FULL validation ladder (magic -> version -> layout
 * -> endian canary -> static CRC -> geometry -> region size), then
 * fills the caller-owned ctx with the decoded config. Role gates all
 * later operations. */
int hplane_attach(void *mem, size_t len, hplane_ctx_t *ctx, uint32_t role);

/* Validation only (no ctx side effects) — the honest boundary probe. */
int hplane_validate(void *mem, size_t len);

// ---------------------------------------------------------------------------
// Producer plane (single-producer global sessions)
// ---------------------------------------------------------------------------

/* Open a global session: begin_seq store + release fence. */
int hplane_commit_begin(hplane_ctx_t *p);

/* Close the open global session: close every touched lane's pair,
 * flush the batched dirty mask, commit_seq store (final), epoch bump,
 * heartbeat. Sub-nanosecond steady-state amortized. */
int hplane_commit_end(hplane_ctx_t *p);

// ---------------------------------------------------------------------------
// Producer plane (multi-producer per-lane sessions)
// ---------------------------------------------------------------------------

/* Open the lane's own session (lane owner only). */
int hplane_lane_begin(hplane_ctx_t *p, uint32_t lane);

/* Close the lane session: dirty bit flush, lane commit store, epoch. */
int hplane_lane_end(hplane_ctx_t *p, uint32_t lane);

// ---------------------------------------------------------------------------
// Mutations (valid only inside an open session; Law 1: zero allocation)
// ---------------------------------------------------------------------------

/* Ring mode: append one sample at the lane's head (ordinal = head+1).
 * Overwrite-oldest ring: producer NEVER blocks; wrap sets the lane's
 * in-band backpressure mark (WHP2_LANE_F_BP_MARK) and bumps the
 * overrun counter — the consumer detects lost ordinals as
 * HEDDLE_E_OVERRUN and resyncs to the new head. */
int hplane_ring_push(hplane_ctx_t *p, uint32_t lane,
                     const void *sample, uint32_t len);

/* State mode: write one cell register (direct-indexed). */
int hplane_state_write(hplane_ctx_t *p, uint32_t lane, uint32_t cell,
                       const void *val, uint32_t len);

// ---------------------------------------------------------------------------
// Consumer plane (lock-free reads — never block, never allocate)
// ---------------------------------------------------------------------------

/* Read one state cell (two-store bracketed, bounded retries). */
int hplane_cell_read(hplane_ctx_t *c, uint32_t lane, uint32_t cell,
                     void *out, uint32_t len, uint32_t max_retries);

/* Read ring sample ordinal `seq` (1-based). Refusals: E_OVERRUN (lost
 * to wrap), E_NOT_PUBLISHED (not yet written), E_STATE (never written),
 * E_SEQ_TORN (bounded retry exhaustion under write pressure). */
int hplane_slot_read(hplane_ctx_t *c, uint32_t lane, uint64_t seq,
                     void *out, uint32_t len, uint32_t max_retries);

/* Monotonic push count of a lane (head). 0 = no samples yet. */
int hplane_ring_head(const hplane_ctx_t *c, uint32_t lane, uint64_t *head_out);

/* Whole-lane snapshot (state mode): consistent copy of every cell. */
int hplane_lane_snapshot(hplane_ctx_t *c, uint32_t lane,
                         void *buf, size_t buf_len, uint32_t max_retries);

/* Lane stats snapshot (min/max/current/commit_count, consistent). */
int hplane_lane_stats_get(hplane_ctx_t *c, uint32_t lane,
                          hplane_lane_stats_t *out, uint32_t max_retries);

// ---------------------------------------------------------------------------
// Dirty plane / frame protocol (the signal-driven render contract, §6)
// ---------------------------------------------------------------------------

/* Harvest: atomically exchange the active dirty mask with 0 and return
 * the previous mask. One call per frame, render thread only (the
 * exchange is the frame barrier; consumers that need concurrent
 * harvest must serialize via Engineer 3's orchestrator). */
int hplane_dirty_harvest(hplane_ctx_t *c, uint64_t *mask_out);

/* Re-mark lanes (set bits) — the render thread calls this when a lane
 * read ended torn (E_SEQ_TORN) or required retries, deferring the lane
 * to the next frame. This is what makes the mask protocol LOSSLESS:
 * a deferred lane is always re-signalled, never silently dropped. */
int hplane_dirty_remark(hplane_ctx_t *c, uint64_t bits);

/* Monotonic count of 0->1 dirty-mask transitions since create — the
 * ground truth for lossless-harvest verification. EXACT identity:
 *   dirty_transitions == SUM(popcount of every harvest result)
 *                        + popcount(final mask)
 * (atomicity of fetch_or/exchange on one register makes it so). */
uint64_t hplane_dirty_transitions_get(const hplane_ctx_t *c);

/* Harvest one lane's bounding box (packed hi32=min | lo32=max indices
 * mutated since the producer last observed this consumer's frame
 * advance) via a two-store bracketed read — the register is
 * PRODUCER-ONLY-written: expanded inside each session, reset at
 * session start when the consumer's frame has advanced (§6). EMPTY
 * is returned only for lanes never written; renderers treat
 * EMPTY-with-dirty-bit as "render the whole lane" (conservative). */
int hplane_bbox_harvest(hplane_ctx_t *c, uint32_t lane,
                        uint64_t *bbox_out);

/* Publish a completed frame: render_frame_id++ (returns the new id).
 * Pure observability/lifecycle marker for the orchestrator plane. */
int hplane_frame_commit(hplane_ctx_t *c, uint64_t *frame_id_out);

/* Last published frame id (acquire load — pairs with commit). */
uint64_t hplane_frame_get(const hplane_ctx_t *c);

/* Fast "did anything change" probe — the idle-frame killer: if epoch
 * equals the value your renderer cached last frame, NOTHING committed
 * and the whole plane can be skipped without touching a single lane. */
uint64_t hplane_epoch_get(const hplane_ctx_t *c);

/* Diagnostics: last producer heartbeat (monotonic ns, 0 = never).
 * commit_end NEVER takes a clock (the hot path stays pure stores);
 * producers touch the heartbeat at orchestrator cadence instead. */
uint64_t hplane_heartbeat_get(const hplane_ctx_t *c);
int      hplane_heartbeat_touch(hplane_ctx_t *p);

const char *hplane_mode_name(uint32_t mode);
const char *hplane_stat_kind_name(uint32_t kind);

#ifdef __cplusplus
}
#endif

#endif /* HEDDLE_HOTPLANE_H */
