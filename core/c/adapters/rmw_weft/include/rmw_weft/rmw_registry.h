// rmw_registry.h — the rmw_weft cross-process topic registry.
//
// WHY EXISTS: publishers and subscribers rendezvous by topic name without
// a daemon, a broker, or a config service: one /dev/shm registry object
// per ROS domain holds a fixed-capacity table of topic entries, each
// carrying per-subscriber records (ring name + owner pid) and per-publisher
// records (gid digest). All slot claiming is CAS state-machine only —
// FREE -> CLAIMED -> ACTIVE -> CLOSED — no mutexes, matching the lock-free
// house discipline. Discovery is O(topics) on cold paths only; the publish
// hot path checks ONE atomic (subs_version) and rescan is the slow path.
//
// LAYOUT (/dev/shm/weft_rmw_d<domain>_registry, one page + entry table):
//   header  : magic "WFRR", version, geometry, epoch, activity futex word,
//             attach_count, creator pid, created ns
//   topics[]: RMW_WEFT_MAX_TOPICS entries, each:
//               _Atomic u32 state (FREE/CLAIMED/ACTIVE/CLOSED)
//               name[96], name_hash, type_hash, msg_size
//               _Atomic u32 subs_version  (bumped when the sub list mutates)
//               _Atomic u32 pubs_version
//               sub records[16]: state, sub_instance, sub_pid,
//                                ring_name[80], ring slots/payload snapshot
//               pub records[8]:  state, pub_instance, pub_pid, gid digest
//
// ORPHAN SWEEP: every rmw_init sweeps registry records whose owner pid is
// dead (kill(pid,0) == -1 && errno == ESRCH), unlinks their rings, and
// closes their slots — a crashed participant heals on the next init. PID
// reuse is mitigated by the registry epoch stamped into every ring header
// (rings older than the current epoch with a dead pid are reclaimed; a
// live pid is never reclaimed). The window between "pid reused" and
// "epoch check" is a declared residual, audited in D-62 §C.3.
//
// TOPIC-CLAIM LOCK (v2, D-62 §C.2): find-or-create on the topic table is
// a TOCTOU otherwise — two processes creating endpoints for the SAME
// topic concurrently can each claim a DIFFERENT entry (the finder reads
// a CLAIMED entry whose name is still being written, skips it, claims
// its own), splitting the topic into two invisible halves: publishers
// on one entry, subscribers on the other, publishes silently no-op into
// zero rings. The claim lock serializes add/remove/sweep (COLD paths
// only — entity create/destroy and init); it is a CAS machine holding
// the owner pid, stealable when the owner is dead, and the publish/take
// hot paths NEVER touch it. Same-process callers are serialized by rcl's
// context lock discipline upstream (documented contract).
//
// LAW: registry mutation happens only on create/destroy (cold); the hot
// path reads one relaxed atomic per publish and nothing else.

#ifndef RMW_WEFT__RMW_REGISTRY_H_
#define RMW_WEFT__RMW_REGISTRY_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "rmw_weft/rmw_weft_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RMW_WEFT_REGISTRY_MAGIC 0x52524657u  /* "WFRR" little-endian */
#define RMW_WEFT_REGISTRY_VERSION 2u /* v2: claim_lock + claim_steals words */

enum {
    RMW_WEFT_SLOT_FREE = 0,
    RMW_WEFT_SLOT_CLAIMED = 1,
    RMW_WEFT_SLOT_ACTIVE = 2,
    RMW_WEFT_SLOT_CLOSED = 3,
};

typedef struct rmw_registry_sub_record {
    _Atomic uint32_t state;
    uint32_t sub_instance;
    uint32_t sub_pid;
    char ring_name[RMW_WEFT_RING_NAME_MAX];
    uint32_t ring_slots;
    uint32_t ring_payload;
    uint32_t _pad;
} rmw_registry_sub_record_t;

typedef struct rmw_registry_pub_record {
    _Atomic uint32_t state;
    uint32_t pub_instance;
    uint32_t pub_pid;
    uint64_t gid_a;
    uint64_t gid_b;
} rmw_registry_pub_record_t;

typedef struct rmw_registry_topic {
    _Atomic uint32_t state;
    uint32_t name_hash;
    char name[RMW_WEFT_TOPIC_NAME_MAX];
    uint64_t type_hash;
    uint32_t msg_size;
    uint32_t _pad;
    _Atomic uint32_t subs_version;
    _Atomic uint32_t pubs_version;
    rmw_registry_sub_record_t subs[RMW_WEFT_MAX_SUBS_PER_TOPIC];
    rmw_registry_pub_record_t pubs[RMW_WEFT_MAX_PUBS_PER_TOPIC];
} rmw_registry_topic_t;

