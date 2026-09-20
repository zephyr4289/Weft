// weft_trace.h — RFC 0016: continuous lock-free flight recorder, C reference.
//
// WHY EXISTS: RFC-0014 standardized the .weftrec EVENT CONTAINER (a pure
// function of a kernel scenario — byte-identical across ports). A container
// is not a recorder. Production concurrency failures are diagnosed from the
// last N kernel decisions BEFORE the failure was noticed, on threads that
// never stopped publishing. This module is that recorder: per-producer
// lossy SPSC shards with wait-free emission (no CAS, no loop, no alloc —
// Law 1, Law 2), overwrite-oldest backlog policy, K-way merge drain, and
// export to BOTH surfaces of RFC-0016 §1:
//
//   .weftrec v4  — kernel kinds (1-8) only; byte-identity with RFC-0014
//   .wsid   v1   — the sidecar timeline (timestamps + runtime kinds)
//
// CONTENTION MODEL (the "zero lock contention" claim, made precise):
// every producer thread owns one shard exclusively. The producer writes
// its slot's payload words and publishes them with a SINGLE Release store
// of the slot stamp. It never reads shared mutable state — there is no
// shared cursor to CAS. Contention is structurally absent (one dirty cache
// line per emit, the line the producer had to write anyway), not merely
// small. The consumer (drain) is the only cross-thread reader; it is
// seqlock-stamped, resync-on-lap, and bounded (RFC-0016 §5).
//
// LAWS: emit is wait-free and allocation-free (L1: there is no loop to
// bound; L2: no allocator on the path). init is the only allocator (the
// kernel precedent). The frozen kernel (weft.c/weft.h) is untouched — this
// module observes through weft.h's public surface and lives in the driver
// layer (Law 3). Timestamps are INJECTED by callers (deterministic tests —
// Law 4); the v4 export remains a pure function of the kernel scenario.

#ifndef WEFT_TRACE_H
#define WEFT_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "trace_rec.h"  // RFC-0014 codec: kinds, packing, container writer

// ---------------------------------------------------------------------------
// Tunables (declared bounds — RFC-0016 §2)
// ---------------------------------------------------------------------------

#define WEFT_TRACE_SHARDS_MAX 8u     // producer lanes
#define WEFT_TRACE_SLOT_SIZE  32u    // bytes, two per 64B line
#define WEFT_TRACE_CAP_MIN    64u    // per-shard slot count (power of two)
#define WEFT_TRACE_CAP_MAX    (1u << 20)

// Sidecar runtime-kind registry (RFC-0016 §4 — OPEN but declared here
// first; a new kind extends this table in a new RFC). Runtime kinds live
// in [16, 0x8000) — DISJOINT from the kernel kinds 1..8 (the u16 kind
// space carries both surfaces; a collision would make the v4 export
// filter misclassify, so the gap is enforced by the test suite).
#define WEFT_XT_VSYNC_TICK      16u
#define WEFT_XT_GOVERNOR_ACTION 17u
#define WEFT_XT_TREND_VERDICT   18u
#define WEFT_XT_GC_PAUSE        19u
#define WEFT_XT_RING_DEPTH      20u
#define WEFT_XT_FRESHNESS       21u
#define WEFT_XT_CLAIM_LATENCY   22u
#define WEFT_XT_DROP_BURST      23u
#define WEFT_XT_MARKER          24u
#define WEFT_XT_BASE            16u

// Sidecar file constants.
#define WEFT_SIDECAR_MAGIC    0x44495357u  // "WSID" LE
#define WEFT_SIDECAR_VERSION  1u
#define WEFT_SIDECAR_HDR_SIZE 32u
#define WEFT_SIDECAR_REC_SIZE 20u
#define WEFT_SIDECAR_NO_REF   0xFFFFFFFFu

