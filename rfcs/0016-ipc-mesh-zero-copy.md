---
RFC: 0016
Title: Zero-Copy IPC Mesh — Sealed Descriptor Exchange, Lock-Free Registry, Crash Healing
Status: Draft
Authors: weft-contributor
Created: 2026-09-19
Supersedes / Superseded-by: None
Extends: RFC-0011 (IPC SHM ring sessions — this RFC builds on its session object)
---

# RFC 0016 — Zero-Copy IPC Mesh: Sealed Descriptor Exchange, Lock-Free Registry, Crash Healing

## Summary

Promote Weft from inter-process ring **sessions** (RFC-0011: one
producer, N attacher processes, no way to find each other) to an
inter-process **mesh**: processes that have never met discover active
publishers, receive sealed read-only ring descriptors through an
authenticated handshake, verify liveness via heartbeats, survive
consumer and producer crashes without a coordinator, and resume a
crashed producer's stream with **exact cross-incarnation frame
accounting**. Ships as two driver-layer C modules —
`core/c/weft_shm.{h,c}` (the fabric: memfd + seals + SCM_RIGHTS +
handshake) and `core/c/weft_ipc.{h,c}` (the mesh: lock-free registry +
heartbeats + healing + HMAC capability tokens) — with the M/R-series
conformance batteries and the `ipc-torture` runner (100k+ cross-process
publications, crash-restart chaos, wake-up latency measurement) under
ASan and TSan. The kernel and every prior driver file stay byte-frozen.

## Motivation

RFC-0011 crossed the process boundary but left four gaps, each of which
blocks a real mesh:

1. **The anonymous road had no exchange protocol.** A memfd fd number is
   meaningless in another process; RFC-0011's `weft_shm_attach_fd`
   documents the fd-passing road but implements none of it. Nothing
   sealed the object: any fd holder could `ftruncate` it under every
   attacher that had already validated the geometry. Read-only mapping
   was consumer discipline, not protection.
2. **No discovery.** An attacher needed to know the session name out of
   band. A sidecar, a capture daemon, and a telemetry inspector could
   not find the live publishers in a machine — there was no registry,
   no liveness, no "who is publishing what".
3. **No crash story for the mesh (only for the ring).** RFC-0011/S6/S8
   cover producer handoff and orphaned tails on the *object* level; the
   mesh needs the *control-plane* version: crashed producers reaped,
   successors taking the name with a bumped epoch, consumers re-attaching
   without double-counting or phantom frames.
4. **No wake-up answer.** Volume II §3.6 rejected eventfd/futex
   doorbells on the data path (zero-syscall gate) but never measured
   what the doorbell-free alternative actually costs against them.

This RFC closes all four under the same constitution: Law 1 (every scan
bounded, park capped), Law 2 (zero allocation on any steady-state
path), Law 3 (mechanism in `weft_shm`, policy in `weft_ipc`, kernel
byte-frozen), Law 4 (every refusal explicit; every advisory field
labeled advisory).

## Guide-level explanation

A producer prepares one sealed anonymous session and serves handshakes;
consumers discover the session by name in the shared registry,
handshake for a descriptor, and claim frames exactly as in RFC-0011:

```c
/* producer */
weft_shm_memfd_t m;
weft_shm_memfd_create(256, 8, &m);      /* memfd + WFSH v1 + seals    */
weft_shm_memfd_ro_view(&m);             /* kernel-enforced RO fd      */
int ls; weft_shm_listen("engine", &ls); /* /tmp/weft-ipc/engine.sock  */
weft_ipc_registry_t reg;
weft_ipc_registry_open(&reg, 1, 0);     /* /dev/shm/weft_registry_v1  */
weft_ipc_session_t s;
weft_ipc_register(&reg, "engine", WEFT_IPC_TRANSPORT_MEMFD, 256, 8, 0, &s);
/* per consumer: accept + one serve(); per cadence: heartbeat() */

/* consumer (another process, discovered by name) */
weft_ipc_registry_t reg;  weft_ipc_registry_open(&reg, 1, 0);
weft_ipc_discovered_t d[4];
weft_ipc_discover(&reg, 1000, "engine", d, 4);   /* + pid liveness    */
weft_shm_memfd_t m;  weft_shm_grant_info_t g;
weft_shm_handshake_connect("engine", 0, d[0].session_id, NULL, &m, &g);
weft_ipc_consumer_attach(&reg, d[0].session_id);
weft_fanout_reader_t r;  /* reader_init on m.map.ring; claim as usual */
```

