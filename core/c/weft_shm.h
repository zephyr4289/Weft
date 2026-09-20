// weft_shm.h — Zero-Copy IPC Mesh, layer 1: the SHM fabric (RFC-0016).
//
// WHY EXISTS: RFC-0011 gave Weft inter-process ring SESSIONS over named
// POSIX shm / anonymous fork-inherited mappings / raw fds — but the
// ANONYMOUS road had no descriptor-exchange protocol (a bare fd number is
// meaningless in another process), nothing SEALED the object against
// resize attacks, and consumers mapped read-only only by their own
// discipline. This module is the next layer: the anonymous-descriptor
// exchange fabric the Weft Mesh runs on.
//
//   1. SEALED ANONYMOUS SESSIONS. memfd_create(MFD_ALLOW_SEALING) +
//      ftruncate + a WFSH v1 session header (byte-identical to RFC-0011 /
//      shm_ring.{h,c} — same object, same validation), then
//      F_ADD_SEALS(F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL). After the
//      seal, NO holder of any fd to the object — including a hostile
//      consumer that scrapes one via /proc — can ftruncate it (EPERM):
//      the mapping geometry every attacher validated is frozen by the
//      kernel for the object's lifetime.
//      WHY NOT F_SEAL_WRITE: the producer must keep filling the ring
//      through its own writable mapping; sealing write with a live
//      writable mapping fails EPERM at seal time (memfd_create(2)) — and
//      consumer-side write protection has a sharper tool:
//
//   2. THE KERNEL-ENFORCED READ-ONLY VIEW. The producer derives a second,
//      O_RDONLY descriptor to the same object via open("/proc/self/fd/N").
//      An fd's access rights are fixed at open() time, so SCM_RIGHTS
//      recipients of the RO view CANNOT mmap PROT_WRITE — EACCES from the
//      kernel, MMU-level, no library discipline involved. This is the
//      "strict hardware-level memory protection" of the mesh: writable
//      access exists ONLY as the producer's private fd (or an explicit
//      WRITE-grant handed out by the capability-token policy in weft_ipc).
//
//   3. THE DESCRIPTOR EXCHANGE. SCM_RIGHTS ancillary messages over
//      AF_UNIX sockets: weft_shm_send_fd()/recv_fd() move exactly one fd
//      plus a bounded data record, validating the cmsg shape (level, type,
//      exactly one fd, no truncation) — a malformed message is refused,
//      never guessed at (Law 4).
//
//   4. THE HANDSHAKE. A fixed-frame HELLO/GRANT protocol over any
//      connected AF_UNIX socket (socketpair for fork meshes, a named
//      /tmp/weft-ipc/<name>.sock listener for independent processes).
//      The client presents a capability token + requested permissions;
//      the server verifies via a caller-supplied verifier (the token
//      policy lives in weft_ipc — mechanism here, policy there; Law 3)
//      and grants the RO view fd with a nonce-bound GRANT carrying the
//      session geometry + producer epoch. Every field is validated on
//      BOTH sides; refusals carry explicit status codes.
//
// DATA PATH: none of this is in the data path. Publish/claim run on the
// mapped ring exactly as in RFC-0011 — zero syscalls, zero allocations
// (Law 2), bounded claims (Law 1). The fabric is attach-time machinery.
//
// Layer discipline: driver layer. weft.c/weft.h byte-frozen; this module
// BUILDS ON shm_ring.{h,c} (reuses weft_shm_map_t + attach-by-fd) and
// fanout.{h,c} (the ring itself) without modifying either. POSIX-only
// (memfd/seals/SCM_RIGHTS are Linux roads; Windows uses RFC-0011 named
// mappings — compile-gated, declared).
//
// Honesty boundary: executable-verified on x86_64 Linux 5.10 (M-series +
// ipc-torture under ASAN; TSAN legs on the in-process shared-memory
// access paths). /proc/self/fd reopen + seal + EACCES semantics probed
// green before this header was written (litmus/evidence/ipc-mesh/).

#ifndef WEFT_SHM_FABRIC_H
#define WEFT_SHM_FABRIC_H

