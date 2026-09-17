// shm_ring.h — Inter-process shared-memory rings (RFC-0004 layout over
// POSIX shm / anonymous mmap / Windows named mappings). Driver layer.
//
// WHY EXISTS: RFC 0004's fan-out ring is already CROSS-THREAD and
// CROSS-LANGUAGE (byte-compatible layout, Series 4), and the flight
// recorder attaches to POSIX shm by name — but that plumbing lives inside
// the tool (tools/weft-fanout-rec), with no session protocol: an attacher
// validates geometry only by fstat size, nothing records what the object
// IS, and the C/Rust kernels expose no IPC surface at all. This module
// makes the ring a first-class INTER-PROCESS citizen: one shm object,
// N processes, zero kernel context switches in the data path (the claim
// protocol is pure shared-memory atomics — no futex, no pipe, no socket;
// the strace evidence in the Series-7 logs shows a steady state of zero
// syscalls between marker writes).
//
// SESSION HEADER (64 bytes, little-endian) — the cross-process contract:
//   offset 0   magic "WFSH" (u32)      0x48534657
//   offset 4   version (u16)           1
//   offset 6   header_size (u16)       64
//   offset 8   flags (u32)             0 (reserved)
//   offset 12  payload_bytes (u32)     ring slot capacity (multiple of 4)
//   offset 16  slot_count (u32)        ring depth M
//   offset 20  ring_bytes (u64)        16 + 8M + M*payload_bytes (RFC 0004)
//   offset 28  creator_pid (u32)       diagnostics only (AXIOM T)
//   offset 32  created_unix_ns (u64)   diagnostics only (AXIOM T)
//   offset 40  reserved (24 bytes)     zero; unknown bits are a version
//                                      violation on attach (reject)
//   offset 64  the RFC-0004 ring (byte-compatible with core/ts/fanout.ts,
//              core/c/fanout.c, core/rust/src/fanout.rs)
//
// ATTACH SEMANTICS: the header is written ONCE by the creator, before any
// frame publishes; attach validates magic/version/header_size/geometry and
// the mapping size EXACTLY (header + ring_bytes — a mismatched object is
// refused, not guessed at). The header is advisory metadata for attach;
// the ring's own stamps remain the only correctness authority (AXIOM T).
//
// MEMORY MODEL: C11 atomics over MAP_SHARED (POSIX) / a named file mapping
// (Windows) are cross-process-safe on every platform this tree targets —
// the same physical pages are mapped cache-coherently in every process;
// the fanout.h ordering regime (fenced acq/rel) applies unchanged across
// address spaces. This is the documented foundation of every
// shared-memory IPC library; Weft adds no new ordering rules.
//
// LIFETIME: the shm OBJECT outlives any process (POSIX: until shm_unlink;
// Windows: until the last handle closes) — a crashed producer's ring
// remains attachable, which is exactly the crash-tolerant posture the
// recorder wants. The CREATOR unlinks on destroy; attachers never unlink
// (a reader destroying the object under a live writer would be a
// use-after-unlink, documented, not defended — single-creator contract).
//
// LAW 2: create/attach may allocate (page-table setup, one syscall each);
// the data path (publish/claim through the fanout API) allocates nothing
// and makes no syscalls.
//
// Layer discipline: driver layer. weft.c/weft.h untouched. POSIX side:
// shm_open/mmap/munmap/ftruncate (RTLD now required on Linux link lines —
// see the Makefile); Windows side: CreateFileMappingA/OpenFileMappingA/
// MapViewOfFile — compile-gated _WIN32, compile-verified by the windows CI
// leg, NOT executable-tested in the x86_64-POSIX sandbox — declared.
//
// Honesty boundary: named and anonymous POSIX paths are
// executable-verified here (S-series + fork torture + strace evidence).
// The Windows path is compile-gated. Fork-based torture is POSIX-only
// (Windows has no fork; the named path is the Windows story).

#ifndef WEFT_SHM_RING_H
#define WEFT_SHM_RING_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"

/// Session header size in bytes (the ring starts at this offset).
#define WEFT_SHM_HEADER_BYTES 64u

/// "WFSH" little-endian.
#define WEFT_SHM_MAGIC 0x48534657u

/// Session protocol version (bump on header layout change — attachers
/// reject mismatches rather than guess).
#define WEFT_SHM_VERSION 1u

