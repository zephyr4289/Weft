# HOTPLANE-LAYOUT-V2 — The heddle-2.0 Unified Hot-Plane Memory Layout

**Status: NORMATIVE.** Implemented by `core/c/heddle/heddle_hotplane.{h,c}`
(Pillar 4, heddle-2.0 core hot-plane memory engine). Mirrored byte-exactly
by `core/wasm/heddle_bridge.c` (WASM/FFI bridge) and
`packages/heddle-hotplane` (JavaScript / SharedArrayBuffer / Atomics).
Conformance: `core/c/heddle/hotplane_unit_test.c` (H-series),
`torn_read_torture.c` (T-series), `core/wasm/heddle_bridge_test.c`
(W-series), `packages/heddle-hotplane/test/golden.test.mjs` (J-series),
gated by `ci/scripts/run_heddle_shard.sh`.

Magic: `WHP2` = `0x57485032` (hex digits spell the ASCII glyphs
`W`=`0x57`, `H`=`0x48`, `P`=`0x50`, `2`=`0x32`; serialized
little-endian the field occupies bytes `32 50 48 57` at offset 0).

---

## §1 Scope and positioning

heddle-2.0 bridges ultra-high-frequency telemetry (100k+ msgs/sec)
directly onto physical display refresh (60/120/240 Hz) with **zero GC
pauses, zero copies, zero serialization, zero widget re-render
storms**. The hot plane is ONE shared memory region — POSIX `mmap`
(MAP_SHARED), a `SharedArrayBuffer`, or WASM linear memory — in which
producers (C/Rust/Worker threads, or a WASM module) publish samples
while the render thread reads consistent frames **without ever
blocking, locking, or allocating**.

The plane provides:

* a **lock-free two-store seqlock** (§4) for tear-free consistent reads;
* a **signal-driven dirty plane** (§6): a 64-bit atomic dirty mask plus
  per-lane bounding boxes, so shaders skip un-mutated series instantly;
* **dual-mode lanes** (§7): continuous-streaming rings (oscilloscopes,
  audio, point-clouds) and direct-indexed state cells (order books,
  gauges, HUDs);
* per-lane **min/max/current statistics** (§5);
* a **zero-copy WASM/N-API/FFI bridge contract** (§9).

## §2 Memory model, alignment, byte order

* The region begins 64-byte aligned; its total size is
  `plane_size` (§3) and every sub-structure inherits 64-byte alignment.
  Lanes/slots/cells are cacheline-bracketed: independently mutated
  cells never share a cacheline (no false sharing).
* **All multibyte fields are little-endian.** Non-atomic fields are
  formatted explicitly via the `hplane_le16/32/64_put/get` helpers
  (plain moves on LE hosts, honest byte-swaps on BE hosts). Atomic
  registers (`u64`) are operated on by `__atomic` builtins whose
  native representation **is** the LE byte image on every supported
  target (x86-64, aarch64-LE, wasm32). A big-endian host is REFUSED at
  attach via the endian canary (§3) with `HEDDLE_E_ENDIAN` — never
  silently mis-operated.
* All shared-memory accesses (registers AND payload chunks) are
  `__atomic` builtins — 8-byte aligned chunked copies for payloads.
  Consequences: data-race-free by construction (TSan-clean), and
  atomicity of each ≤8-byte access is hardware-guaranteed. Consistency
  *across* fields comes exclusively from the two-store protocol (§4) —
  never from assumed multi-field atomicity.
* WASM: `i64.atomic.*` requires the threads/atomics feature
  (Emscripten `-pthread`). JS consumes the plane through
  `Atomics` on a `BigUint64Array` view — sequentially consistent,
  which is STRONGER than the engine's acquire/release ordering, so
  protocol correctness is preserved by construction.

## §3 The layout

### §3.1 Plane header — 128 bytes (2 cachelines)

Cacheline 0 (0x00–0x3F): identity + static geometry, **immutable after
create**; `static_crc32` covers bytes [0x10,0x40).
Cacheline 1 (0x40–0x7F): the **synchronization plane** — deliberately
the ONE shared cacheline (producer commit rate + consumer frame rate
traffic; all cross-thread coordination lives here by design).