// Producer lane conventions (binding is a caller discipline; the recorder
// only needs shard < WEFT_TRACE_SHARDS_MAX).
#define WEFT_TP_WRITER   0u
#define WEFT_TP_READER   1u
#define WEFT_TP_GOVERNOR 2u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// One timeline observation, as returned by drain. The in-memory form of a
/// sidecar record (§4) before packing. `back_ref` is meaningful only when
/// the event is a kernel kind routed through the recorder.
typedef struct {
    uint64_t t_ns;       // injected monotonic timestamp
    uint32_t back_ref;   // v4 event index, or WEFT_SIDECAR_NO_REF
    uint16_t producer;   // shard id
    uint16_t kind;       // kernel kind (1-8) or runtime kind (registry)
    uint32_t data;       // kind-scoped
    uint16_t aux;        // kernel kinds: as in RFC-0014; runtime: 0
} weft_trace_obs;

/// One producer shard. Thread-private to its producer except during drain.
/// Layout note: head is producer-private; `stamp` lives INSIDE the slot
/// (the publish word); the consumer keeps its own tickets outside.
typedef struct {
    void*           slots;      // cap * 32B slot storage (opaque; 64B-aligned
                                // via init; mmap-able via init_at, §6)
    uint32_t        cap;        // power of two
    uint32_t        cap_mask;
    uint64_t        head;       // producer-private next ticket
    // consumer-side (drain) private state — stored here for locality,
    // written ONLY by the drainer:
    uint64_t        c_ticket;   // last consumed ticket + 1
    uint64_t        t_lap;      // events skipped on resync (this shard)
    uint64_t        t_torn;     // torn-read retries consumed
} weft_trace_shard;

/// The recorder. Shards are heap-allocated at init (the ONLY allocation),
/// or placed by weft_trace_init_at (mmap host path — RFC-0016 §6).
typedef struct {
    weft_trace_shard shard[WEFT_TRACE_SHARDS_MAX];
    uint8_t          shard_used[WEFT_TRACE_SHARDS_MAX];
    uint8_t          shard_owned[WEFT_TRACE_SHARDS_MAX]; // 1 = init allocated
                                                          // (destroy frees);
                                                          // 0 = init_at placed
                                                          // (destroy MUST NOT
                                                          // free — the mmap
                                                          // host path, §6)
    // The v4 export needs kernel events in merged order; drain supplies
    // them. Counters are advisory (Relaxed, AXIOM T):
    _Atomic uint64_t t_emit;      // total emit() calls
    _Atomic uint64_t t_drain;     // total drain() calls
} weft_trace_t;

// ---------------------------------------------------------------------------
// Lifecycle (init allocates; everything else is Law 2 clean)
// ---------------------------------------------------------------------------

/// Create a recorder with per-shard capacity `cap` (power of two, clamped
/// to [CAP_MIN, CAP_MAX]). All 8 shards are allocated (uniform layout keeps
/// the mmap story honest); unused shards cost only their slot array.
/// Returns 0 on success, -1 on allocation failure. `t_ns` values are
/// caller-injected everywhere (no clock is read anywhere in this file).
int weft_trace_init(weft_trace_t* r, uint32_t cap);

/// Placement init (mmap host path, RFC-0016 §6): the caller provides a
/// contiguous region holding all 8 shards' slot storage
/// (WEFT_TRACE_SHARDS_MAX * cap * 32 bytes). Nothing is allocated — the
/// zero-alloc operation proof (T6) and the crash-tolerant post-mortem path
/// both go through here. Returns 0 / -1 (bad geometry or region too small).
int weft_trace_init_at(weft_trace_t* r, uint32_t cap, void* storage,
                       size_t storage_len);

/// Destroy. Idempotent; frees what init allocated (init_at storage is the
/// caller's — untouched).
void weft_trace_destroy(weft_trace_t* r);

// ---------------------------------------------------------------------------
// Emission (producer thread only — the shard-binding discipline, §2)
// ---------------------------------------------------------------------------

