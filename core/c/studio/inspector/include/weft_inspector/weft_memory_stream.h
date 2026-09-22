// weft_memory_stream.h — the Studio visual memory-map feeder (Pillar 7).
//
// WHY EXISTS: Weft Studio renders the live occupancy of zero-copy rings at
// 120/240 FPS — up to 1,000,000 cells — and a naive "snapshot = scan every
// slot version" costs 50-100 us per 100k slots, six times the 5 us SLA
// before the GPU ever sees a byte. The feeder solves it the way frame
// buffers solved it: an incremental SHADOW PLANE. Each scrape pass touches
// only the cells whose absolute ring position crossed a frontier since the
// last pass (O(work delta), budgeted), records cell changes as 8-byte
// delta records, and the frame assembler packs header + ring blocks +
// deltas into ONE contiguous blob — a send-ready WebSocket / WebGPU upload
// buffer — while the full-resolution plane is exported ZERO-COPY (the
// studio uploads the shadow plane directly as an R8 texture; no memcpy
// exists anywhere on the frame path).
//
// CELL STATE SEMANTICS (per family — the legend the Studio renders):
//   WFRM (rmw_weft loan-ring), absolute slot s, physical k = s & (M-1):
//     WRITING    slot version odd (writer mid-commit — caught by the
//                frontier probe or a version read on a changed cell)
//     CLAIMED    k == head & mask: the next write target (writer-visible
//                borrow leaves no shared trace until the version flips —
//                CLAIMED is the honest frontier convention, labeled as such)
//     COMMITTED  tail_ack <= s < head, version even (in-flight loan)
//     READ       s < tail_ack (acked; the writer may reuse the cell)
//     FREE       version == 0 (never written; fresh ring capacity)
//     DROPPED    aggregate-only: BEST_EFFORT drops never occupy a slot, so
//                they stream as the ring block's dropped counters — the
//                cell enum value exists for the Studio's overlay legend
//   WFSH (cluster mesh, RFC-0004), frame L, slot k = (L-1) mod M:
//     WRITING    slotSeq[k] == 0 while latest > 0 (invalidated: filling)
//     FREE       slotSeq[k] == 0 while latest == 0 (never written)
//     COMMITTED  slotSeq[k] != 0 (age = latest - slotSeq; no ack plane —
//                READ is unreachable for stateless RFC-0004 readers,
//                declared rather than faked)
//
// LAWS carried here:
//   Law 1  the scrape pass is bounded by a caller budget (cells/pass);
//          saturation beyond the budget coalesces intermediate cell states
//          and is REPORTED (lag counters), never dropped silently.
//   Law 2  planes and delta records live in caller-provided arenas; the
//          frame blob is packed into a caller arena; steady state is
//          zero-heap (allocator interposition proves it in the battery).
//   Law 3  frame headers carry CLOCK_MONOTONIC_RAW stamps; the pacer is
//          pure arithmetic (never sleeps — the Studio owns the loop).
//
// WIRE FORMAT (little-endian, frozen v1; static asserts below):
//   blob header  40 B: "WFMS" u32, u16 version, u16 kind (0 delta /
//                1 keyframe), u64 ts_ns, u64 frame_seq, u32 ring_count,
//                u32 delta_count, u32 total_cells, u32 flags
//   ring block  112 B each: name[48], u32 cells, u32 rows, u32 cols,
//                u32 pad, u64 head, tail, published, dropped, in_flight,
//                scrape_seq
//   delta rec     8 B each: u16 ring, u8 state, u8 pad, u32 cell

#ifndef WEFT_INSPECT__MEMORY_STREAM_H_
#define WEFT_INSPECT__MEMORY_STREAM_H_

#include <stddef.h>
#include <stdint.h>

#include "weft_inspector/weft_inspect_common.h"
#include "weft_inspector/weft_shm_inspector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEFT_MSTREAM_MAX_BINDS 8u
#define WEFT_MSTREAM_BLOB_MAGIC 0x534d4657u   /* "WFMS" little-endian */
#define WEFT_MSTREAM_BLOB_VERSION 1u

/* --- cell states (the Studio's texture palette) ---------------------------- */

enum {
    WEFT_MSTREAM_CELL_FREE = 0,
    WEFT_MSTREAM_CELL_CLAIMED = 1,
    WEFT_MSTREAM_CELL_WRITING = 2,
    WEFT_MSTREAM_CELL_COMMITTED = 3,
    WEFT_MSTREAM_CELL_READ = 4,
    WEFT_MSTREAM_CELL_DROPPED = 5,   /* aggregate-only; see header legend */
};

/* --- wire structs (frozen; byte-exact by static assert) --------------------- */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;          /* 0 delta, 1 keyframe */
    uint64_t ts_ns;
    uint64_t frame_seq;
    uint32_t ring_count;
    uint32_t delta_count;
    uint32_t total_cells;
    uint32_t flags;
} weft_mstream_blob_hdr_t;

typedef struct {
    char name[48];
    uint32_t cells;
    uint32_t rows;
    uint32_t cols;
    uint32_t pad;
    uint64_t head;
    uint64_t tail;
    uint64_t published;
    uint64_t dropped;
    uint64_t in_flight;
    uint64_t scrape_seq;
} weft_mstream_blob_ring_t;

typedef struct {
    uint16_t ring;
    uint8_t state;
    uint8_t pad;
    uint32_t cell;
} weft_mstream_blob_delta_t;