| Off | Size | Field | Semantics |
|-----|------|-------|-----------|
| 0x00 | u32 | `magic` | `0x57485032` ("WHP2") |
| 0x04 | u16 | `ver_major` | 2 (this specification) |
| 0x06 | u16 | `ver_minor` | 0 |
| 0x08 | u32 | `header_size` | 128, self-describing |
| 0x0C | u32 | `static_crc32` | CRC-32/IEEE over [0x10,0x40) |
| 0x10 | u32 | `layout_rev` | 1 (layout revision within V2) |
| 0x14 | u32 | `mode` | 0 = ring streaming, 1 = state snapshot |
| 0x18 | u32 | `lane_count` | 1..64 (the dirty-mask width) |
| 0x1C | u32 | `lane_stride` | bytes per lane data region, 64-mult |
| 0x20 | u32 | `sample_size` | 1..4080 |
| 0x24 | u32 | `slot_capacity` | slots (ring) / cells (state) per lane |
| 0x28 | u64 | `plane_size` | total region bytes |
| 0x30 | u64 | `create_stamp_ns` | diagnostics (deterministic in goldens) |
| 0x38 | u32 | `stat_kind` | 0 = u64, 1 = i64, 2 = f64 (§5) |
| 0x3C | u32 | `flags` | `F_MULTI_PRODUCER`=1, `F_BBOX`=2, `F_STATS`=4 |
| 0x40 | u64 | `epoch` | session epoch (§8: multi = fetch-add total; single = derived from `commit_seq`) |
| 0x48 | u64 | `begin_seq` | two-store #1 — session enters flight |
| 0x50 | u64 | `commit_seq` | two-store #2 — COMMIT, the final store |
| 0x58 | u64 | `dirty_mask` | active dirty mask, lane i = bit i |
| 0x60 | u64 | `render_frame_id` | consumer frame counter |
| 0x68 | u64 | `heartbeat_ns` | last producer touch (diagnostics; cold path) |
| 0x70 | u32 | `endian_canary` | `0x0DD0C0DE`, LE |
| 0x74 | u32 | `reserved0` | 0 |
| 0x78 | u64 | `dirty_transitions` | monotonic 0→1 mask transitions (§6) |

`plane_size = 128 + lane_count*64 + lane_count*lane_stride`, where
`lane_stride = slot_capacity * stride` and

* ring: `stride = max(64, align64(16 + sample_size))` (16B slot header §3.3)
* state: `stride = max(64, align64(sample_size))` (payload at cell base)

The derivation is deterministic; attach re-derives and REFUSES any
deviation (`HEDDLE_E_LAYOUT`).

### §3.2 Lane descriptor — 64 bytes, array at offset 128

One per lane; producer-owned (the lane's single owner writer);
consumers read via the lane's own two-store pair.

| Off | Field | Semantics |
|-----|-------|-----------|
| 0x00 | u64 `lane_min` | running min (raw stat bits, §5) |
| 0x08 | u64 `lane_max` | running max |
| 0x10 | u64 `lane_current` | state: latest sample raw bits; ring: monotonic push count (head) |
| 0x18 | u64 `lane_begin_seq` | per-lane two-store #1 |
| 0x20 | u64 `lane_commit_seq` | per-lane two-store #2 — COMMIT |
| 0x28 | u64 `lane_commit_cnt` | lane sessions committed (version counter) |
| 0x30 | u64 `bbox_packed` | hi32 min idx \| lo32 max idx (§6) |
| 0x38 | u32 `lane_flags` | `F_ACTIVE`=1, `F_BP_MARK`=2, `F_OVERRUN`=4 |
| 0x3C | u32 `lane_misc` | hi16 owner_tag \| lo16 overrun cycle count |

Initial (no-data) state: `min=UINT64_MAX, max=0, current=0,
commit_cnt=0, bbox=EMPTY` — callers detect "no samples yet" via
`commit_count == 0`.

### §3.3 Ring slot — 16-byte header, payload at +0x10

Slot *i* of lane *L* lives at `128 + lane_count*64 + L*lane_stride +
i*slot_stride`. The slot's two-store pair carries the **push ordinal**
*n* (1-based, monotonic per lane): push #n writes slot
`(n-1) % slot_capacity`.

| Off | Field | Semantics |
|-----|-------|-----------|
| 0x00 | u64 `begin_seq` | ordinal enters flight |
| 0x08 | u64 `commit_seq` | ordinal COMMIT — final store |
| 0x10 | payload | `sample_size` bytes |

State cells carry NO header — payload sits directly at the (64-byte
aligned) cell base.

## §4 The two-store seqlock (normative protocol)

A session id *S* (monotonic, +1 per session) is published as TWO
ordered 64-bit stores into one cacheline:

```
store #1: begin_seq  = S     (session enters flight)
    < payload writes, signals, bookkeeping >
store #2: commit_seq = S     (COMMIT — the FINAL store of the session)
```

A frame is **quiescent-consistent** iff `begin_seq == commit_seq`.
Because *S* increments by exactly 1, the transient window between the
stores exposes `begin = S+1, commit = S`, which readers detect.

**Reader protocol (open → read → close):**

```
open : c = load(commit_seq, acquire); b = load(begin_seq, acquire)
       if c != b -> session in flight, RETRY (bounded)
read : payload (8-aligned relaxed atomic chunks)
close: b2 = load(begin_seq, acquire)
       if b2 != c -> a writer raced the read, RETRY (bounded)
```

**Writer memory ordering** (per session, single owner):

```
begin : store begin_seq (relaxed);  fence release
body  : payload + signal writes
commit: fence release;  store commit_seq (relaxed)
```

On x86-64/TSO both fences compile to compiler-only barriers; the
protocol's instruction stream is two shared stores (measured **below
the 1 ns timer quantum** per commit — B1c). On weakly-ordered targets
the release fences order the body before the COMMIT store; the
begin-store/payload ordering relies on the fence after store #1 plus
TSO-equivalent behavior of same-line stores on all supported targets.