/// Wait-free emission. `kind` may be a kernel kind (1-8; then `aux` follows
/// RFC-0014 semantics and back_ref wiring on export) or a runtime kind from
/// the §4 registry. NEVER blocks, loops, or allocates. Publishes with one
/// Release store. If the shard slot being written holds the oldest events,
/// those events are overwritten — the lap is accounted at drain time.
///
/// IMPORTANT (RFC-0016 §2): call only from the thread that owns shard
/// `producer`. Cross-thread emission is a caller error (the debug build's
/// shard-binding probe trips it; the release build defines the behavior as
/// "advisory telemetry with a torn line" — do not).
void weft_trace_emit(weft_trace_t* r, uint32_t producer, uint16_t kind,
                     uint16_t aux, uint32_t data, uint64_t t_ns);

/// Convenience for the kernel lanes: emit AND remember the v4 back-ref of
/// the kernel event so export can pair container and sidecar records.
/// (Implementation detail: back_refs are assigned at EXPORT time by merged
/// order — see weft_trace_export_v4. This function is emit() with the
/// kernel-kind route; it exists so call sites read intent, not magic.)
void weft_trace_emit_kernel(weft_trace_t* r, uint32_t producer,
                            const weft_trace_event* ev, uint64_t t_ns);

// ---------------------------------------------------------------------------
// Drain (single consumer; RFC-0016 §5)
// ---------------------------------------------------------------------------

/// Advisory pending count (sum over shards of a non-destructive peek-walk,
/// bounded by shard capacity). EXACT when no producer is mid-emit — the
/// sizing contract for export (below); advisory under live emission
/// (AXIOM T). O(cap) per shard: a consumer-side sizing aid, not a hot path.
size_t weft_trace_pending(weft_trace_t* r);

/// Merge-drain up to `out_cap` observations across all shards, ordered by
/// (t_ns, producer, arrival). Returns the number written. Bounded:
/// O(out_cap · log SHARDS). Zero allocation. Lapped/torn events are
/// skipped and counted (t_lap on each shard; never exported torn).
size_t weft_trace_drain(weft_trace_t* r, weft_trace_obs* out, size_t out_cap);

// ---------------------------------------------------------------------------
// Export (RFC-0016 §6) — both surfaces, caller-buffered
// ---------------------------------------------------------------------------

/// Export kernel kinds as a valid .weftrec v4 container (RFC-0014 codec).
/// Drains EVERYTHING first (the export is a snapshot point), filters kinds
/// 1-8 in merged order, assigns v4 indices, and returns the container byte
/// length written (0 on overflow — `cap` too small; nothing is lost: the
/// drained observations are returned through `obs_out`/`obs_n` so the
/// caller may retry with a larger buffer without re-emitting).
///
///   needed = WEFT_TRACE_HDR_SIZE + 12 * kernel_event_count
size_t weft_trace_export_v4(weft_trace_t* r, uint8_t* buf, size_t cap,
                            weft_trace_obs* obs_out, size_t obs_cap,
                            size_t* obs_n);

/// Export the full drained observation set as a .wsid v1 sidecar.
/// `t_epoch_ns` is recorded in the header (0 in deterministic tests).
/// Returns the byte length written, 0 on overflow.
///
///   needed = WEFT_SIDECAR_HDR_SIZE + 20 * obs_count
size_t weft_trace_export_sidecar(const weft_trace_obs* obs, size_t obs_n,
                                 uint8_t* buf, size_t cap, uint64_t t_epoch_ns);

/// Byte-length helper for sizing buffers before the drain/export pair.
size_t weft_trace_needed_v4(size_t kernel_events);
size_t weft_trace_needed_sidecar(size_t obs_n);

#ifdef WEFT_TRACE_DEBUG
/// Debug build only: count of §2 shard-binding violations (cross-thread
/// emit on a bound shard). Test-observable; the release build compiles the
/// whole probe out.
uint64_t weft_trace_debug_binding_violations(void);
#endif

#endif  // WEFT_TRACE_H
