// weft_ipc.h — Zero-Copy IPC Mesh, layer 2: the mesh itself (RFC-0016).
//
// WHY EXISTS: the SHM fabric (weft_shm) moves sealed descriptors between
// two processes that already know each other. A MESH needs more: processes
// that have never met must FIND each other (peer discovery), the found
// publisher must be verifiably ALIVE (heartbeats), processes that vanish
// must be reaped without a central coordinator (crash resilience), and a
// restarted producer must resume the stream cleanly (successor handoff).
// This module is that control plane — built as ONE fixed-size shared
// registry file plus a pure-C HMAC capability layer, no daemons, no
// locks, no kernel mediation on any steady-state path.
//
// THE REGISTRY (/dev/shm/weft_registry_v1): a fixed 9984-byte object —
// 64-byte header + 62 slots x 160 bytes. Every slot is a lock-free
// 4-state lifecycle keyed by ONE atomic word (state | generation):
//
//        CAS(FREE, g -> RESERVED, g)      owner fills immutable fields
//   RESERVED --release store--> ACTIVE    fields now visible to scanners
//   ACTIVE   --release store--> DEAD      clean detach (owner's goodbye)
//   DEAD/RESERVED-stale --CAS--> FREE(g+1) reaped by ANY process's heal
//   ACTIVE-stale+dead-pid --CAS--> DEAD   crashed producer, detected
//
// The generation counter makes reaping ABA-safe: a scanner that re-loads
// a slot mid-lifecycle sees (state, g) pairs that cannot alias an older
// incarnation of the same state. Registration fields (session_id, name,
// geometry, epoch, transport, producer pid) are written ONCE between
// RESERVE and ACTIVATE, so the release-store publication makes every
// ACTIVE snapshot CONSISTENT for the immutable set (not merely advisory);
// heartbeat and consumer counters remain advisory (AXIOM T).
//
// CAPABILITY TOKENS: 64-byte objects = 32-byte canonical claims
// {session_id, perms, epoch, expiry, key_id, nonce} || 32-byte
// HMAC-SHA256(K_session, claims), K from getrandom at session create.
// The honest split: the DATA plane is protected by the KERNEL (memfd
// anonymity + seals + the RO-view's open()-time access rights — an fd IS
// a capability); the CONTROL plane is protected by the TOKEN (WRITE
// grants, successor-writer handoff, privileged ops are denied without a
// verified token). The registry is cooperative discovery infrastructure,
// NOT a security boundary — /dev/shm is writable by the mesh's own user
// population; we say so rather than pretend otherwise.
//
// HEALING: weft_ipc_heal() is a bounded scan any process may run:
// ACTIVE entries with a stale heartbeat AND a dead pid are crashed out;
// DEAD entries are reaped to FREE with a generation bump; mapped rings
// get the Axis-3 self-stabilizing treatment (weft_ring_health_check /
// weft_ring_recover, already in fanout.{h,c}). A consumer crashing
// mid-claim needs NO recovery at the ring (RFC-0004 readers are
// stateless observers — the writer never waits); what it leaves behind
// is an advisory consumer count and its copy buffer, both private.
//
// Law 1: every scan is bounded (fixed slot count); no unbounded spins.
// Law 2: heartbeat/discover/heal allocate nothing (fixed-size snapshot
// arrays owned by the caller). Law 3: pure driver layer; weft.c/weft.h
// and every other kernel/driver file byte-frozen. Law 4: every refusal
// carries an explicit code; every advisory field is labeled advisory.
//
// Honesty boundary: executable on POSIX (the registry file road needs
// a writable /dev/shm; token crypto is the in-tree pure-C HMAC-SHA256 —
// zero new dependencies). PID liveness via kill(pid, 0) is heuristic
// (PID reuse/containers) — combined with heartbeat staleness, never
// trusted alone; documented where used.

#ifndef WEFT_IPC_MESH_H
#define WEFT_IPC_MESH_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"     // weft_fanout_reader_t (resync), Axis-3 health API
#include "shm_ring.h"   // weft_shm_map_t

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Registry constants
// ---------------------------------------------------------------------------