**Bounded retries (Law 4):** every reader loop has a
compile-time-visible bound (`max_retries`, default 64). Exhaustion
REFUSES with `HEDDLE_E_SEQ_TORN` — torn data is never returned as
data. A producer crashing between the two stores leaves a permanently
in-flight pair → honest refusal with an explicit error, forever.

The identical protocol runs at three granularities: the **plane pair**
(single-producer whole-plane sessions), the **lane pair** (per-lane
sessions; also driven inside plane sessions for every touched lane),
and the **slot pair** (per ring push, ordinal-tagged).

## §5 Lane statistics

Every mutation updates the owning lane's `min/max/current` with the
sample's raw bits — the first `min(8, sample_size)` bytes,
LE-decoded, zero-padded. Comparison uses a per-`stat_kind` total-order
key:

* `u64`: raw bits;
* `i64`: `raw ^ 0x8000000000000000` (two's-complement sign flip);
* `f64`: `(raw >> 63) ? ~raw : raw | 0x8000000000000000`
  (IEEE-754 total order; negative NaNs order below −∞, positive NaNs
  above +∞).

Seeding: the first mutation of a lane (detected by the unreachable
sentinel signature `min==UINT64_MAX && max==0`; `min<=max` is an
invariant afterwards) seeds both `min` and `max`. Stats are read under
the lane's two-store bracket — always a consistent snapshot.

## §6 The signal plane (dirty mask, bounding boxes, frames)

### §6.1 Dirty mask and the transitions identity

`dirty_mask` bit *i* is set when lane *i* has been mutated since the
last harvest. Producers batch bits per session and flush with ONE
`fetch_or` at session close (single-producer planes: in `commit_end`;
multi-producer planes: in `lane_end`) — the "<3 ns dirty update" is
this register op. `dirty_transitions` counts 0→1 transitions; because
`fetch_or`/`exchange` on one register are serialized, the identity

```
dirty_transitions == SUM(popcount(every harvest result))
                   + popcount(final mask)
```

is EXACT. The T-series torture asserts it over millions of updates —
any lost, swallowed or double-flushed signal breaks it.

### §6.2 Harvest, remark, frames

The render thread, once per frame:

1. `mask = exchange(dirty_mask, 0)` — the frame barrier;
2. for each set bit: read the lane (two-store bracketed); harvest its
   bbox (§6.3);
3. `frame_commit()` — `render_frame_id++`.

If a lane read ended torn (`E_SEQ_TORN`) or required retries, the
renderer **re-marks** the lane (`remark(bit)`) and defers it to the
next frame. This makes the mask protocol **lossless**: a deferred lane
is always re-signalled, never silently dropped. The epoch probe
("did anything change") lets idle frames skip the entire plane.

### §6.3 Bounding boxes (producer-owned registers)

`bbox_packed` records the mutated index range. The register is written
ONLY by the lane's owning producer:

* **expansion** — inside every session, per mutated index;
* **reset** — at session start iff the consumer's `render_frame_id`
  has advanced since this producer's last reset (checked via the ctx's
  private `frame_seen`). The reset happens INSIDE the open session
  bracket, so consumers bracketing on the lane pair never observe the
  empty window.