_Static_assert(sizeof(weft_mstream_blob_hdr_t) == 40u, "frozen wire: hdr 40B");
_Static_assert(sizeof(weft_mstream_blob_ring_t) == 112u, "frozen wire: ring 112B");
_Static_assert(sizeof(weft_mstream_blob_delta_t) == 8u, "frozen wire: delta 8B");

/* frame flags */
#define WEFT_MSTREAM_FLAG_DELTA_OVERFLOW 0x1u  /* delta list truncated:
                                                * consume a keyframe frame */
#define WEFT_MSTREAM_FLAG_SOFT_SNAPSHOT 0x2u   /* some ring snapshot soft */

/* --- bindings ---------------------------------------------------------------- */

typedef struct {
    int32_t seg_idx;
    uint8_t *plane;            /* cells bytes, arena-owned (Law 2)        */
    uint32_t cells;            /* == segment slot_count                  */
    uint32_t rows, cols;       /* declared visual dims (rows*cols>=cells) */
    uint64_t tail_cursor;      /* READ-marking frontier (absolute s)      */
    uint64_t head_cursor;      /* COMMIT-marking frontier (absolute s)    */
    uint64_t last_dropped;
    uint64_t lag_cells;        /* cells whose intermediate state coalesced
                                * under budget saturation (honesty)       */
    uint8_t family;
    uint8_t _pad[7];
} weft_mstream_binding_t;

/* --- frame handle (zero-copy exports) ---------------------------------------- */

typedef struct {
    const uint8_t *blob;                    /* send-ready contiguous buffer */
    size_t blob_bytes;
    const uint8_t *planes[WEFT_MSTREAM_MAX_BINDS];  /* zero-copy cell planes */
    uint32_t plane_cells[WEFT_MSTREAM_MAX_BINDS];
    uint32_t plane_rows[WEFT_MSTREAM_MAX_BINDS];
    uint32_t plane_cols[WEFT_MSTREAM_MAX_BINDS];
    uint64_t ts_ns;
    uint64_t next_deadline_ns;    /* pacer hint for the chosen FPS         */
    uint32_t kind;                /* 0 delta, 1 keyframe                   */
    uint32_t ring_count;
    uint32_t delta_count;
    uint32_t total_cells;
    uint32_t flags;
} weft_mstream_frame_t;

/* --- context ------------------------------------------------------------------- */

typedef struct {
    weft_inspect_ctx_t *insp;
    weft_mstream_binding_t *binds;      /* caller array, cap entries */
    unsigned cap, count;
    weft_mstream_blob_delta_t *deltas;  /* caller array, dcap entries */
    unsigned dcap, dcount;
    uint8_t delta_overflow;
    uint64_t frames_emitted;
    uint64_t scrape_passes;
    uint64_t cells_marked;              /* total cell updates applied */
    int64_t last_frame_ns;
    double target_fps;                  /* pacer state (default 240) */
} weft_mstream_ctx_t;

/* --- lifecycle ------------------------------------------------------------------- */

/// Bind the feeder to an inspector context plus caller-owned binding and
/// delta arrays. Zero allocations.
int weft_mstream_init(weft_mstream_ctx_t *ctx, weft_inspect_ctx_t *insp,
                      weft_mstream_binding_t *binds, unsigned cap,
                      weft_mstream_blob_delta_t *deltas, unsigned dcap);

/// Bind a ring segment for visualization. The cell plane (slot_count
/// bytes) is carved from the caller's arena. rows/cols are the declared
/// visual dims (0/0 = linear 1 x cells; rows*cols must cover cells).
/// The initial plane classification is a one-time O(cells) sweep.
int weft_mstream_bind(weft_mstream_ctx_t *ctx, int seg_idx,
                      uint32_t rows, uint32_t cols,
                      weft_inspect_arena_t *plane_arena);

int weft_mstream_unbind(weft_mstream_ctx_t *ctx, int seg_idx);

/* --- scrape pass (budgeted; Law 1) -------------------------------------------------- */

/// One incremental pass: refresh the inspector snapshot, then mark cells
/// whose absolute position crossed the tail (READ) or head (COMMITTED /
/// WRITING) frontier since the last pass, plus the write frontier
/// (CLAIMED). `max_cells` bounds the work; saturation beyond budget
/// coalesces states and increments lag counters (reported, never silent).
int weft_mstream_scrape(weft_mstream_ctx_t *ctx, uint32_t max_cells);

/* --- frame assembly (cold for the SLA: O(rings + deltas), zero SHM reads) ----------- */

/// Assemble one frame into the caller's blob arena: wire header, ring
/// blocks, delta records — and export the cell planes ZERO-COPY. The
/// delta list is consumed (emptied) atomically with emission. When the
/// delta list had overflowed since the last frame, the frame is emitted
/// as a keyframe (planes authoritative) with FLAG_DELTA_OVERFLOW set.
int weft_mstream_frame(weft_mstream_ctx_t *ctx,
                       weft_inspect_arena_t *blob_arena,
                       weft_mstream_frame_t *out);

/* --- pacer (pure arithmetic; the Studio owns sleep policy) -------------------------- */

/// Set the target frame rate (e.g. 120.0 / 240.0). No sleeping here.
int weft_mstream_set_fps(weft_mstream_ctx_t *ctx, double fps);

/// Absolute MONOTONIC_RAW deadline for the NEXT frame at the target rate
/// (the frame's next_deadline_ns equals this after each emission).
uint64_t weft_mstream_next_deadline_ns(const weft_mstream_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_INSPECT__MEMORY_STREAM_H_ */