/// Canonical registry object path.
#define WEFT_IPC_REGISTRY_PATH "/dev/shm/weft_registry_v1"
/// "WFRE" LE.
#define WEFT_IPC_REGISTRY_MAGIC 0x45524657u
#define WEFT_IPC_REGISTRY_VERSION 1u
#define WEFT_IPC_REGISTRY_HEADER_BYTES 64u
#define WEFT_IPC_REGISTRY_ENTRY_COUNT 62u
#define WEFT_IPC_REGISTRY_ENTRY_BYTES 160u
/// 64 + 62*160 = 9984 bytes, validated EXACTLY on open.
#define WEFT_IPC_REGISTRY_BYTES 9984u

/// Slot lifecycle states (the low u32 of the CAS word).
#define WEFT_IPC_SLOT_FREE 0u
#define WEFT_IPC_SLOT_ACTIVE 1u
#define WEFT_IPC_SLOT_DEAD 2u
#define WEFT_IPC_SLOT_RESERVED 3u

/// Session transports.
#define WEFT_IPC_TRANSPORT_NAMED 1u   ///< RFC-0011 named shm object
#define WEFT_IPC_TRANSPORT_MEMFD 2u   ///< weft_shm sealed memfd + handshake

/// Permission bits (mirror weft_shm.h — one definition each side, equal
/// values; static asserts pin the equality in weft_ipc.c).
#define WEFT_IPC_PERM_READ 0x1u
#define WEFT_IPC_PERM_WRITE 0x2u
#define WEFT_IPC_PERM_CLAIM 0x4u
#define WEFT_IPC_PERM_ADMIN 0x8u

#define WEFT_IPC_NAME_MAX 40u
#define WEFT_IPC_TOKEN_BYTES 64u
#define WEFT_IPC_KEY_BYTES 32u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// An open registry view. RW mappings may mutate slots (register,
/// heartbeat, heal); read-only mappings are monitor posture (discover
/// only — CAS ops would fault, and we say so instead of trying).
typedef struct weft_ipc_registry {
    uint8_t* base;
    size_t bytes;
    int fd;
    int read_only;
} weft_ipc_registry_t;

/// A registered producer session (the registration handle). `slot` +
/// `generation` identify the entry incarnation; unregister is valid only
/// while they still match (a healed-out registration fails loudly, not
/// silently — Law 4).
typedef struct weft_ipc_session {
    weft_ipc_registry_t* reg;
    uint32_t slot;
    uint32_t generation;
    uint64_t session_id;
    uint32_t epoch;
    int active;
} weft_ipc_session_t;

/// Discovery snapshot — the immutable field set is consistent by the
/// RESERVE->ACTIVE release-store protocol; counters are advisory.
typedef struct {
    uint32_t slot;
    uint64_t session_id;
    uint32_t epoch;
    uint32_t transport;
    uint32_t producer_pid;
    uint32_t n_consumers;      ///< advisory (AXIOM T)
    uint64_t heartbeat_seq;    ///< advisory
    uint64_t heartbeat_ns;     ///< advisory
    uint32_t payload_bytes;
    uint32_t slot_count;
    uint64_t latest_seq_pub;   ///< advisory mirror of the ring's latestSeq
    char name[WEFT_IPC_NAME_MAX + 1];
    uint32_t token_key_id;     ///< 0 = no token published
    int stale;                 ///< heartbeat older than the caller's window
    int producer_alive;        ///< kill(pid,0) verdict (heuristic, advisory)
} weft_ipc_discovered_t;

/// Heal pass report — every counter is a measured fact of THIS pass.
typedef struct {
    uint32_t crashed_detected;  ///< ACTIVE, stale heartbeat + dead pid
    uint32_t reaped_dead;       ///< DEAD entries freed (generation bumped)
    uint32_t reaped_reserved;   ///< abandoned RESERVATIONs freed
    uint32_t active_now;        ///< ACTIVE count after the pass
    uint32_t scanned;           ///< slots examined
} weft_ipc_heal_report_t;

