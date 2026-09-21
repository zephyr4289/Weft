// rmw_ring.h — the rmw_weft seqlock SPSC loan-ring over POSIX shm.
//
// WHY EXISTS: DDS middleware pays 20-60 us intra-host round trips because
// every message crosses a serialization + loopback transport stack. The
// rmw_weft transport is one /dev/shm/weft_rmw_* object per (topic,
// subscriber) pair: a lock-free single-producer/single-consumer ring whose
// slots are SEQLOCK-versioned, whose payload window is LOANED to the
// subscriber in place (zero-copy pointer identity — the address the
// subscriber reads IS the slot's payload address), and whose slot reuse is
// gated by the subscriber's tail acknowledgement, so a live loan can never
// be overwritten underneath its reader.
//
// LAYOUT (little-endian; magic + version + geometry validated on attach,
// house session-header discipline):
//   page 0   static header (128 B): magic "WFRM", version, header_size,
//            slot_count, payload_bytes, slot_stride, mapping_bytes,
//            creator_pid, created_unix_ns, registry epoch, sub instance,
//            ring-instance counter; reserved zero — unknown bits reject.
//   page 0   control block (64-B aligned, offset 128): the hot atomics —
//            head, tail_ack, published_total, dropped_total, doorbell,
//            waiters; plus pub_gid (stamped by the publisher at attach).
//   page 1   slot[0] .. slot[slot_count-1], each 64-B aligned:
//              _Atomic u64 version    seqlock; even = stable, odd = writing
//              u64 seq_id             message sequence (== published_total
//                                    at commit time; gaps = losses)
//              u32 payload_size       bytes committed in this slot
//              u32 payload_crc        0 on the hot path (see below); the
//                                    diagnostic CRC-32 stamp seam is
//                                    rmw_weft_crc32, used by the batteries
//                                    for content-level evidence
//              u64 source_ts_unix_ns  publisher stamp (CLOCK_REALTIME)
//              u8  payload[payload_bytes]
//
// PROTOCOL (memory order is part of the ABI — aarch64 included):
//   PUBLISH   writer picks slot idx = head & (slot_count-1); requires
//             head - tail_ack <= slot_count - 2 (one-slot safety margin:
//             a live loan at the tail is never the write target). It stores
//             version := v+1 (odd, release), writes payload + seq_id +
//             size + crc + ts, then version := v+2 (even, release), then
//             head := head+1 (release), then bumps doorbell (release) and
//             futex-wakes if waiters != 0.
//   TAKE      reader observes head (acquire) > cursor, reads the slot's
//             version (acquire) — must be even — and LOANS payload out.
//             Safety comes from the tail-ack invariant, not from a
//             validation copy: the writer cannot touch this slot until the
//             reader advances tail_ack. The version word is a crash/tear
//             tripwire re-checked on the copy-out path (defense in depth).
//   RETURN    reader stores tail_ack := cursor (release), granting the
//             writer every slot below cursor. Zero cost, zero syscalls.
//   FULL      RELIABLE: bounded wait ladder (pause/yield/50us sleeps,
//             deadline-checked) then RMW_RET_TIMEOUT. BEST_EFFORT: drop the
//             NEWEST message (dropped_total++). Overwriting the oldest
//             would break the loan safety invariant — the divergence from
//             DDS keep-last overwrite is deliberate and audited in D-62.
//
// LAWS:
//   Law 1  zero heap allocation and zero syscalls on the data path —
//          publish/take/return are pure shared-memory atomics; the futex
//          doorbell fires ONLY when a waiter has parked (waiters != 0).
//   Law 2  zero-copy — the subscriber's loaned pointer aliases the slot
//          payload directly; tests assert the offset identity
//          (loan - ring_base) is byte-equal across processes.
//   Law 3  deterministic degradation — full rings, torn writers, and dead
//          peers produce counted drops / honest return codes, never
//          corruption, never blocking past the caller's deadline.
//
// Cross-process atomics: C11 _Atomic over MAP_SHARED is lock-free and
// cache-coherent on x86_64 and aarch64 for the 32/64-bit naturally aligned
// words used here; weft_rmw_ring_attach FAILS CLOSED if any control word is
// not lock-free or not 64-B aligned (never a silent libc-lock fallback).

