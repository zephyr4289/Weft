# STUDIO-SEAMS-V1 — Weft Studio Managed Architecture & Seam Contracts

Status: NORMATIVE for Pillar 7 (managed territory)
Owner: Senior Engineer 3 (Managed Runtimes / UI Connectors / Studio Application)
Boundary: this document governs `packages/studio/`, `tests/studio/managed/`,
`examples/studio/`, `tools/studio/`, `docs/reports/D-73-*.md` ONLY.
Engineers 1 & 2 territory (`core/c/`, native inspector, WASM `weftc`) is consumed
strictly through the seams defined in §5 and §6.

---

## 1. The Two-Plane Law (STUI1)

Weft Studio partitions every pixel into exactly two planes:

| Plane   | Rendered           | Updated by                | React involvement            |
|---------|--------------------|---------------------------|------------------------------|
| COLD    | once on mount      | user interaction only     | `setState` allowed           |
| HOT     | once on mount      | rAF tick reading engine buffers directly via refs | FORBIDDEN (Law 4) |

Rules (mechanically audited):

1. A HOT component's function body MUST execute exactly once per mount.
   The **render spy** (`react-adapter.ts`) counts every invocation; the count is
   surfaced live in the Render Spy tool window and asserted by Stage 3 of the
   managed suite (`exactly 1` under a 10,000-frame burst).
2. Streaming updates MUST mutate a Canvas2D/WebGL2 surface obtained via a ref,
   or mutate pre-existing DOM text nodes (`textContent`), or paint into
   ImageData. They MUST NOT allocate per frame (audited by Stage 7 static scan:
   the frame-path function bodies contain no `new`, no object/array literals,
   no template literals, no closures created per call).
3. `setState` in HOT components is permitted only in mount/unmount lifecycles
   (`useEffect` body / cleanup), never in a data-update path.
4. Frame cadence is a governed 240 Hz virtual cadence (drop-not-queue):
   a late tick is SKIPPED, never queued. p99 frame work must fit the 4.166 ms
   budget (Stage 4).

## 2. Memory Map (the 1,000,000-slot map)

The studio's Ring Monitor attaches to a slot map identical in spirit to the
production ring: one `ArrayBuffer` (or `SharedArrayBuffer` when the host is
cross-origin isolated), `capacity` slots × `SLOT_SIZE = 32` bytes.

Slot layout (little-endian, 32 B):

```
off  size  field
0    4     seq        seqlock version: even = stable, odd = write in progress
4    2     writer_id  producing lane id
6    1     msg_type   1 = MARKET_TICK, 2 = IMU_SAMPLE, 3 = FRAME_EVENT,
                      4 = MUTATION, 5 = HEARTBEAT
7    1     flags      bit0 torn-read-observed, bit1 dropped-under-pressure
8    8     ts_ns      nanosecond virtual timestamp
16   16    payload    opaque 16 bytes (message body)
```

Slot states (Ring Monitor visualization): `FREE < WRITING < COMMITTED < READ`,
plus `DROPPED` (overwritten before read). State is derived from
`seq` + reader bookkeeping, never stored redundantly.

Default capacity: **1,000,000 slots** (32 MiB). Capacity is validated
fail-closed on attach (E_CAPACITY).

## 3. SREC1 — Studio Flight Record Sidecar

A managed, self-contained record stream for the Time-Travel panel. It is the
studio-side companion of the kernel `.weftrec` v4 (TIER4 #RFC-0014): same
philosophy (append-only, checksummed, deterministic replay), managed encoding.

```
header (32 B):
  0  4  magic "SREC"
  4  2  version = 1
  6  2  record_size = 40
  8  8  opened_ns
 16  8  schema_hash (FNV-1a 64 of schema text)
 24  4  record_count (written on close)
 28  4  crc32 of header[0..28]

record (40 B):
  0  8   ts_ns
  8  8   addr        slot base address (byte offset) of the mutation
 16  4   seq_old
 20  4   seq_new
 24  8   before      first 8 bytes of prior payload (FNV-1a folded)
 32  8   after       first 8 bytes of new payload (FNV-1a folded)
```

Replay fold (deterministic, RFC-0019 discipline): the replay state is a fixed
8-lane u64 shadow vector `S[8]`; applying record `r` performs
`S[r.addr % 8] = S[r.addr % 8] ^ r.before ^ r.after` (XOR-delta fold).
Checkpoints every 4096 records store the full `S`. Scrubbing to record `i` =
seed from nearest checkpoint ≤ i, fold forward. Two passes MUST produce
identical per-index hashes (Stage 5).

Crash export bundle (`weft-studio-crash.zip`-less, single file `*.srecburst`):
`SREC1 stream + 32-byte trailer {bundle_magic "SBURST", schema_hash, record_count, crc32}`.
One click in the Time-Travel panel; byte-identical round-trip asserted (Stage 5).

## 4. Render Spy Protocol

`react-adapter.ts` exposes `bindReact(impl)` + `spy`:

- `spy.counts: Int32Array` indexed by registered component id — incremented on
  EVERY function invocation of an instrumented component (the counter write is
  the only hot-path cost: one i32 increment, zero allocation).
- `spy.renders` — setState-initiated re-render requests (must remain 0 in hot
  planes during streaming).
- The Render Spy tool window reads `spy.counts` from the rAF tick and paints it
  into a canvas (never via setState).

## 5. Engineer 1 Seam — `weftc` WASM compiler & LSP

```ts
interface WeftcCompilerSeam {
  parseSchema(text: string): ParseResult;          // diagnostics + AST
  generate(target: CodegenTarget, ast: Ast): string;
  lspDiagnostics(text: string): Diagnostic[];      // squiggles
}
```

Studio attaches via `attachCompiler(seam)`. Until E1's WASM engine ships, the
managed mirror (`engine/schema.ts`, `engine/codegen.ts`) implements the same
contract and is the default seam (`ManagedCompilerSeam`). Attach must be
fail-closed: a seam that throws during `parseSchema` is detached and the
managed mirror re-engaged, with a status-bar incident badge.

## 6. Engineer 2 Seam — live shared-memory inspector

```ts
interface InspectorSeam {
  attach(map: RingMap): void;                      // native IPC / WebSocket
  detach(): void;
  pollWrites(since_seq: u64, out: DataView): i32;  // zero-copy pull
}
```

`attachInspector(seam)`; when absent, the studio drives `RingMap` from its own
deterministic simulators (§ engine/sim.ts) — the demo path. Both paths feed the
identical hot planes; no panel knows which is live.

## 7. Law-4 Taxonomy (studio subset)

`E_SCHEMA` parse failure · `E_CAPACITY` bad geometry · `E_TORN` seqlock torn
read (retried) · `E_DROPPED` overwritten before read · `E_SEAM` seam contract
violation · `E_EXPORT` bundle integrity failure. Every failure is surfaced in
the Problems tool window AND the status bar incident counter — never thrown
into the render loop.