typedef struct rmw_registry_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t max_topics;
    uint32_t max_subs;
    uint32_t max_pubs;
    uint32_t creator_pid;
    uint64_t created_unix_ns;
    _Atomic uint64_t epoch;          /* bumped every structural change     */
    _Atomic uint64_t instance_seq;   /* global instance id mint            */
    _Atomic uint32_t activity;       /* registry-wide futex doorbell       */
    _Atomic uint32_t attach_count;   /* live contexts (registry unlink gate)*/
    _Atomic uint32_t waiters;        /* parked wait_set threads            */
    _Atomic uint32_t attach_pids[8]; /* live attacher pids; 0 = free       */
    _Atomic uint32_t claim_lock;     /* COLD-path topic-claim CAS lock     */
    _Atomic uint32_t claim_steals;   /* dead-owner recoveries (audit)      */
    uint8_t reserved[56];
} rmw_registry_header_t;

typedef struct rmw_registry_map {
    uint8_t *base;
    rmw_registry_header_t *hdr;
    rmw_registry_topic_t *topics;   /* base + page 1 */
    size_t mapping_bytes;
    int fd;
    int creator;
    int attach_slot;                /* our pid slot; -1 if table full */
    char name[80];
} rmw_registry_map_t;

/// Open (create if absent) the registry for a domain; sweeps orphans.
/// Returns 0 / -1 (fail-closed; error string set via rmw error state).
int rmw_registry_open(uint32_t domain_id, rmw_registry_map_t *out);

/// Detach; the last context unlinks the registry object.
void rmw_registry_close(rmw_registry_map_t *m);

/// Mint a process-unique instance id (gid material).
uint64_t rmw_registry_mint_instance(rmw_registry_map_t *m);

/// Current registry epoch (ring creation stamps).
uint64_t rmw_registry_epoch(const rmw_registry_map_t *m);

/// FNV-1a 64 over a NUL-terminated string.
uint64_t rmw_weft_hash64(const char *s);

/// Register a subscriber: find-or-create the topic entry, claim a sub
/// record, fill ring name + geometry snapshot, bump subs_version.
/// out_record receives the table slot address (stable for the mapping's
/// lifetime — the table is a fixed array, never reallocated).
int rmw_registry_add_sub(rmw_registry_map_t *m, const char *topic,
                         uint64_t type_hash, uint32_t msg_size,
                         const char *ring_name, uint32_t ring_slots,
                         uint32_t ring_payload, uint32_t sub_instance,
                         uint32_t sub_pid,
                         rmw_registry_sub_record_t **out_record,
                         uint64_t *out_subs_version);

/// Close a subscriber record (state CLOSED, bump subs_version).
int rmw_registry_remove_sub(rmw_registry_map_t *m,
                            rmw_registry_sub_record_t *record);

/// Register a publisher on a topic (find-or-create). *out_topic stable.
int rmw_registry_add_pub(rmw_registry_map_t *m, const char *topic,
                         uint64_t type_hash, uint32_t msg_size,
                         uint32_t pub_instance, uint32_t pub_pid,
                         uint64_t gid_a, uint64_t gid_b,
                         rmw_registry_topic_t **out_topic,
                         rmw_registry_pub_record_t **out_record);

int rmw_registry_remove_pub(rmw_registry_map_t *m,
                            rmw_registry_pub_record_t *record);

/// Snapshot every ACTIVE sub record's ring name for a topic (publisher
/// fan-out refresh). Returns count written into names[] (cap max).
int rmw_registry_list_subs(rmw_registry_map_t *m,
                           rmw_registry_topic_t *topic, uint64_t known_version,
                           const char *names[], uint32_t slots[],
                           uint32_t payloads[], int max);

/// Publisher-side helper: read subs_version without a scan.
uint64_t rmw_registry_subs_version(const rmw_registry_topic_t *topic);

/// Wait-set parking: registry-wide activity futex (bumped by any publish
/// in this domain). Hybrid spin-then-futex; returns 1 if any listed ring
/// advanced, 0 on timeout.
int rmw_registry_activity_wait(rmw_registry_map_t *m, int64_t deadline_ns);

/// Bump the registry activity doorbell (called by every publish, relaxed,
/// futex-wake only when waiters != 0 — the zero-syscall steady state).
void rmw_registry_activity_bump(rmw_registry_map_t *m);

/// Sweep dead-owner records + unlink their rings. Returns count reclaimed.
int rmw_registry_sweep_orphans(rmw_registry_map_t *m);

#ifdef __cplusplus
}
#endif

#endif  // RMW_WEFT__RMW_REGISTRY_H_