/// A mapped shared-memory session: header + ring, plus the resources needed
/// to unmap it. Borrowed by fanout broadcasters/readers via the ring
/// pointer — keep the map alive as long as any fanout object uses it.
typedef struct weft_shm_map {
    uint8_t* base;          ///< Mapping start (the session header)
    uint8_t* ring;          ///< base + WEFT_SHM_HEADER_BYTES (RFC-0004 layout)
    size_t mapping_bytes;   ///< WEFT_SHM_HEADER_BYTES + ring_bytes
    int fd;                 ///< POSIX fd; -1 for anonymous mappings
    int creator;            ///< 1 = created here (destroy unlinks the name)
    char name[96];          ///< Object name (no leading '/'; "" = anonymous)
} weft_shm_map_t;

// ---------------------------------------------------------------------------
// Session creation / attach
// ---------------------------------------------------------------------------

/// Create a NAMED session (POSIX shm object; Windows named file mapping).
/// `name` must be 1..80 chars of [A-Za-z0-9._-], not starting with '-'.
/// Fails (-1) on an invalid name, bad ring geometry, an EXISTING object of
/// the same name (O_EXCL — replace-stale is the CALLER's explicit choice,
/// see weft_shm_unlink), or resource exhaustion. On success the header is
/// fully written and the ring ctrl area is zero-initialized (latestSeq=0,
/// all slots invalidated) — the same invariants a fresh malloc'd ring has.
int weft_shm_create_named(const char* name, size_t payload_bytes,
                          unsigned slot_count, weft_shm_map_t* out);

/// Attach an existing NAMED session. Validates magic, version,
/// header_size, reserved-zero, and the EXACT mapping size. `read_only`
/// maps PROT_READ (a recorder that never writes the ring). Fails (-1) if
/// the object is absent or not a Weft session — including size mismatch.
int weft_shm_attach_named(const char* name, weft_shm_map_t* out, int read_only);

/// Create an ANONYMOUS session (MAP_ANONYMOUS|MAP_SHARED — no name, no
/// fd). For fork()-based multiprocessing: the mapping is INHERITED by
/// children, which see the same pages. Fails (-1) on bad geometry or mmap
/// failure. (Windows: unsupported — no fork to inherit it; -1, declared.)
int weft_shm_create_anon(size_t payload_bytes, unsigned slot_count,
                         weft_shm_map_t* out);

/// Attach a session by fd (memfd_create / shm fd passing). The caller owns
/// the fd before the call; the map owns it after (destroy closes it).
int weft_shm_attach_fd(int fd, weft_shm_map_t* out, int read_only);

/// Explicitly remove a stale named object (the replace-stale road the
/// recorder's replay mode takes). Ignores absence; returns 0/-1.
int weft_shm_unlink(const char* name);

/// Unmap and release. Creator ALSO unlinks the name (a crash leaves the
/// object for the next attach — declared, not defended). Idempotent.
void weft_shm_destroy(weft_shm_map_t* m);

// ---------------------------------------------------------------------------
// Geometry accessors (from the session header)
// ---------------------------------------------------------------------------

size_t weft_shm_payload_bytes(const weft_shm_map_t* m);
unsigned weft_shm_slot_count(const weft_shm_map_t* m);
size_t weft_shm_ring_bytes(const weft_shm_map_t* m);
const void* weft_shm_ring(const weft_shm_map_t* m);

// ---------------------------------------------------------------------------
// Fan-out bindings (thin conveniences over the Series-4 attach APIs)
// ---------------------------------------------------------------------------

/// Create a named session AND bind a broadcaster to its ring (creator
/// starts a fresh stream). Destroy the map with the broadcaster.
int weft_fanout_shm_create(const char* name, size_t payload_bytes,
                           unsigned slot_count, weft_fanout_t* f, weft_shm_map_t* m);

/// Attach a broadcaster to an existing named session (producer restart /
/// handoff: frame numbering continues from the ring's latestSeq — the
/// Series-4 attach_writer contract).
int weft_fanout_shm_attach_writer(const char* name, weft_fanout_t* f,
                                  weft_shm_map_t* m);

/// Attach a reader to an existing named session (read_only=1 maps the ring
/// PROT_READ — the flight-recorder posture). One call per reader, N readers
/// per session, each fully independent (RFC 0004).
int weft_fanout_shm_attach_reader(const char* name, weft_fanout_reader_t* r,
                                  weft_shm_map_t* m, int read_only);

#endif // WEFT_SHM_RING_H