The three security planes, stated honestly:

- **The descriptor IS a capability.** The memfd is anonymous — reachable
  only by fd inheritance or explicit SCM_RIGHTS transfer. The producer
  derives an O_RDONLY view via `/proc/self/fd` reopen; an fd's access
  rights are fixed at open() time, so recipients of that view **cannot
  mmap PROT_WRITE — EACCES from the kernel** (M2f/M3b pin it, including
  across fork). `F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL` freeze the
  object size against every fd holder (M1e). F_SEAL_WRITE is *not*
  applied: it cannot coexist with the producer's own writable mapping
  (memfd_create(2)) — and it is not needed, because the RO view already
  gives MMU-level write protection.
- **The token is the control-plane key.** WRITE grants and
  successor-writer handoff require an HMAC-SHA256 token over canonical
  claims {session, perms, epoch, expiry, key_id, nonce}, issued by the
  producer's random session key (in-tree `hmac.{h,c}` — zero new
  dependencies). Forged tag, expired token, wrong session, stale epoch,
  insufficient perms: every path is an explicit GRANT denial, tested
  (M7). The data plane needs no crypto — the kernel already did the
  work.
- **The registry is cooperative infrastructure, NOT a security
  boundary.** `/dev/shm` is writable by the mesh's user population; we
  say so in the header rather than pretend. Its job is discovery and
  liveness with crash-safe bookkeeping — and its invariants hold even
  against a hostile scribbler only to the extent the CAS protocol does
  (a wiped registry degrades discovery, never the rings).

## Reference-level specification

### 1. The fabric (`weft_shm`)

**Sealed session create.** `memfd_create(MFD_ALLOW_SEALING)` →
`ftruncate(64 + ring_bytes)` → `pwrite` the RFC-0011 WFSH v1 header
(byte-identical — the same object, the same validation) →
`F_ADD_SEALS(GROW|SHRINK|SEAL)` → validated RW attach for the producer.
tmpfs zero-fill gives the fresh-ring invariants (latestSeq=0, all slots
invalidated).

**The RO view.** `open("/proc/self/fd/N", O_RDONLY|O_CLOEXEC)` with a
dev+ino+size identity check. Never falls back silently (Law 4): if the
reopen is unavailable, derivation fails loudly.

**SCM_RIGHTS primitives.** `weft_shm_send_fd`/`recv_fd` move exactly one
fd plus ≤480 data bytes; the cmsg is validated (SOL_SOCKET, SCM_RIGHTS,
exactly one descriptor, no truncation). A zero-length data message with
a cmsg is a message, not EOF (an fd alone is a complete handoff);
multiple descriptors are refused. Recipients get CLOEXEC fds.

**The handshake.** Fixed 96-byte HELLO (magic "WFHI", version, cmd
SUBSCRIBE/WRITER_HANDOFF/PING, requested perms, expected session id,
nonce, 64-byte token) and 64-byte GRANT ("WFHG", status, session id,
nonce echo, geometry, epoch, granted perms, current latestSeq —
advisory). The server encodes the geometry **from the session object
itself**, so the client's grant-vs-header cross-check is a real
consistency gate (M11 proves a lying server is refused with -3). The
nonce echo binds the grant to this hello (replay); the session binding
is enforced on OK grants. Anonymous SUBSCRIBE yields READ|CLAIM + the
RO view; anything stronger needs a verified token; WRITE grants hand
the RW fd (the co-processor road — M10). PING answers epoch + latestSeq
without a descriptor; WRITER_HANDOFF is an ADMIN-token-gated read grant
of the RO view (the successor seeds its numbering from the old ring —
the graceful-restart road; the crashy road reads the registry mirror).

**Bounded park.** `weft_shm_park` polls latestSeq (documented RFC-0004
offset 0, Acquire load) with a CPU pause, up to a caller-chosen spin
cap: **zero syscalls**, the house zero-syscall gate untouched, Law 1 by
construction. The ring's bounded claim remains the only doorbell
(Volume II §3.6); park is the latency-optimal way to *wait* for it.

### 2. The registry (`weft_ipc`)