#ifndef RMW_WEFT__RMW_RING_H_
#define RMW_WEFT__RMW_RING_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "rmw_weft/rmw_weft_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RMW_WEFT_RING_MAGIC 0x4d465257u  /* "WFRM" little-endian */
#define RMW_WEFT_RING_VERSION 1u
#define RMW_WEFT_RING_HEADER_BYTES 128u
#define RMW_WEFT_RING_CTRL_BYTES 64u
#define RMW_WEFT_RING_SLOTS_OFFSET 4096u
#define RMW_WEFT_SLOT_HDR_BYTES 64u  /* slot: metadata pad, then payload */

_Static_assert(RMW_WEFT_RING_SLOTS_OFFSET >=
                   RMW_WEFT_RING_HEADER_BYTES + RMW_WEFT_RING_CTRL_BYTES,
               "ctrl block must fit before the page-aligned slots");

typedef struct rmw_ring_header {
    /* --- static header: written once by the creator, read-only after --- */
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t slot_count;
    uint32_t payload_bytes;
    uint32_t slot_stride;      /* 64-aligned: hdr + payload, rounded up   */
    uint32_t _pad0;
    uint64_t mapping_bytes;    /* SLOTS_OFFSET + slot_count * stride      */
    uint32_t creator_pid;
    uint32_t _pad1;
    uint64_t created_unix_ns;
    uint64_t registry_epoch;   /* anti-PID-reuse generation stamp        */
    uint32_t sub_instance;     /* subscriber instance that owns the tail  */
    uint32_t reliability;      /* RMW_QOS_POLICY_RELIABILITY_* snapshot  */
    uint8_t reserved[64];      /* must be zero; unknown bits reject       */
} rmw_ring_header_t;

_Static_assert(sizeof(rmw_ring_header_t) == RMW_WEFT_RING_HEADER_BYTES,
               "rmw_ring_header_t must be exactly 128 bytes");

typedef struct rmw_ring_ctrl {
    /* --- hot words: 64-B aligned, lock-free required on attach --- */
    _Atomic uint64_t head;             /* next slot index to write        */
    _Atomic uint64_t tail_ack;         /* first slot index not yet freed  */
    _Atomic uint64_t published_total;  /* committed messages ever         */
    _Atomic uint64_t dropped_total;    /* publisher-side drops (honest)   */
    _Atomic uint32_t doorbell;         /* futex word: bumped on publish   */
    _Atomic uint32_t waiters;          /* parked readers (wake decision)  */
    _Atomic uint32_t state;            /* 0 live, 2 dead-peer              */
    uint32_t _pad;
    _Atomic uint64_t pub_gid_a;        /* publisher gid digest, stamped   */
    _Atomic uint64_t pub_gid_b;        /* by the publisher at attach      */
} rmw_ring_ctrl_t;

_Static_assert(sizeof(rmw_ring_ctrl_t) == RMW_WEFT_RING_CTRL_BYTES,
               "rmw_ring_ctrl_t must be exactly 64 bytes");

typedef struct rmw_ring_slot {
    _Atomic uint64_t version;          /* even = stable; odd = writing    */
    uint64_t seq_id;                   /* message sequence id             */
    uint32_t payload_size;
    uint32_t payload_crc;
    uint64_t source_ts_unix_ns;
    uint8_t payload[];                 /* payload_bytes capacity          */
} rmw_ring_slot_t;

/// A mapped ring (creator or attacher). POD; destroy to unmap.
typedef struct rmw_ring_map {
    uint8_t *base;              /* mapping start (the static header)      */
    rmw_ring_ctrl_t *ctrl;      /* base + 128                             */
    rmw_ring_slot_t *slots;     /* base + SLOTS_OFFSET                    */
    const rmw_ring_header_t *hdr;  /* alias of base, const view           */
    size_t mapping_bytes;
    int fd;                     /* POSIX shm fd; -1 never happens here    */
    int creator;                /* 1 = created here (destroy unlinks)     */
    char name[80];              /* shm object name without leading '/'    */
} rmw_ring_map_t;

