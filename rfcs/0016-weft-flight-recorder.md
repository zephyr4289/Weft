---
RFC: 0016
Title: weft_trace — Continuous Lock-Free Flight Recorder & Perfetto Bridge
Status: Draft
Authors: Engineer 3 (Observability Fabric & Reactive Dataflow)
Created: 2026-09-20
Supersedes / Superseded-by: None
Requires: 0014 (.weftrec v4 event standard — container stays byte-frozen), 0008 (freshness telemetry)
---

# RFC 0016 — weft_trace: the continuous lock-free flight recorder & Perfetto bridge

## Summary

RFC-0014 gave Weft a standardized **event codec**: a `.weftrec` v4 container is
a pure function of a kernel run's decision stream, byte-identical across every
port. But a codec is not a recorder. Recording today means `tools/weft-record`
— a harness you attach, run, and stop. Production concurrency bugs do not
schedule themselves around harnesses. The failure you need to explain happened
ninety seconds ago, on a device you no longer control.

This RFC specifies **`weft_trace`**, a continuous flight recorder that lives
in-process for the entire lifetime of the application and answers, at any
moment, "what did the kernel do in the last N events?" — with the same three
properties the kernel itself has:

1. **Wait-free emission.** `weft_trace_emit` performs no CAS, no retry loop,
   no system call, no allocation (Law 1, Law 2). Worst-case cost is one cache
   line of stores. It is safe to call from the writer thread inside the
   publish path — the hottest path Weft has.
2. **Zero lock contention — by construction, not by tuning.** Every producer
   owns a private SPSC shard. Producers never touch shared mutable state;
   contention is structurally absent, not merely small.
3. **Always the newest history.** When a shard is full the recorder
   overwrites the OLDEST events (it is a flight recorder, not an audit log);
   the lap is counted, never hidden.

Plus **the bridge**: a deterministic converter from (v4 container + sidecar)
to Chrome/Perfetto JSON — the format `ui.perfetto.dev` renders as an
interactive timeline of VSYNC alignment, claim latencies, governor decisions,
and drop bursts.

## Non-goals

- **The v4 container format does not change.** Its kind registry (8 kernel
  kinds) is closed; its event stream must remain a pure function of the
  kernel scenario (that is what makes xlang-trace byte-identity possible).
  Every runtime-only fact lives in the sidecar (§4).
- **No daemon, no sockets, no background threads.** The recorder is inert
  memory + arithmetic. Draining is the host's decision.

## Specification

### §1 The view of time: two surfaces, one trace

