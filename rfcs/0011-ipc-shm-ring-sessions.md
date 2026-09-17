---
RFC: 0011
Title: Inter-Process Shared-Memory Ring Sessions
Status: Draft
Authors: weft-contributor
Created: 2026-09-17
Supersedes / Superseded-by: None
---

# RFC 0011 — Inter-Process Shared-Memory Ring Sessions

## Summary

Promote the RFC-0004 fan-out ring from cross-thread/cross-language to fully
**inter-process**: a 64-byte session header ("WFSH") on top of the
byte-compatible ring, carried in a POSIX `shm` object (or anonymous
`MAP_SHARED` mapping, or Windows named file mapping), so one writer process
and N reader processes exchange frames with **zero kernel context switches
in the data path** — attach/validate once, then pure shared-memory atomics.
Ships as a driver-layer module in C (`core/c/shm_ring.{h,c}`) and Rust
(`core/rust/src/shm.rs`), with the flight recorder migrated onto it.

## Motivation

RFC-0004's ring is already shared across threads and languages, and the
flight recorder already attaches to POSIX shm by name — but the plumbing
lives inside the tool (`tools/weft-fanout-rec`), with **no session
protocol**: geometry is guessed from a bare `fstat` size, nothing records
what the object is, and the C/Rust kernels expose no IPC surface at all.
Every real deployment shape named in the RFCs — a native engine process
feeding a renderer, a capture daemon watching a producer, cross-process
telemetry for the Inspector — currently has no in-tree primitive. The
strace evidence in `litmus/evidence/shm-ring/s-series.log` shows what the
road buys: 50,000 publishes + a claim execute with **zero syscalls of any
kind** between marker writes — no futex, no pipe, no socket, no `poll`.
This is the same latency class the kernel already delivers in-process;
this RFC extends it across the process boundary without touching the
kernel.

## Guide-level explanation

A producer creates a **named session** and publishes as usual:

```c
weft_fanout_t f; weft_shm_map_t m;
weft_fanout_shm_create("engine-frames", 256, 8, &f, &m);
uint8_t* p = weft_fanout_begin(&f);   /* fill; */
weft_fanout_publish(&f);
```

Any other process attaches by name — readers map read-only (the
flight-recorder posture), a successor writer continues the stream after a
crash (frame numbering resumes from `latestSeq`):

```c
weft_fanout_reader_t r; weft_shm_map_t mr;
weft_fanout_shm_attach_reader("engine-frames", &r, &mr, 1);
const weft_fanout_claim_t* c = weft_fanout_claim(&r);
```

Attach validates a full contract — magic, version, header size, reserved
zero bits, geometry reconciliation, and the exact mapping size — and
**refuses** anything else with an error, never guesses. The Rust twin is
`weft_core::shm::{create_named, attach_named, fanout_*}`. Anonymous
(`fork`-inherited) and fd-passed (memfd) sessions round out the POSIX
roads; Windows uses named file mappings behind the same API
(compile-gated, compile-verified by the windows CI leg).

## Reference-level specification

**Session object layout** (little-endian):

```
offset 0   magic "WFSH" (u32)      0x48534657
offset 4   version (u16)           1
offset 6   header_size (u16)       64
offset 8   flags (u32)             0 — unknown bits REJECT on attach
offset 12  payload_bytes (u32)     ring slot capacity (multiple of 4)
offset 16  slot_count (u32)        ring depth M
offset 20  ring_bytes (u64)        16 + 8M + M*payload_bytes (RFC-0004)
offset 28  creator_pid (u32)       advisory only (AXIOM T)
offset 32  created_unix_ns (u64)   advisory only (AXIOM T)
offset 40  reserved (24 bytes)     zero — nonzero REJECTS on attach
offset 64  the RFC-0004 ring, byte-identical to fanout.{h,c}/fanout.ts
```

- **Invariants touched** — none of I1–I6; the ring protocol (FI1–FI3,
  stamp-then-fill bracket, telescoping) is UNCHANGED and byte-identical.
  C11 atomics over `MAP_SHARED` mappings are cross-process-safe on every
  targeted platform (cache-coherent mappings of the same physical pages);
  the fanout.h ordering regime applies across address spaces unchanged.
  New session-level invariants: header written once by the creator before
  any publish; creator unlinks on destroy; attachers never unlink
  (single-creator contract, documented not defended).
- **Litmus impact** — new S-series (`core/c/shm_test.c`, mirrored in
  `core/rust/tests/shm_test.rs`): S1 header/ctrl invariants, S2 attach
  validation + cross-mapping visibility, S3 O_EXCL semantics, S4/S5 fork
  torture (anonymous and named roads; 3 readers x 200k frames, payload
  bit-exact, telescoping exact), S6 producer handoff (stamp monotonicity
  across processes), S7 fd road, S8 crashed-producer posture, S9
  bindings round trip. The fan-out native CI shard gains the S-series,
  the multi-process torture runner, and the strace zero-syscall gate.
- **Envelope impact** — none for frames. The SHM OBJECT gains the 64-byte
  session header: producers that previously posted a bare RFC-0004 ring
  must create sessions via the module (one call — see the FORMATS.md §3
  note). Sessions are ephemeral OS objects, not persisted files, so no
  version migration is required; the recorder's tools speak the session
  protocol end to end (selftest + e2e gates re-run green).

**Data path** — `weft_fanout_*` calls operate on `m.ring` exactly as on
any attached ring; no new hot-path API. Zero syscalls, zero allocation
(Law 2); every loop bounded by the ring protocol (Law 1); drops counted
by the existing telescoping identity (Law 4).

## Boundary of the claim (Law 4)

This RFC makes one shm object serve N processes with zero kernel
transitions **in the data path**. It does NOT: schedule processes
(priories/pinning are the OS's), provide cross-machine transport (that is
a network protocol, out of scope), or guarantee availability across a
crash beyond what the ring's stamps already prove (a crashed creator
leaves the object attachable — the recorder reads the tail; it does not
resurrect the producer). Windows named mappings are compile-verified
only. The header's advisory fields (pid, created_ns) are diagnostics,
never correctness references (AXIOM T).

## Alternatives considered

- **Do nothing** (tool-local shm, status quo): keeps the session
  contract private to one tool; every future consumer re-derives geometry
  from sizes; the kernels stay thread-only. Loses the zero-syscall road
  for everything except the recorder.
- **POSIX message queues / pipes**: kernel-mediated per message — the
  exact context switches this RFC removes; also breaks the RFC-0004
  claim/telescoping contract (queues are not latest-wins).
- **A dedicated IPC library (e.g. iceoryx-style)**: violates the
  kernel's dependency bar and duplicates a protocol we already have,
  proven byte-compatible across five languages.
- **eventfd/futex doorbells**: adds syscalls and wakeups to a path whose
  consumers poll at frame cadence by design (60–240 Hz UI tick); the
  ring's bounded claim IS the doorbell.

## Drawbacks

One more wire-format-adjacent contract to version (the header). Session
objects outlive crashed creators (by design) — an operator must GC stale
names, same as today's tool objects. The anonymous road is POSIX-fork
shaped (no Windows analog). Read-only mappings make the module's mmap
calls slightly more varied than a pure-ring module (three roads to test).

## Open questions

- Should the Inspector (demos/web) attach to shm sessions via a Node
  addon for the v3 timeline, or keep file-mediated capture? (File-mediated
  works today; direct attach is a follow-up, not part of this RFC.)
- memfd + `SCM_RIGHTS` passing as a first-class road (the fd-attach
  primitive exists; the socket ferry does not).

## Staff Decision

[EMPTY — implementation evidence attached; ratification pending]