/// Token claims — canonical 32-byte little-endian serialization.
typedef struct {
    uint64_t session_id;
    uint32_t perms;
    uint32_t epoch;
    uint64_t expiry_unix;   ///< seconds since epoch; 0 = no expiry
    uint32_t key_id;
    uint32_t nonce;         ///< issue-time randomness (replay binding aid)
} weft_ipc_claims_t;

/// Token verification context for the weft_shm verifier seam.
typedef struct {
    uint8_t key[WEFT_IPC_KEY_BYTES];
    uint64_t session_id;
    uint32_t epoch;
} weft_ipc_token_ctx_t;

/// Explicit error codes (Law 4 — no silent failures).
enum {
    WEFT_IPC_ERR_SYS = -1,        ///< errno carries the syscall failure
    WEFT_IPC_ERR_DUPLICATE = -2,  ///< live producer already owns the name
    WEFT_IPC_ERR_FULL = -3,       ///< registry has no free slot
    WEFT_IPC_ERR_INVALID = -4,    ///< bad arguments / registry validation
    WEFT_IPC_ERR_READONLY = -5,   ///< mutation attempted on a monitor view
};

// ---------------------------------------------------------------------------
// Registry lifecycle
// ---------------------------------------------------------------------------

/// Open (and create on first use) the registry. `create_if_absent` races
/// safely across processes (O_CREAT|O_EXCL + validated retry). `read_only`
/// maps the monitor posture. Returns 0 or a WEFT_IPC_ERR_* code.
int weft_ipc_registry_open(weft_ipc_registry_t* reg, int create_if_absent,
                           int read_only);

/// Close the view (unmap + close fd). Never unlinks — the registry is
/// shared mesh infrastructure that outlives every process (documented).
void weft_ipc_registry_close(weft_ipc_registry_t* reg);

// ---------------------------------------------------------------------------
// Producer session lifecycle
// ---------------------------------------------------------------------------

/// Register a producer session. Scans bounded, CAS-claims a slot, fills
/// the immutable fields, activates. Successor semantics: a same-name
/// entry left ACTIVE by a crashed-and-stale producer is taken over with
/// epoch+1 (the crashed incumbent must be dead per kill(pid,0) AND stale
/// per heartbeat age — both, never one). A LIVE incumbent refuses with
/// WEFT_IPC_ERR_DUPLICATE. `latest_seq_now` seeds the advisory mirror.
/// session_id is generated internally (getrandom). Returns 0 or an error.
int weft_ipc_register(weft_ipc_registry_t* reg, const char* name,
                      uint32_t transport, size_t payload_bytes,
                      unsigned slot_count, uint64_t latest_seq_now,
                      weft_ipc_session_t* out);

/// The registration's session_id (0 if inactive).
uint64_t weft_ipc_session_id(const weft_ipc_session_t* s);

/// Heartbeat: bump the monotonic beat counter, stamp wall time, refresh
/// the latestSeq mirror. Relaxed atomics — advisory liveness, never a
/// correctness reference. Zero allocation, zero syscalls. Returns 0, or
/// WEFT_IPC_ERR_INVALID if the registration was healed out.
int weft_ipc_heartbeat(weft_ipc_session_t* s, uint64_t latest_seq);

/// Clean detach: ACTIVE -> DEAD (release store). The entry stays readable
/// until some heal pass reaps it. Idempotent; returns 0 or an error code
/// if the registration no longer matches the slot generation (healed out
/// — loud, not silent).
int weft_ipc_unregister(weft_ipc_session_t* s);

/// Publish the current token's key id + HMAC tag into the entry (the
/// advisory cross-check plane: consumers compare a offered token's tag
/// against this without holding the key). Returns 0 or an error code.
int weft_ipc_session_publish_token(weft_ipc_session_t* s, uint32_t key_id,
                                   const uint8_t* tag);

// ---------------------------------------------------------------------------
// Consumers
// ---------------------------------------------------------------------------

/// Attach/leave a consumer to a session (advisory n_consumers counter;
/// exact under clean lifecycles, inflated by crashes — documented, and
/// the torture pins BOTH behaviors). Find-by-session_id is a bounded
/// scan. Returns 0 or WEFT_IPC_ERR_INVALID (no such ACTIVE session).
int weft_ipc_consumer_attach(weft_ipc_registry_t* reg, uint64_t session_id);
int weft_ipc_consumer_leave(weft_ipc_registry_t* reg, uint64_t session_id);

