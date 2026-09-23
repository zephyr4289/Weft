// weft_shm_inspector.h — the live shared-memory attach engine (Pillar 7).
//
// WHY EXISTS: Weft Studio must peer into the beating heart of running
// zero-copy rings — rmw_weft ROS 2 topics, cluster mesh sessions, whatever
// the next pillar mints — WITHOUT pausing a single producer. Every existing
// attach path (rmw_ring_attach, weft_shm_attach) takes a READ-WRITE
// mapping and a priori geometry, which is correct for participants and
// wrong for an observer: a monitor that can write is a monitor that can
// corrupt. This engine is the observer's path: scan /dev/shm, classify by
// wire magic, attach PROT_READ, validate geometry EXACTLY against the
// frozen public layouts, and snapshot the live control words with acquire
// fences and seqlock-style double reads.
//
// SEGMENT FAMILIES (classified by MAGIC first, name second — a name is a
// hint, a magic is a contract):
//   WFRM  weft_rmw_d<dom>_t<hash>_s<inst>   rmw_weft ROS 2 loan-ring
//         (layout: core/c/adapters/rmw_weft/include/rmw_weft/rmw_ring.h —
//          parsed through that PUBLIC header, so the ABI cannot drift)
//   WFRR  weft_rmw_d<dom>_registry          rmw_weft topic registry
//         (public structs from rmw_registry.h — same no-drift discipline)
//   WFSH  any named weft_shm session        cluster mesh ring (RFC-0004
//         fanout layout; parsed at the documented byte offsets)
//   WFRE  weft_registry_v1                  cluster mesh registry
//         (sessions discovered through the REAL weft_ipc API — the entry
//          layout is private to weft_ipc.c, so we link it, we do not
//          re-declare it: ABI fidelity by construction)
//   ?     any other weft_* object           flagged UNKNOWN + magic hex,
//         never guessed at, never attached beyond the 4-byte probe.
//
// HONESTY NOTE (weft-spectrum): the spectrum pillar (P5) exposes no named
// /dev/shm segments in this tree — its buffers are in-process SIMD/GPU
// workspaces. The classifier therefore has no WFSx case to parse; a future
// spectrum segment would land in UNKNOWN with its magic reported, which is
// the correct posture until that pillar publishes a wire contract. Declared
// here so the audit can quote it.
//
// LAW 1 (this module): every mapping this engine creates is PROT_READ |
// MAP_SHARED. No lock is acquired, no futex is touched, no watched word is
// ever stored to or RMW'd. The ONLY effect of an inspector pass on the
// observed system is cache-line reads — the batteries prove the stronger
// statement (zero corruption, zero reliable drops, sub-0.5% CPU) on live
// multi-process traffic.
//
// LAW 2: scan/attach may syscall (opendir/shm_open/mmap — cold paths);
// weft_inspect_scrape() and weft_inspect_peek_slot() are pure userspace
// loads over pinned mappings: zero syscalls, zero allocations, zero writes.
//
// LAW 3: every snapshot carries weft_inspect_now_ns() stamps; torn ctrl
// reads are retried ONCE and then honestly reported (soft snapshots are
// marked, never silently accepted).
//
// LAW 4: -std=c11 -Wall -Wextra -Werror -pedantic clean; no VLA; the
// segment table is a fixed caller-owned array; unknown bits reject.

#ifndef WEFT_INSPECT__SHM_INSPECTOR_H_
#define WEFT_INSPECT__SHM_INSPECTOR_H_

#include <stddef.h>
#include <stdint.h>

#include "weft_inspector/weft_inspect_common.h"

/* frozen public layouts of the segments we parse (read-only includes —
 * these headers ARE the ABI; static asserts inside them pin the sizes) */
#include "rmw_weft/rmw_ring.h"      /* WFRM ring: header/ctrl/slot        */
#include "rmw_weft/rmw_registry.h"  /* WFRR registry: topics/pubs/subs    */