/// Geometry sanity for ring creation (power-of-two slots, bounded payload).
int rmw_ring_geometry_ok(uint32_t slot_count, uint32_t payload_bytes);

/// Create a named ring (/dev/shm/<name>). Zero-fills, stamps the header,
/// validates lock-freedom + alignment. Returns 0 / -errno-style negative.
int rmw_ring_create(const char *name, uint32_t slot_count,
                    uint32_t payload_bytes, uint32_t sub_instance,
                    uint32_t reliability, uint64_t registry_epoch,
                    rmw_ring_map_t *out);

/// Attach by name (publisher side). Validates magic/version/geometry/
/// mapping size EXACTLY; fails closed on mismatch. Read-write mapping.
int rmw_ring_attach(const char *name, uint32_t expected_slot_count,
                    uint32_t expected_payload_bytes, rmw_ring_map_t *out);

/// Creator unlink + unmap; attacher unmap only. Idempotent.
void rmw_ring_destroy(rmw_ring_map_t *m);

/// Publisher stamps its gid digest into the control block (attach time).
void rmw_ring_stamp_publisher(rmw_ring_map_t *m, uint64_t gid_a,
                              uint64_t gid_b);

/// Hot path: publish a message (copy-in variant — one copy per ring).
/// Returns 0, -ETIMEOUT (RELIABLE ladder exhausted), -EAGAIN would-block
/// (BEST_EFFORT drop, dropped_total already incremented), -EBADF state.
int rmw_ring_publish(rmw_ring_map_t *m, const void *msg, uint32_t size,
                     int reliable, int64_t deadline_ns);

/// Hot path: borrow the next slot payload for zero-copy writing.
/// Returns 0 with *slot_out/*payload_out, -EAGAIN (full, honest drop
/// already counted for BEST_EFFORT), -ETIMEOUT (RELIABLE ladder).
int rmw_ring_borrow(rmw_ring_map_t *m, int reliable, int64_t deadline_ns,
                    rmw_ring_slot_t **slot_out, uint8_t **payload_out);

/// Hot path: commit a borrowed slot (payload already written in place).
int rmw_ring_commit(rmw_ring_map_t *m, rmw_ring_slot_t *slot,
                    uint32_t size);

/// Hot path: try-take without waiting. Returns 1 = message ready at cursor
/// (slot_out/meta filled: version must be even), 0 = empty, -1 = torn
/// (writer crashed mid-commit; caller may skip via rmw_ring_skip).
int rmw_ring_try_take(rmw_ring_map_t *m, uint64_t cursor,
                      rmw_ring_slot_t **slot_out, uint64_t *seq_id,
                      uint32_t *size, uint32_t *crc,
                      uint64_t *source_ts);

/// Hot path: advance the tail acknowledgement (loan return). Pure store.
void rmw_ring_advance_tail(rmw_ring_map_t *m, uint64_t new_tail);

/// Reader parking: hybrid spin-then-futex wait until head != cursor or the
/// deadline passes. Returns 1 ready / 0 timeout. The futex word is the
/// ring's own doorbell (shared memory, no FUTEX_PRIVATE_FLAG).
int rmw_ring_wait(rmw_ring_map_t *m, uint64_t cursor, int64_t deadline_ns);

/// Wait ladder bound (spin phase) — config-tunable, default 40 us.
int64_t rmw_weft_spin_budget_ns(void);

/// Slot payload address (the loaned zero-copy pointer).
uint8_t *rmw_ring_slot_payload(rmw_ring_slot_t *slot);

/// CRC-32 (IEEE reflected, poly 0xEDB88320), table initialized once.
uint32_t rmw_weft_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // RMW_WEFT__RMW_RING_H_