**Object.** `/dev/shm/weft_registry_v1`, fixed 9984 bytes: 64-byte
header (magic "WFRE", version, entry count 62, entry size 160) + 62
entries. Opened create-or-attach with an O_EXCL race retry; validation
is exact-size + full-header; decoys are refused (R1). Never unlinked —
mesh infrastructure that outlives every process.

**Entry lifecycle — one atomic CAS word per slot** (state | generation):

```
        CAS(FREE, g -> RESERVED, g)       winner fills the fields
   RESERVED --release CAS--> ACTIVE       fields visible to scanners
   ACTIVE   --release CAS/store--> DEAD   clean detach (owner goodbye)
   DEAD/RESERVED-stale --CAS--> FREE(g+1) reaped by ANY process's heal
   ACTIVE-stale + dead pid --CAS--> DEAD  crashed producer, detected
```

The RESERVE step exists so registration fields are written **before**
publication; the activation release-store makes every ACTIVE snapshot
consistent for the immutable set (session id, name, geometry, epoch,
transport, pid) while heartbeats and counters stay advisory (AXIOM T).
The generation counter makes reaping ABA-safe (R9). Same-name
registration races are resolved by a one-sided dedup re-scan: the later
activation always sees the earlier one, so exactly one of two racing
registrants yields (R3: 8 threads × 30 cycles, zero duplicates).
**Every entry field access is a relaxed atomic** (the fanout.h
payload-word discipline): a registrant overwriting a DEAD slot has no
synchronizing edge against a concurrent scan of that slot — plain
bytes would be a strict-C11 race there (TSan proved it during
development; outcome-safety came from the CAS, but the house bar is
zero plain races, and relaxed atomics are free on x86).

**Heartbeats & discovery.** `heartbeat()` bumps a monotonic counter,
stamps wall time, refreshes the latestSeq mirror — 3 relaxed stores,
zero syscalls, zero allocation. `discover()` is a bounded scan
returning consistent immutable sets + advisory counters + a
`kill(pid, 0)` liveness verdict (heuristic: EPERM counts as alive — we
reap only on proof of death, never on failure to probe).

**Healing.** `weft_ipc_heal()` — any process, any time, safe
concurrent: stale **AND** dead-pid ACTIVE entries are crashed out to
DEAD; DEAD entries reaped to FREE (gen+1); abandoned RESERVATIONs
likewise. A clock jump alone cannot evict a live producer (the pid leg
must also fail). `weft_ipc_heal_ring()` applies the Axis-3
self-stabilizing contract (`weft_ring_health_check` /
`weft_ring_recover`) to a mapped ring (R11).

**Successor handoff.** `register()` with an existing name: a live
incumbent is refused (DUPLICATE, R5b — cross-process); a crashed+stale
one is taken over with **epoch+1** on the same slot; a cleanly-detached
tombstone likewise; a fully-reaped name starts fresh (continuity then
lives in the fresh session_id — documented). The successor seeds its
ring numbering from the registry mirror. Consumers re-attach via
`weft_ipc_reader_resync(reader, own_last_seq)`, which makes drop
accounting **span the incarnation gap exactly**: the outage telescopes
as genuine drops, never phantom frames. The continuity contract: the
mirror must not lag the published truth at crash time, i.e. a
per-frame heartbeat cadence buys exact accounting; a sparser cadence
trades outage-gap precision for fewer control-plane stores (both
cadences are exercised — torture-mesh uses per-frame, torture-memfd
every 4096).

**Capability tokens.** 64 bytes = 32-byte canonical claims
{session_id, perms, epoch, expiry_unix, key_id, nonce} || 32-byte
HMAC-SHA256(K_session, claims). Constant-time tag compare. Verification
sites: the producer's handshake verifier (perm-gated grants); the
registry's advisory key_id + tag publication (consumers cross-check a
proffered token's tag without holding the key). Explicit denial codes
end to end (M7, R6).

**Consumers.** attach/leave maintain an advisory count — exact under
clean lifecycles, honestly inflated by crashes (R7 pins both; the
torture pins the crash leak visible: 4 attached − 3 clean leaves = 1).

### 3. Invariants touched

None of I1–I6; none of FI1–FI3. The ring protocol is byte-identical
RFC-0004 over RFC-0011 sessions — the mesh is a pure control plane.
New session-level invariants: seals applied before any fd leaves the
producer; the RO view is the only anonymous export (an undrived session
answers SUBSCRIBE with DENIED_PROTOCOL rather than leaking the RW fd);
registry transitions are single-word CAS/release-store; immutable
fields are relaxed-atomic words.