#ifdef __cplusplus
extern "C" {
#endif

/* --- capacity defaults (caller may size differently) --------------------- */

/// Segment name capacity (matches the longest house name, WFSH's 96).
#define WEFT_INSPECT_NAME_MAX 96u

/// Default segment-table capacity. 64 covers the studio's development
/// posture (dozens of topics x subscribers); larger meshes pass their own
/// array — the struct never reallocates (Law 2).
#define WEFT_INSPECT_MAX_SEGMENTS 64u

/* --- families -------------------------------------------------------------- */

typedef enum {
    WEFT_INSPECT_FAMILY_UNKNOWN = 0,
    WEFT_INSPECT_FAMILY_RMW_RING = 1,          /* WFRM */
    WEFT_INSPECT_FAMILY_RMW_REGISTRY = 2,      /* WFRR */
    WEFT_INSPECT_FAMILY_CLUSTER_RING = 3,      /* WFSH */
    WEFT_INSPECT_FAMILY_CLUSTER_REGISTRY = 4,  /* WFRE */
} weft_inspect_family_t;

/* Why a segment is present but not attached (honest, per-segment):
 * ABI mismatches are FLAGGED, not fatal — a live mesh may legitimately
 * contain an object from a newer/older wire version, and the studio must
 * show it as such rather than skip it silently. */
typedef enum {
    WEFT_INSPECT_EXCL_NONE = 0,
    WEFT_INSPECT_EXCL_ABI = 1,       /* magic/version/geometry mismatch   */
    WEFT_INSPECT_EXCL_TABLE_FULL = 2,/* segment table at capacity         */
    WEFT_INSPECT_EXCL_TOO_SMALL = 3, /* smaller than its family's header  */
} weft_inspect_excl_t;

/* --- live snapshot of one segment ------------------------------------------ */

/// One watched segment. POD; the fixed table lives in caller memory.
/// Current control words are refreshed by weft_inspect_scrape(); the
/// geometry fields are filled once at attach and never mutated.
typedef struct weft_inspect_segment {
    char name[WEFT_INSPECT_NAME_MAX];   /* no leading '/'; "weft_..."     */
    uint8_t family;                     /* weft_inspect_family_t          */
    uint8_t attached;                   /* 1 = mapped PROT_READ           */
    uint8_t excluded;                   /* weft_inspect_excl_t reason     */
    uint8_t probe_magic[4];             /* raw magic bytes (UNKNOWN case) */

    /* mapping (attached segments only; fd >= 0) */
    uint8_t *base;                      /* mapping start                  */
    size_t map_bytes;                   /* fstat size at attach           */
    int fd;                             /* O_RDONLY shm fd; -1 otherwise  */

    /* ring geometry — WFRM and WFSH */
    uint32_t slot_count;
    uint32_t payload_bytes;
    uint32_t slot_stride;               /* WFRM only                      */
    uint64_t mapping_bytes_hdr;         /* creator-declared size (WFRM)   */
    uint64_t ring_bytes_hdr;            /* WFSH only                      */
    uint32_t creator_pid;               /* WFRM: subscriber creator       */
    uint64_t created_unix_ns;
    uint32_t sub_instance;              /* WFRM only                      */
    uint32_t reliability;               /* WFRM only (QoS snapshot)       */

    /* live control words — refreshed by scrape(); WFRM */
    uint64_t head;
    uint64_t tail_ack;
    uint64_t published_total;
    uint64_t dropped_total;
    uint32_t doorbell;
    uint32_t waiters;
    uint32_t ring_state;                /* 0 live, 2 dead-peer            */
    /* live control words — WFSH */
    uint64_t latest_seq;
    uint64_t publishes;

    /* registry census — WFRR/WFRE (refreshed by topology passes) */
    uint32_t reg_topics_active;
    uint32_t reg_pubs_active;
    uint32_t reg_subs_active;

    /* snapshot quality (Law 3): 1 when the ctrl double-read bracket saw
     * movement even after the bounded retry — advisory data, marked. */
    uint8_t soft_snapshot;
    uint8_t scan_found;   /* scan() scratch: 1 = object seen this sweep  */
    uint8_t _pad[6];
} weft_inspect_segment_t;

_Static_assert(sizeof(weft_inspect_segment_t) % 8u == 0u,
               "segment record stays 8-byte aligned for atomic word copies");

/* --- context --------------------------------------------------------------- */

/// Inspector context. Caller-owned storage for the segment table; the
/// context itself holds no pointers into heap. Init/destroy bracket the
/// mapping lifetime; scrape/peek between them are allocation-free.
typedef struct weft_inspect_ctx {
    weft_inspect_segment_t *segs;
    unsigned cap;
    unsigned count;
    /* whole-run telemetry (honest counters, monotone) */
    uint64_t scrape_passes;      /* completed scrape() calls             */
    uint64_t torn_ctrl_retries;  /* bracket retries (pass-level)         */
    uint64_t soft_snapshots;     /* passes that stayed moving            */
    uint64_t peek_attempts;      /* slot peeks attempted                 */
    uint64_t peek_torn;          /* slot peeks that returned -EAGAIN     */
    uint64_t scan_overflow;      /* segments skipped: table full         */
} weft_inspect_ctx_t;

/* --- lifecycle ------------------------------------------------------------- */

/// Bind the context to a caller-owned segment table. The table is NOT
/// copied; it must outlive the context. Returns 0 / -INVALID.
int weft_inspect_init(weft_inspect_ctx_t *ctx,
                      weft_inspect_segment_t *table, unsigned cap);

/// Detach everything (munmap + close). Idempotent; never unlinks — an
/// inspector has no right to remove another process's object (monitor
/// posture; crash recovery belongs to the participants' own sweeps).
void weft_inspect_destroy(weft_inspect_ctx_t *ctx);

/* --- discovery ------------------------------------------------------------- */

/// One /dev/shm sweep: enumerate "weft_*" objects, classify by magic,
/// validate geometry EXACTLY, attach PROT_READ. Re-scanning an already
/// attached name is a no-op (stable index). Objects that vanished since
/// the last scan are pruned from the table (indices may compact — the
/// studio should re-resolve by name after every scan; scrape() between
/// scans is index-stable). Returns the number of segments now watched,
/// or -errno-style on a hard scan failure (opendir).
int weft_inspect_scan(weft_inspect_ctx_t *ctx);

/// Resolve a segment index by exact name. -1 when absent.
int weft_inspect_find(const weft_inspect_ctx_t *ctx, const char *name);

/// Attach a segment by an ALREADY-OPEN fd (memfd_create / SCM_RIGHTS
/// received / shm fd the studio holds). Same magic-first classification
/// and EXACT geometry validation as scan(); the name is the studio's
/// display label. This is the fd-transport discipline the cluster mesh
/// itself uses (WEFT_IPC_TRANSPORT_MEMFD): a 1M-slot ring does not fit
/// a 64 MiB container tmpfs, but an anonymous memfd pages from RAM —
/// the bench attaches exactly that way. Returns the new segment index
/// or a negative error; the fd is owned by the context after success.
int weft_inspect_attach_fd(weft_inspect_ctx_t *ctx, int fd,
                           const char *name);

/// Explicit detach of one segment (the studio stops watching a topic).
/// Table compacts; indices shift. Returns 0 / -INVALID.
int weft_inspect_detach(weft_inspect_ctx_t *ctx, unsigned idx);

/* --- live scraping (hot path: zero syscall, zero alloc, zero store) ------- */

/// One snapshot pass over every attached ring: acquire-fenced control-word
/// reads bracketed by a head double-read (WFRM) or latest_seq double-read
/// (WFSH). Torn brackets retry once; a still-moving ring is marked
/// soft_snapshot (advisory, counted, never silently accepted). Returns
/// passes-in-flight errors: 0, or -INVALID.
int weft_inspect_scrape(weft_inspect_ctx_t *ctx);

/* --- read-only peek seam (Law 1's "safe, non-destructive frame reading") -- */

/// Slot metadata observed under the seqlock bracket: version read
/// (acquire), metadata plain-read, version re-read (acquire); a version
/// change or an odd version means a writer is mid-commit — bounded retry,
/// then honest -EAGAIN (counted in ctx->peek_torn). `cursor` is an
/// absolute slot index; the caller learns valid cursors from the segment's
/// head/tail_ack snapshot. Returns 1 (meta filled), 0 (cursor >= head:
/// not yet published), -EAGAIN (torn), -INVALID/-NOENT (bad args/segment).
typedef struct {
    uint64_t version;        /* even, stable at observation time        */
    uint64_t seq_id;         /* ring sequence stamp                     */
    uint32_t payload_size;
    uint32_t payload_crc;
    uint64_t source_ts_unix_ns;
    int64_t observed_ns;     /* Law 3 stamp                             */
    uint8_t recycled;        /* 1 = seq_id != cursor (slot wrapped)     */
    uint8_t _pad[7];
} weft_inspect_slot_meta_t;

int weft_inspect_peek_slot(const weft_inspect_ctx_t *ctx, unsigned idx,
                           uint64_t cursor, weft_inspect_slot_meta_t *out);

/// Zero-copy read-only payload view: the address the PUBLISHER wrote,
/// aliased directly in this process (the loan-offset identity the P6
/// batteries proved for subscribers, now held by the observer). The
/// caller brackets its own reads against *version_out (seqlock discipline
/// — defense in depth; the loan invariant already protects committed
/// slots). Returns 0, or the peek errors above. *version_out is the LIVE
/// version word address (never dereferenced for write; const view).
int weft_inspect_slot_payload_ro(const weft_inspect_ctx_t *ctx,
                                 unsigned idx, uint64_t cursor,
                                 const uint8_t **payload_out,
                                 const uint64_t **version_out,
                                 uint32_t *size_out);

/* --- topology --------------------------------------------------------------- */

/// Topic rows harvested from every attached WFRR registry (public struct
/// discipline — parsed with rmw_registry_topic_t, counted only when the
/// topic state is ACTIVE). Ring names come from the sub records; the
/// studio correlates them against scan() results by name.
typedef struct {
    char topic[RMW_WEFT_TOPIC_NAME_MAX];
    uint64_t type_hash;
    uint32_t msg_size;
    uint32_t pubs;
    uint32_t subs;
    uint32_t rings_mapped;   /* sub records whose ring is attached here  */
    uint32_t _pad;
} weft_inspect_topic_row_t;

/// Census of one scrape-time topology pass. All numbers are measured
/// facts of THIS pass; advisory fields are labeled at the source.
typedef struct {
    int64_t observed_ns;
    uint32_t rmw_registries;      /* WFRR attached                        */
    uint32_t rmw_topics_active;   /* summed over registries               */
    uint32_t rmw_pubs_active;
    uint32_t rmw_subs_active;
    uint32_t cluster_registries;  /* WFRE attached                        */
    uint32_t cluster_sessions;    /* ACTIVE sessions (real weft_ipc API)  */
    uint32_t cluster_stale;       /* heartbeat-stale sessions (advisory)  */
    uint32_t rings_watched;       /* attached WFRM + WFSH                 */
    uint32_t unknown_flagged;     /* classified UNKNOWN this scan         */
    uint64_t ring_payload_bytes;  /* sum(slot_count * payload_bytes)      */
} weft_inspect_topology_t;

/// Fill the census + (when rows/row_cap are given) topic rows. The cluster
/// session table is read through the REAL weft_ipc discovery API (linked
/// read-only; its entry layout is private by design). Zero allocations.
/// Returns the topic-row count (>= 0) or a negative error code.
int weft_inspect_topology(const weft_inspect_ctx_t *ctx,
                          weft_inspect_topic_row_t *rows, unsigned row_cap,
                          weft_inspect_topology_t *out);

#ifdef __cplusplus
}
#endif

#endif  /* WEFT_INSPECT__SHM_INSPECTOR_H_ */