// ---------------------------------------------------------------------------
// Discovery / healing
// ---------------------------------------------------------------------------

/// Snapshot ACTIVE sessions (optionally filtered by exact name). Bounded
/// by max; returns the number written, or an error code. `stale_ms`
/// marks entries whose heartbeat wall-stamp is older (advisory flag).
int weft_ipc_discover(weft_ipc_registry_t* reg, uint64_t stale_ms,
                      const char* name_filter, weft_ipc_discovered_t* out,
                      unsigned max);

/// One bounded healing pass (any process may run it; safe to run
/// concurrently — every transition is a CAS or a release store):
///   - ACTIVE + stale heartbeat + dead pid  -> crashed out to DEAD
///   - DEAD                                 -> reaped to FREE (gen + 1)
///   - RESERVED + stale                     -> abandoned, reaped to FREE
/// Returns 0 (report filled) or an error code. Zero allocation.
int weft_ipc_heal(weft_ipc_registry_t* reg, uint64_t stale_ms,
                  weft_ipc_heal_report_t* report);

/// Axis-3 integration for a ring THIS process has mapped: health-check,
/// and recover if corrupt (weft_ring_recover semantics). Returns
/// weft_ring_health_t before recovery; *after_recovery holds the
/// post-state when recovery ran. Declares a recovered ring loudly.
weft_ring_health_t weft_ipc_heal_ring(weft_shm_map_t* m,
                                      weft_ring_health_t* after_recovery);

/// Mesh re-attach baseline: reset a reader's last_seq to `baseline` so
/// telescoping restarts from a known point. Passing the reader's OWN
/// last observed seq when moving to a successor incarnation makes drop
/// accounting span the handoff EXACTLY (the outage gap telescopes).
/// Field access on the public reader struct — driver-layer discipline.
void weft_ipc_reader_resync(weft_fanout_reader_t* r, uint64_t baseline);

// ---------------------------------------------------------------------------
// Capability tokens (HMAC-SHA256 over canonical claims)
// ---------------------------------------------------------------------------

/// Fill `out` with `n` random bytes (getentropy; /dev/urandom fallback).
/// Returns 0/-1.
int weft_ipc_random_bytes(void* out, size_t n);

/// Issue a token: canonical claims + HMAC-SHA256(key, claims).
/// Returns 0/-1 (bad args).
int weft_ipc_token_issue(const weft_ipc_claims_t* claims,
                         const uint8_t* key, uint8_t* out /*[64]*/);

/// Verify a token against a key. Returns 0 (claims out), or
/// -2 bad HMAC tag, -3 wrong session, -4 expired, -5 epoch mismatch,
/// -6 malformed (all-zero claims). The epoch check binds tokens to a
/// producer incarnation; pass epoch_now = the entry's current epoch.
int weft_ipc_token_verify(const uint8_t* token, const uint8_t* key,
                          uint64_t session_id, uint32_t epoch_now,
                          weft_ipc_claims_t* out);

/// The weft_shm verifier-seam adapter: ctx = weft_ipc_token_ctx_t.
/// Grants the token's perms masked by required_perms; maps verify
/// failures onto WEFT_SHM_GRANT_DENIED_* statuses (returned nonzero).
int weft_ipc_token_verify_adapter(void* ctx, const uint8_t* token,
                                  uint64_t session_id, uint32_t required_perms,
                                  uint32_t* out_perms);

/// Canonical claims <-> bytes (little-endian, fixed layout; the exact
/// bytes HMAC covers). Zero-padding forbidden: decode validates.
int weft_ipc_claims_encode(const weft_ipc_claims_t* c, uint8_t* out /*[32]*/);
int weft_ipc_claims_decode(const uint8_t* in /*[32]*/, weft_ipc_claims_t* c);

#ifdef __cplusplus
}
#endif

#endif // WEFT_IPC_MESH_H