#include <stddef.h>
#include <stdint.h>

#include "shm_ring.h"   // weft_shm_map_t, WFSH header contract (RFC-0011)

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Wire protocol constants (little-endian on the wire)
// ---------------------------------------------------------------------------

/// "WFHI" LE — client HELLO magic.
#define WEFT_SHM_HELLO_MAGIC 0x49484657u
/// "WFHG" LE — server GRANT magic.
#define WEFT_SHM_GRANT_MAGIC 0x47484657u
/// Handshake protocol version.
#define WEFT_SHM_HANDSHAKE_VERSION 1u
/// HELLO frame size, bytes.
#define WEFT_SHM_HELLO_BYTES 96u
/// GRANT frame size, bytes.
#define WEFT_SHM_GRANT_BYTES 64u

/// HELLO commands.
#define WEFT_SHM_CMD_SUBSCRIBE 1u        ///< attach as a reader (RO view fd)
#define WEFT_SHM_CMD_WRITER_HANDOFF 2u   ///< successor-writer negotiation (ADMIN token)
#define WEFT_SHM_CMD_PING 3u             ///< liveness/epoch probe; no fd granted

/// GRANT status codes — every refusal is explicit (Law 4).
#define WEFT_SHM_GRANT_OK 0u
#define WEFT_SHM_GRANT_DENIED_TOKEN 1u       ///< HMAC verification failed
#define WEFT_SHM_GRANT_DENIED_EXPIRED 2u     ///< token expiry passed
#define WEFT_SHM_GRANT_DENIED_PERMS 3u       ///< token lacks requested perms
#define WEFT_SHM_GRANT_DENIED_PROTOCOL 4u    ///< bad magic/version/nonce/session
#define WEFT_SHM_GRANT_DENIED_ANON 5u        ///< perms requested need a token; none given
#define WEFT_SHM_GRANT_DENIED_SESSION 6u     ///< hello session id does not match ours

/// Permission bits (mirrored in weft_ipc.h — the token policy plane).
#define WEFT_SHM_PERM_READ 0x1u
#define WEFT_SHM_PERM_WRITE 0x2u
#define WEFT_SHM_PERM_CLAIM 0x4u
#define WEFT_SHM_PERM_ADMIN 0x8u

/// Default anonymous grant: read + claim (the recorder posture).
#define WEFT_SHM_PERM_DEFAULT (WEFT_SHM_PERM_READ | WEFT_SHM_PERM_CLAIM)

/// Capability token size on the wire (claims 32B + HMAC tag 32B).
#define WEFT_SHM_TOKEN_BYTES 64u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// A sealed memfd session: the WFSH map plus the fabric's descriptor
/// discipline. Field discipline:
///   - map: the session mapping (header + ring); map.fd owns ONE fd.
///   - fd_ro: creator — the derived read-only view used for SCM_RIGHTS
///     export (an ALIAS is NOT stored; for importers fd_ro == map.fd).
///     Destroy closes it exactly once (alias-aware).
///   - writable: 1 when map.fd grants write access (creator, or an
///     explicit WRITE grant). 0 for imported read-only views.
///   - seals: F_GET_SEALS at create time — diagnostics only (AXIOM T).
typedef struct weft_shm_memfd {
    weft_shm_map_t map;
    int fd_ro;
    int writable;
    uint32_t seals;
} weft_shm_memfd_t;

/// What the server told us in a GRANT (validated against the mapped
/// header by weft_shm_handshake_connect_fd before it returns 0).
typedef struct {
    uint64_t session_id;
    uint64_t nonce;          ///< echo of OUR hello nonce (replay binding)
    uint64_t latest_seq;     ///< ring latestSeq at grant time (ADVISORY — the
                             ///< ring's own stamps are the only authority)
    uint32_t status;         ///< WEFT_SHM_GRANT_*
    uint32_t epoch;          ///< producer incarnation (successor detection)
    uint32_t perms_granted;
    uint32_t payload_bytes;
    uint32_t slot_count;
    uint64_t ring_bytes;
} weft_shm_grant_info_t;