Consumers read the bbox under the lane's two-store bracket (no
exchange, no dual-writer race — an earlier V2 draft had the consumer
exchange-reset the register, which could clobber a concurrent producer
expansion; the producer-owned design eliminates that class entirely).
A dirty bit therefore always carries a non-empty range (T-series
asserts zero violations across millions of updates). `EMPTY` is
returned only for lanes never written; renderers treat
`EMPTY`-with-dirty-bit as "render the whole lane" (conservative).

Ring-mode bboxes track slot indices modulo capacity (best-effort hint;
the authoritative ring mechanism is the head watermark + per-slot
ordinals).

## §7 Dual-mode lanes

### §7.1 Ring (continuous streaming)

Append-only per lane: `push` writes slot `(n-1) % cap` under the slot
pair, then releases the head (`lane_current = n`). Consumers
acquire-load the head, then read ordinals `≤ head`:

* slot `commit == seq` → exact frame;
* `commit > seq` → `HEDDLE_E_OVERRUN` (data lost to wrap — honest);
* `commit < seq` → `HEDDLE_E_NOT_PUBLISHED`;
* `begin != commit` → in-flight, bounded retry.

The producer NEVER blocks. Wrap (a full cycle beyond capacity) sets the
in-band backpressure marks `F_BP_MARK | F_OVERRUN` and increments the
overrun cycle count in `lane_misc` — the "ACK-equivalent" never leaves
the data plane.

### §7.2 State (snapshot registers)

Direct-indexed cells; a mutation writes the cell inside the lane
session (bbox + stats + dirty signal alongside). Unwritten cells read
as zero (deterministic create). `lane_snapshot` copies a whole lane
under one bracket.

## §8 Producer modes and the epoch

* **Single-producer (default):** one producer thread drives global
  sessions (`commit_begin/end`); every touched lane's pair is driven
  inside the session (session ids are shared, so plane and lane pairs
  show identical version numbers). The commit path contains **zero
  RMWs**: `epoch` is DERIVED by readers from `commit_seq` (the values
  coincide — the committed session id IS the plane epoch).
* **Multi-producer (`F_MULTI_PRODUCER`):** each lane is owned by
  exactly one producer thread driving per-lane sessions
  (`lane_begin/end`). `lane_begin` refuses a lane already in flight
  (`HEDDLE_E_STATE`) — the ownership-contract tripwire. `epoch` is the
  fetch-add total of lane sessions.

Consistency scope (honest boundary): multi-producer planes are
**per-lane consistent** with a globally monotonic epoch; strict
cross-lane snapshots require single-producer mode.

## §9 Bridge and JavaScript mirror contract

The bridge (`core/wasm/heddle_bridge.c`) re-derives every offset from
this specification as pure arithmetic and exposes: a pointer
descriptor (`hedbridge_describe`), offset helpers, fixed-order 64-bit
atomics, chunked payload copies, and a full session-write path
(`hedbridge_session_write`). The same source compiles to wasm32 linear
memory (Emscripten `-pthread`) and to native N-API/FFI consumers.

The JavaScript engine (`packages/heddle-hotplane`) mirrors the layout
over a `SharedArrayBuffer` with `Atomics` on a `BigUint64Array` view.
The **golden interop proof**: `golden_dump.c` emits a deterministic
plane image + JSON descriptor; the C unit suite, the bridge suite and
the JS suite all attach to that image and agree byte-exactly (37 JS
checks, 21 bridge checks, 145 C checks; golden regenerated 3× must be
byte-identical — a CI gate).

## §10 Error ladder (Law 4 — unique named refusals)

| Code | Name | Meaning |
|------|------|---------|
| 0 | `HEDDLE_OK` | success |
| 1 | `HEDDLE_E_ARG` | null / zero-length / misaligned argument |
| 2 | `HEDDLE_E_MAGIC` | magic != WHP2 |
| 3 | `HEDDLE_E_VERSION` | layout major != 2 |
| 4 | `HEDDLE_E_LAYOUT` | header size / layout rev / geometry deviation |
| 5 | `HEDDLE_E_ENDIAN` | endian canary mismatch (BE host or corruption) |
| 6 | `HEDDLE_E_CFG_CRC` | static config CRC-32/IEEE mismatch |
| 7 | `HEDDLE_E_REGION_SIZE` | buffer smaller than plane_size |
| 8 | `HEDDLE_E_CAPACITY` | capacity / sample / stride out of range |
| 9 | `HEDDLE_E_LANE_OVERFLOW` | lane >= lane_count (or remark bits beyond) |
| 10 | `HEDDLE_E_PAYLOAD` | len == 0 or len > sample_size |
| 11 | `HEDDLE_E_MODE` | op not valid for this plane's mode |
| 12 | `HEDDLE_E_STATE` | session / role / slot-state violation |
| 13 | `HEDDLE_E_SEQ_TORN` | two-store tear after BOUNDED retries |
| 14 | `HEDDLE_E_OVERRUN` | ring slot overwritten before consume |
| 15 | `HEDDLE_E_NOT_PUBLISHED` | ordinal not yet written |
| 16 | `HEDDLE_E_UNSUPPORTED` | declared v2 boundary refused |