The recorder produces two artifacts, exactly per the RFC-0014 header's
sidecar provision ("timestamps live in the container's sidecar or the replay
tool, never in the event stream"):

| Artifact | Contents | Determinism contract |
|---|---|---|
| `.weftrec` v4 container | kernel decision events (kinds 1–8), arrival order | pure function of the scenario — byte-identical across ports (RFC-0014 gate) |
| `.wsid` sidecar v1 | timeline records: wall/monotonic timestamps, producer ids, runtime kinds, back-references | pure function of (scenario, injected clock) |

The sidecar is what makes the Perfetto view *temporal*; the container is what
makes the trace *cross-port*. A replay tool needs only the container; a
timeline view needs both.

### §2 Producer shards

```
weft_trace_t
├── shard[0] — writer thread   (binds: PUBLISH / DROP / REVOKE events)
├── shard[1] — reader thread   (binds: CLAIM / ACK / STALL / TEAR / CANARY)
├── shard[2] — governor thread (binds: governor actions, trend verdicts)
├── shard[3..N-1] — display / host / user lanes
└── t_* global counters (Relaxed; advisory per AXIOM T)
```

- `WEFT_TRACE_SHARDS_MAX = 8`, capacity per shard is a power of two, set at
  init (64 ≤ cap ≤ 2^20). Slots are 32 bytes (two per cache line):
  `{u16 kind; u16 aux; u32 data; u32 producer; u64 t_ns; u32 stamp; u32 pad}`
  where `stamp` is the slot's sequence ticket (see §3).
- **Thread discipline:** a shard is bound to exactly one producer thread.
  Emit only from the binding thread (same discipline as `w_work` /
  `r_work` — the kernel's existing thread-privacy rule). Cross-thread
  emission is a caller error, detectable in tests, defended by assertion in
  the debug build only (the hot path pays nothing).

### §3 Wait-free lossy emission (the heart)

Each slot carries a monotonically increasing **ticket**. Producer-private
state: `head` (next ticket). No shared mutable state is written by the
producer except the slot itself:

```
emit(kind, aux, data, t_ns):
    slot   = &shard->slots[head & (cap-1)]
    slot'  = {kind, aux, data, producer, t_ns, stamp: ++head}
    // single u32 Release store publishes all prior stores (stamp is last)
    // head full? the slot we just wrote WAS the oldest — the lap is
    // accounted by the consumer on resync (§5), never by the producer
    // (the producer performs NO reads of shared state — zero coherence
    // traffic beyond the one dirty line it must write anyway)
```

Worst case: ~10 ns. No loop exists to bound (Law 1: there is nothing to
spin on). No allocation exists to collect (Law 2). The kernel's forbidden
patterns are all trivially absent — the recorder contains **zero atomics
that participate in any ownership decision**; it is not a second kernel.

### §4 Sidecar record (`.wsid` v1)

```
HEADER (32 bytes)
  0   u32 magic      "WSID" = 0x44495357 (LE)
  4   u32 version    1
  8   u32 flags      0
  12  u32 reserved   0
  16  u32 rec_count
  20  u32 crc32      CRC-32/zlib over bytes 0..20 (RFC-0014 §1.4 polynomial)
  24  u64 t_epoch_ns producer-agnostic clock epoch (0 in deterministic tests)

RECORD (20 bytes, fixed)
  0   u64  t_ns        injected monotonic timestamp
  8   u32  back_ref    index into the paired v4 event stream, or 0xFFFFFFFF
                       for runtime-only records
  12  u16  producer    shard id
  14  u16  kind_ext    runtime kind (registry below)
  16  u32  data        kind-scoped
  20  (records are NOT individually CRC'd — the header CRC covers the
       structure; records are 20-byte fixed and validated by position.
       Rationale: the sidecar is advisory context for humans and the
       Perfetto bridge; the decision stream's integrity lives in the v4
       container's per-record CRCs. One mechanism per guarantee.)
```

**Runtime kind registry (`kind_ext`, u16 — OPEN but declared here first):**

Runtime kinds live in `[16, 0x8000)` — disjoint from the kernel kinds
`1..8` (the u16 kind space carries both surfaces; a collision would make
the v4 export filter misclassify, so the gap is a tested invariant).

| kind | name | data semantics |
|---|---|---|
| 16 | `VSYNC_TICK` | display frame counter |
| 17 | `GOVERNOR_ACTION` | bits 31..24 action kind, bits 23..0 skip_n/param |
| 18 | `TREND_VERDICT` | bits 31..24 verdict, bits 23..0 level q16 |
| 19 | `GC_PAUSE` | pause duration in ns (capped u32) |
| 20 | `RING_DEPTH` | observed depth at sample time |
| 21 | `FRESHNESS` | framesBehind at sample time |
| 22 | `CLAIM_LATENCY` | publish→claim latency ns (capped u32; needs both endpoints — emitted by the reader on claim, using the writer's stamped t_ns of the claimed seq from the shard scan) |
| 23 | `DROP_BURST` | coalesced drop count |
| 24 | `MARKER` | user annotation (devtools) |

### §5 Drain: the K-way merge (consumer side)

`weft_trace_drain(r, out, out_cap, &count)` reads all shards and merges by
`t_ns`, tie-broken by (producer, arrival). Bounded: O(out_cap · log K), K ≤
8, the merge heap lives in a caller-provided buffer (24 · K bytes). Zero
allocation.

**Resync rule (lossy read):** the drainer tracks its last-read ticket per
shard. If the writer lapped (a slot's stamp jumped more than `cap` ahead of
the expected sequence), the drainer counts `t_lap_dropped += missed` for the
shard, resyncs to `newest - cap`, and continues. A slot read with an
in-flight stamp (torn by a racing emit) fails validation and retries ONCE;
two consecutive failures resync. The drain is *eventually consistent per
shard and merge-ordered across shards* — declared, not assumed: drain output
is ordered by `t_ns`, but two events closer than the drain epoch may swap
across shards (they are distinct only by producer tie-break). The Perfetto
bridge tolerates this by construction (§7); deterministic tests never race
(drained between steps).

### §6 Export

- `weft_trace_export_v4(r, buf, cap)` — kernel kinds (1–8) in merged order →
  a valid `.weftrec` v4 container via the RFC-0014 codec
  (`weft_trace_writer_*`). Runtime records are OMITTED (the container stays
  a pure function of the kernel scenario).
- `weft_trace_export_sidecar(r, buf, cap, t_epoch_ns)` — the `.wsid` v1
  file. Every v4 event emitted through the recorder gets a back-referenced
  sidecar record; runtime kinds get `back_ref = 0xFFFFFFFF`.
- `weft_trace_shard_mmap_hint()` — shards are plain 64-byte-aligned memory;
  a host MAY back them with `mmap(MAP_SHARED)` and hand the region to
  `weft_trace_init_at` (the one init-time placement API). Crash-tolerant
  post-mortem = mmap a shard file, run the resync-drain over it. No daemon
  was harmed.

### §7 The Perfetto bridge

`tools/perfetto/weftrec2perfetto.mjs` — zero-dependency Node 18+:

```
node weftrec2perfetto.mjs capture.weftrec [--sidecar capture.wsid] [--out trace.json]
```

Mapping (Chrome JSON / `traceEvents[]`, consumable verbatim by
`ui.perfetto.dev`):

| Source | Perfetto form |
|---|---|
| v4 PUBLISH/CLAIM/DROP/REVOKE/ACK/STALL/TEAR/CANARY_FAIL | instants (`ph:"i"`) on `lane:writer` / `lane:reader` tracks + async flow links PUBLISH(seq)→CLAIM(seq) (`ph:"b"`/`ph:"e"`, cat "flow") |
| sidecar `VSYNC_TICK` | instants on `lane:display` |
| sidecar `GOVERNOR_ACTION` | slices (`ph:"B"`/`ph:"E"`) colored by action on `lane:governor` |
| sidecar `TREND_VERDICT` | instants on `lane:governor` |
| sidecar `GC_PAUSE` | slices on `lane:host` |
| sidecar `RING_DEPTH`, `FRESHNESS`, `CLAIM_LATENCY`, `DROP_BURST` | counter tracks (`ph:"C"`) |
| sidecar `MARKER` | instants on `lane:user` |

**Determinism (Law 4):** identical inputs → byte-identical JSON. Events are
sorted by `ts` with stable tie-break `(pid, tid, name)`; counters round-trip
through fixed-point integers only; the JSON is emitted with a fixed key
order. A golden-file test pins the exact output hash.

### §8 Laws compliance

- **Law 1 (no unbounded spins):** emit has no loop; drain is bounded by
  `out_cap`; the single validation retry is a declared constant.
- **Law 2 (zero hot-path allocation):** emit: none. Drain/export: caller
  buffers. `init`: the only allocator (kernel precedent).
- **Law 3 (mechanism, not policy):** `weft.c` / `weft.h` untouched — the
  recorder observes through its public API and telemetry counters. All code
  lives in `core/c/weft_trace_*`, `tools/perfetto/`, and port mirrors.
- **Law 4 (deterministic testing):** timestamps are injected; the fixture
  suite drives shards, laps, resyncs, and cross-port v4 parity
  (fixtures/xlang-trace gains a recorder path); the Perfetto bridge has a
  golden-hash test.

## Test plan

`core/c/weft_trace_test.c` (T-series):

- T1 emit/ drain ordering, single + multi-shard merge order
- T2 lap/overwrite-oldest accounting (`t_lap_dropped` exactness)
- T3 torn-read resync (forced by draining mid-emit under an interleaving
  schedule) — no torn event ever exported
- T4 v4 export byte-identity vs direct RFC-0014 encoding of the same
  scenario
- T5 sidecar structure, back-references, header CRC
- T6 zero-allocation proof (emit/drain/export over a malloc-counting
  allocator shim)
- T7 shard-binding discipline (debug assertion trips on cross-thread emit)
- T8 wait-freedom probe: emit latency histogram under concurrent drain —
  p_max bounded by the single-store bound, no outlier tail

## References

- RFC-0014 §1.4 (CRC), §kinds (closed registry), header sidecar provision
- RFC-0009 G5 (cross-language trace parity — the pattern the replay fold
  reuses), RFC-0012 (integer filtering precedent for the sidecar's
  fixed-point counters)
- Vyukov bounded MPMC (the shard is its SPSC, lossy, overwrite-oldest
  specialization — the producer never CASes the consumer cursor, which is
  the difference between "lock-free" and "wait-free with lossy backlog")