/// Token verifier callback — the policy seam (Law 3). ctx is opaque here;
/// weft_ipc supplies a key-holding implementation.
/// Returns 0 to grant (out_perms = the token's perm bits, masked by
/// `required_perms`), nonzero to deny (mapped to a GRANT status).
typedef int (*weft_shm_token_verifier_t)(void* ctx, const uint8_t* token,
                                         uint64_t session_id,
                                         uint32_t required_perms,
                                         uint32_t* out_perms);

// ---------------------------------------------------------------------------
// Sealed anonymous session lifecycle
// ---------------------------------------------------------------------------

/// Create a sealed memfd session. memfd_create + ftruncate + WFSH v1
/// header (byte-identical to RFC-0011) + zero ring + F_ADD_SEALS
/// (GROW|SHRINK|SEAL), then a validated RW mapping for the producer.
/// Fails (-1) on bad geometry, memfd/seal/mmap failure. May allocate /
/// syscall — cold path (Law 2 applies to the data path, not attach).
int weft_shm_memfd_create(size_t payload_bytes, unsigned slot_count,
                          weft_shm_memfd_t* out);

/// Derive the kernel-enforced read-only view (open /proc/self/fd/N,
/// O_RDONLY|O_CLOEXEC). Recipients of THIS fd cannot mmap PROT_WRITE
/// (EACCES — access rights are fixed at open time). Fails (-1) if the
/// reopen is unavailable; NEVER silently falls back to the RW fd (Law 4).
int weft_shm_memfd_ro_view(weft_shm_memfd_t* m);

/// The RO-view fd (for SCM_RIGHTS export); -1 if none was derived.
int weft_shm_memfd_ro_fd(const weft_shm_memfd_t* m);

/// Release the session: unmap + close map.fd and the RO view (alias-
/// aware). Does NOT unlink anything — anonymous objects die with their
/// last fd, which is exactly the crash posture a mesh wants. Idempotent.
void weft_shm_memfd_destroy(weft_shm_memfd_t* m);

// ---------------------------------------------------------------------------
// SCM_RIGHTS primitives — exactly one fd + one bounded data record
// ---------------------------------------------------------------------------

/// Send `fd` plus `len` (<= 480) data bytes on a connected AF_UNIX socket.
/// One sendmsg, one cmsg. Returns 0, or -1 (errno set) on syscall failure,
/// -2 on bad arguments.
int weft_shm_send_fd(int sock, int fd, const void* data, size_t len);

/// Receive one fd + data record. Validates the cmsg shape: SOL_SOCKET +
/// SCM_RIGHTS + exactly one descriptor + no truncation. `*fd_out` owns
/// the received descriptor (CLOEXEC forced via MSG_CMSG_CLOEXEC). A
/// zero-length data message with a cmsg is a MESSAGE, not EOF (an fd
/// alone is a complete handoff; EOF is ruled only when no cmsg arrived).
/// Returns 0 (fd + data), 1 (data only — a legitimate fd-less message,
/// e.g. a denial; *fd_out untouched), -1 (errno) syscall failure,
/// -2 bad args, -3 malformed cmsg / truncation, -4 peer closed (EOF).
int weft_shm_recv_fd(int sock, int* fd_out, void* buf, size_t buflen,
                     size_t* got);

// ---------------------------------------------------------------------------
// Handshake — over any connected socket
// ---------------------------------------------------------------------------