## §11 Threading / ownership contract

* Single-producer plane: exactly ONE producer thread; consumers: any
  number (each with its own ctx).
* Multi-producer plane: ONE producer thread per lane (ownership
  enforced by the in-flight tripwire, §8); consumers as above.
* The dirty harvest / frame commit are the render thread's alone (the
  exchange is the frame barrier; Engineer 3's orchestrator serializes
  multi-renderer access).
* Producer contexts are single-threaded private state; sharing one
  producer ctx across threads is a contract violation (undefined).

## §12 Law compliance

* **Law 1 (zero heap):** the engine performs ZERO dynamic allocation —
  plane memory is caller-provided, the ctx is caller-owned stack state.
  Proven: mallinfo2/sbrk deltas == 0 across 100k cycles (H16), 2M-op
  torture steady-state (T4, dual-pass probe) and 1M bench cycles (B8).
* **Law 2 (bounded, deterministic):** every retry loop has a
  compile-time bound; explicit LE everywhere; static-asserted offsets;
  deterministic create (byte-identical planes across runs — golden gate).
* **Law 3 (byte-frozen kernel):** `core/c/weft.{c,h}` 0-diff vs base
  ref — a hard CI gate in the heddle shard.
* **Law 4 (honest boundaries):** the 17-code ladder above; torn reads
  are refused, never returned as data; all boundaries in §13 are
  declared, not discovered in production.

## §13 Declared boundaries (v2)

1. **Cross-lane snapshots in multi-producer mode** are per-lane
   consistent only (§8). Strict cross-lane consistency: use
   single-producer mode.
2. **Ring mode is drop-oldest.** Under sustained reader lag the
   consumer sees `E_OVERRUN` and resyncs to the new head; the producer
   is never blocked (by design — the anti-backpressure stance of the
   whole architecture).
3. **JS-side writes** ship via the WASM bridge (C session semantics);
   a pure-JS producer session API lands with the orchestrator plane.
4. **Emscripten build** of the bridge is provided as source + build
   line but not exercised in this sandbox (no `emcc`); the native
   build plus the JS golden interop carry verification.
5. **wasm32** requires the threads/atomics feature for `i64.atomic.*`.
6. **SAB alignment**: the plane must sit at a 64-byte-aligned offset
   of the SharedArrayBuffer (fresh SAB allocations satisfy this; the
   JS engine refuses misaligned offsets).
7. **Stats semantic**: raw-bit keys over the first 8 payload bytes;
   exotic sample types order by those bits per §5.
8. **Backpressure is in-band only**: marks + counters (§7.1); there is
   no producer-side blocking and no out-of-band ACK channel, ever.

## §14 Test & evidence index

| Suite | File | Regimes | Evidence |
|-------|------|---------|----------|
| H-series (unit, 145 checks) | `core/c/heddle/hotplane_unit_test.c` | plain / ASAN / TSAN | `litmus/evidence/heddle/hp-unit-*.log` |
| T-series (torture, 2M updates) | `core/c/heddle/torn_read_torture.c` | plain / ASAN / TSAN | `litmus/evidence/heddle/hp-torture-*.log` |
| W-series (bridge, 21 checks) | `core/wasm/heddle_bridge_test.c` | plain | `litmus/evidence/heddle/hp-bridge-o2.log` |
| J-series (JS interop, 37 checks) | `packages/heddle-hotplane/test/golden.test.mjs` | node | `litmus/evidence/heddle/js-golden-interop.log` |
| B-series (scoreboard) | `core/c/heddle/hotplane_bench.c` | API + inlined core | `litmus/evidence/heddle/hp-bench-*.log` |
| Golden determinism | `golden_dump.c` ×3 | byte-compare | `litmus/evidence/heddle/golden-determinism.log` |
| Shard (all gates) | `ci/scripts/run_heddle_shard.sh` | full matrix | `litmus/evidence/heddle/shard-full.log` |

Audit report & scoreboard: `D-report/D-41-HEDDLE2-CORE.md`.