### 4. Litmus impact

M-series (52 checks, `weft_shm_test.c`): M1 seals/geometry/publish-
through-seal; M2 the kernel-enforced RO view (EACCES); M3 SCM_RIGHTS +
fork child posture; M4 cmsg validation; M5 full handshake + 100-round
bit-exact claims; M6 named sockets incl. duplicate-listener refusal and
stale-socket takeover; M7 all six denial paths; M8 PING; M9 3-consumer
fork mesh × 10k frames; M10 WRITE grant; M11 lying-server geometry
refusal; M12 bounded park. R-series (65 checks, `weft_ipc_test.c`):
registry lifecycle + decoys; session lifecycle verbatim; threaded
registration dedup; crash detection (both legs); successor epochs;
token crypto; consumer counting; cross-process discovery; heal
idempotence + ABA; monitor mode; Axis-3 ring healing. `ipc-torture`:
memfd (100k publications, mid-claim SIGKILL victim, ring HEALTHY,
telescoping exact), mesh (P1 crash → successor epoch 2 → 120k total
publications with per-consumer fresh+drops == 120,000 exactly),
latency (park vs eventfd). Sanitizers: M/R × ASan; R × TSan (zero
races); torture memfd/mesh × ASan; torture latency plain-only
(sanitizer instrumentation distorts ns-scale timing — declared; its
memory-safety surface is covered by the other ASan legs).

### 5. Envelope impact

None for frames (WFSH v1 unchanged, byte-identical). New persistent-ish
OS objects: the registry file (documented in FORMATS.md §5) and
`/tmp/weft-ipc/<name>.sock` listeners (stale files are taken over
explicitly: probe-live-then-unlink).

## Boundary of the claim (Law 4)

This RFC claims a **single-machine** zero-copy mesh with
kernel-enforced read-only distribution, cooperative discovery, and
self-healing bookkeeping. It does NOT: provide cross-machine transport
(a network protocol — out of scope); enforce access control against
processes that can already write `/dev/shm` (the registry is
cooperative — stated in the header); guarantee wake-up semantics better
than bounded polling (park is Law-1-capped CPU work, not a blocking
primitive — the measurement is the claim, not a latency guarantee);
or protect against a compromised *producer* (tokens bind consumers to
the producer's policy; they do not bind the producer to anyone).

**Declined vectors, with reasons** (the innovation menu from the
mission): a DMA-BUF/AHardwareBuffer bridge is NOT implemented — no GPU
driver backend exists in this sandbox and the F-2 precedent forbids
simulation-only claims; the design slot is the WRITE-grant road (an
importer receives the RW fd exactly as a Vulkan/AHB importer would
receive a dma-buf), so the mechanism composes when real hardware
arrives. Hardware-assisted capability tokens ARE implemented — the
honest split: kernel object-capability (memfd anonymity + open()-time
access rights + seals) for the data plane, HMAC tokens for the control
plane. The novel synchronization primitive IS implemented — bounded
park, measured against eventfd cross-process: p50 983–1024 ns vs
4493–4603 ns (park ≈ 4.5× faster to observe a publish), with the
trade-off stated (park burns capped CPU while waiting; eventfd blocks
at zero CPU). Zero-syscall data path: inherited from RFC-0011's
committed strace evidence (no strace in the current sandbox — the
S-series gate is cited, not re-run; every new data-path call site is
the same ring API that gate covers).

## Evidence

`litmus/evidence/ipc-mesh/`: m-series{,-asan}.log (52/52 ×2),
r-series{,-asan,-tsan}.log (65/65 ×3, TSan zero warnings),
torture-memfd{,-asan}.log, torture-mesh{,-asan}.log,
torture-latency{,-asan}.log (all 0 failures), and the full local shard
run (ci/run-artifacts/shard-ipc-mesh.log pattern, reproducible via
`ci/scripts/run_ipc_mesh_shard.sh`). Throughput is informational
(sandbox, 2 vCPU): 100k × 256B frames at ~0.5M pub/s with four
consumer processes claiming concurrently. The normative results are the
invariants: bit-exact payloads, exact telescoping (including across a
producer crash), ring HEALTHY after a mid-claim consumer death, honest
advisory counters, kernel-enforced EACCES/EPERM posture.