/// Serve ONE handshake on `conn` (a connected socket) for session `m`.
/// Reads the HELLO, validates it, verifies the token (if perms beyond
/// the anonymous default are requested) via `verify` (NULL = anonymous
/// only), and answers with a GRANT carrying `m`'s RO-view fd — or `m`'s
/// RW fd when a WRITE perm is granted — plus `m`'s geometry + epoch +
/// the ring's current latestSeq (advisory). The GRANT's geometry words
/// come from the session object itself, so the client's grant-vs-header
/// cross-check is a real consistency gate, not a false mismatch. PING
/// answers without an fd. WRITER_HANDOFF is an ADMIN-token-gated read
/// grant of the RO view (the successor seeds its numbering by reading
/// the old ring — graceful restart; the crashy road reads the registry
/// mirror instead).
/// Returns 0 on a completed exchange (whatever the verdict — the status
/// code went to the peer), -1 on I/O or protocol error. Serving requires
/// a derived RO view (weft_shm_memfd_ro_view) — an undrived session
/// answers SUBSCRIBE with DENIED_PROTOCOL rather than leaking the RW fd.
int weft_shm_handshake_serve(int conn, const weft_shm_memfd_t* m,
                             uint64_t session_id, uint32_t epoch,
                             weft_shm_token_verifier_t verify,
                             void* verify_ctx);

/// Client side on a connected socket. Sends the HELLO (token may be NULL
/// for anonymous READ|CLAIM), receives + validates the GRANT (magic,
/// version, nonce echo, session id), and on OK maps + validates the
/// received fd (read-only unless a WRITE perm was granted — which
/// requires a valid token). `expect_session_id` 0 = accept any.
/// Returns 0, -1 syscall/protocol error (errno where applicable),
/// -2 peer DENIED (grant->status carries the reason), -3 GRANT/hdr
/// geometry mismatch.
int weft_shm_handshake_connect_fd(int conn, uint32_t requested_perms,
                                  uint64_t expect_session_id,
                                  const uint8_t* token,
                                  weft_shm_memfd_t* out,
                                  weft_shm_grant_info_t* grant);

/// PING a live server: status + epoch + session id, no fd. Returns 0,
/// -1 I/O error, -2 protocol violation.
int weft_shm_handshake_ping_fd(int conn, uint64_t expect_session_id,
                               weft_shm_grant_info_t* grant);

// ---------------------------------------------------------------------------
// Named-socket conveniences (independent processes; fork meshes use
// socketpair and call the _fd forms directly)
// ---------------------------------------------------------------------------

/// Derive the canonical socket path for a session name:
/// /tmp/weft-ipc/<name>.sock (name grammar: RFC-0011's). Returns 0/-1.
int weft_shm_sock_path(const char* name, char* buf, size_t buflen);

/// Create the listener (mkdir /tmp/weft-ipc best-effort, bind, listen).
/// The socket is a CLOEXEC fd the caller owns. Returns 0/-1.
int weft_shm_listen(const char* name, int* out_sock);

/// Connect to a listener by session name. Returns the connected fd or -1.
int weft_shm_connect(const char* name);

/// Accept one connection (CLOEXEC). Returns the conn fd or -1.
int weft_shm_accept(int listen_sock);

/// Client handshake over the named socket (connect + _fd form; the socket
/// is closed before returning).
int weft_shm_handshake_connect(const char* name, uint32_t requested_perms,
                               uint64_t expect_session_id,
                               const uint8_t* token,
                               weft_shm_memfd_t* out,
                               weft_shm_grant_info_t* grant);

// ---------------------------------------------------------------------------
// Doorbell-free bounded park (the wake-up-latency primitive)
// ---------------------------------------------------------------------------

/// Atomic Acquire load of the ring's latestSeq (documented offset 0 of the
/// RFC-0004 layout — the same contract weft_fanout_ring() exposes).
uint64_t weft_shm_latest_seq(const weft_shm_map_t* m);

/// Bounded adaptive park: poll latestSeq up to `spin_cap` iterations with
/// a CPU pause between loads (x86 `rep; nop` where available). ZERO
/// syscalls — the house zero-syscall data-path gate is untouched. Returns
/// 0 and updates *last_seq when a newer frame is observed; 1 when the
/// spin budget ran out (bounded — Law 1: the caller re-parks at its own
/// cadence; the ring's bounded claim remains the only doorbell, per
/// Volume II §3.6). -1 on bad arguments.
int weft_shm_park(const weft_shm_map_t* m, uint64_t* last_seq,
                  uint32_t spin_cap);

#ifdef __cplusplus
}
#endif

#endif // WEFT_SHM_FABRIC_H
