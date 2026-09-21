# HPL1 — Weft Hot-Plane Layout, Version 1 (Normative)

> Status: **Normative** for Pillar 4 (`heddle-2.0`). Freeze class: **byte-frozen** like
> WCN1/WTR1 — any change is a protocol version bump, never an in-place edit.
>
> HPL1 is the managed-side **integration seam** for the Unified Hot-Plane described in
> `docs/pillars/04-HEDDLE2-UNIFIED-HOT-PLANE-UI.md`. Engineer 1's native SharedArrayBuffer
> bridge attaches through the adapter descriptor (`attachPlane`) without HPL1 itself
> changing. All offsets are **explicit little-endian** (Law 2). 64-bit quantities are
> stored as lo/hi u32 pairs; `lo` is stored LAST by writers (lo-last publish ordering)
> so 32-bit readers can use `lo` as the freshness gate without torn u64 reads.

---

## 1. Plane Geometry

An HPL1 plane is ONE contiguous `SharedArrayBuffer` (or `ArrayBuffer`/`mmap` region
natively) partitioned into four regions, in order:

```
┌──────────────────────────────┐  offset 0
│ HEADER (128 bytes, pinned)   │
├──────────────────────────────┤  128
│ DIRTY MASK (u32 × dirtyWords)│
├──────────────────────────────┤  laneCtrlBase (8-aligned)
│ LANE CONTROL BLOCKS (64B ea) │  laneCount blocks
├──────────────────────────────┤  ringBase (8-aligned)
│ SAMPLE RINGS (f64, per lane) │  laneCount × samplesPerLane × 8 bytes
└──────────────────────────────┘  totalBytes
```

Derived offsets (all computed by `packages/heddle-core/src/layout.js` and mirrored
byte-exactly in `packages/flutter-heddle` and `packages/swift-heddle`):

| Symbol         | Value                                            |
|----------------|--------------------------------------------------|
| `HEADER_SIZE`  | `128`                                            |
| `dirtyWords`   | `ceil(laneCount / 32)`                           |
| `laneCtrlBase` | `128 + align8(4 * dirtyWords)`                   |
| `laneCtrl(i)`  | `laneCtrlBase + i * 64`                          |
| `ringBase`     | `align8(laneCtrlBase + 64 * laneCount)`          |
| `laneRing(i)`  | `ringBase + i * samplesPerLane * 8`              |
| `totalBytes`   | `ringBase + laneCount * samplesPerLane * 8`      |

`align8(x) = (x + 7) & ~7`. `samplesPerLane` MUST be a power of two ≥ 2 (mask-based
wraparound: `index & ringMask`, `ringMask = samplesPerLane - 1`). Validators MUST
reject non-power-of-two capacities with `HPL1_CAPACITY_MISMATCH`.

## 2. Header (offset 0, 128 bytes, all little-endian)

| Offset | Size | Type    | Field            | Notes                                            |
|-------:|-----:|---------|------------------|--------------------------------------------------|
| `0x00` | 4    | u32     | `magic`          | bytes `'H','P','L','1'` (LE u32 `0x314C5048`)    |
| `0x04` | 4    | u32     | `version`        | `1`                                              |
| `0x08` | 4    | u32     | `flags`          | bit0 `LE_REQUIRED` (must be 1); bit1 `EPOCH_STABLE` |
| `0x0C` | 4    | u32     | `laneCount`      | ≥ 1, ≤ 4096                                      |
| `0x10` | 4    | u32     | `samplesPerLane` | power of two, ≥ 2                                |
| `0x14` | 4    | u32     | `ringMask`       | `samplesPerLane - 1`                             |
| `0x18` | 4    | u32     | `tickHz`         | nominal producer tick rate (0 = unspecified)     |
| `0x1C` | 4    | u32     | `reserved0`      | zero                                             |
| `0x20` | 8    | u64     | `epoch`          | producer session id; bumped on every (re)start   |
| `0x28` | 8    | u64     | `publishSeq`     | header seqlock (odd = header write in progress)  |
| `0x30` | 8    | u64     | `lastPublishNs`  | monotonic nanoseconds of last publish            |
| `0x38` | 8    | u64     | `framesDropped`  | producer-side drop accounting                    |
| `0x40` | 8    | f64     | `globalMin`      | all-lane session window minimum                  |
| `0x48` | 8    | f64     | `globalMax`      | all-lane session window maximum                  |
| `0x50` | 8    | f64     | `globalAvg`      | all-lane running average                         |
| `0x58` | 8    | f64     | `globalCurrent`  | headline value (lane 0 mirror)                   |
| `0x60` | 4    | u32     | `dirtyWords`     | must equal `ceil(laneCount / 32)`                |
| `0x64` | 4    | u32     | `laneCtrlStride` | pinned `64`                                      |
| `0x68` | 4    | u32     | `ringBaseOffset` | must equal derived `ringBase`                    |
| `0x6C` | 4    | u32     | `totalBytes`     | must equal derived `totalBytes` and buffer size  |
| `0x70` | 16   | —       | `reserved1`      | zero                                             |

The four self-describing fields (`dirtyWords`, `laneCtrlStride`, `ringBaseOffset`,
`totalBytes`) are redundancy checks: a validator recomputes them from `laneCount` and
`samplesPerLane` and fails closed (`HPL1_CAPACITY_MISMATCH`) on any disagreement.

## 3. Dirty Mask (offset 128)

One bit per lane, `dirtyWords × 32` bits. **Producer-set only** — producers OR-in the
bit of every lane touched during a publish batch, then clear all bits at the START of
the next batch (set → publish → hold). Consumers NEVER write the mask: multi-consumer
clear semantics are undefined by design; the mask is an advisory broadcast, and the
authoritative freshness gate is always the per-lane `laneSeq` (§4). Consumers count
observed bit transitions per frame for HUD "dirty-mask activity".

## 4. Lane Control Block (64 bytes, stride 64, all little-endian)

| Offset | Size | Type | Field            | Notes                                        |
|-------:|-----:|------|------------------|----------------------------------------------|
| `0x00` | 8    | u64  | `laneSeq`        | per-lane seqlock (odd = lane write active)   |
| `0x08` | 8    | f64  | `current`        | newest value                                 |
| `0x10` | 8    | f64  | `min`            | session window minimum                       |
| `0x18` | 8    | f64  | `max`            | session window maximum                       |
| `0x20` | 8    | f64  | `avg`            | running average                              |
| `0x28` | 8    | u64  | `samplesSeen`    | monotonic sample counter for this lane       |
| `0x30` | 4    | u32  | `head`           | next ring write index (pre-mask)             |
| `0x34` | 4    | u32  | `laneFlags`      | bit0 `ACTIVE`; bit1 `MANUAL` (user-driven)   |
| `0x38` | 8    | u64  | `lanePublishNs`  | monotonic ns of last lane publish            |
| `0x40` | 4    | u32  | `laneDrops`      | drops on this lane                           |
| `0x44` | 4    | u32  | `pad0`           | zero                                         |
| `0x48` | 8    | f64  | `spare0`         | forward compatibility, zero                  |
| `0x50` | 8    | f64  | `spare1`         | forward compatibility, zero                  |
| `0x58` | 8    | u64  | `pad1`           | zero                                         |

### 4.1 Producer publish protocol (single writer per lane)

```
seq ← laneSeq (read own bookkeeping)
store laneSeq ← seq + 1            // odd: write in progress
write current, min, max, avg
write sample into ring[head & ringMask]
store head ← (head + 1)
store samplesSeen ← samplesSeen + n; lanePublishNs ← now; laneDrops as needed
store laneSeq ← seq + 2            // even: stable (release point)
```

Producers MUST reject `NaN`/`±Infinity` samples with `HPL1_INVALID_SAMPLE` rather than
publishing them. Statistics (min/max/avg) are maintained **in place on the plane** by
the producer — UI readers never allocate accumulator objects (Law 1).

### 4.2 Consumer acquire protocol (any count of readers)

```
s1 ← laneSeq.lo (SeqCst)          // lo stored LAST by writer = freshness gate
if (s1 & 1) → torn (retry, count tear)
read f64/u32 fields (plain loads)
s2 ← laneSeq.lo (SeqCst)
if (s1 ≠ s2) → torn (retry, count tear)
```

`laneSeq.hi` extends the sequence beyond 2³²: writers store `hi` first, then `lo` last
(lo-last ordering), so a reader that passes the `lo` gate sees a consistent `hi`.
A bounded retry (default 64 attempts) escalates to `HPL1_TORN_SEQLOCK` — readers count
tears and surface them to the HUD; a tear is NEVER silently swallowed.

### 4.3 Header seqlock

`publishSeq` guards the header globals (`globalMin/Max/Avg/Current`, `lastPublishNs`,
`framesDropped`) with the same odd/even discipline. Producers bump `epoch` BEFORE
taking the header lock; consumers that observe an `epoch` change must reset all
consumer-side snapshots and raise `HPL1_EPOCH_CHANGED` (Law 4: restarts are explicit).

## 5. Wire-Deterministic Fixtures

`fixtures/heddle2/generate.mjs` emits byte-identical planes on every run (seeded PRNG,
dyadic constants only). Committed golden fixtures:

| Fixture                  | Geometry      | Purpose                                        |
|--------------------------|---------------|------------------------------------------------|
| `hpl1-basic-4x8.bin`     | 4 lanes × 8   | happy-path parity across TS/Dart/Swift         |
| `hpl1-edge-1x2.bin`      | 1 lane × 2    | minimum geometry, ring wrap on second sample   |

Each fixture has a `.json` manifest (field-by-field expected values + sha256). The
generator's double-run must be byte-identical (CI stage 2). Consumers of a fixture:
TS `test/fixtures.test.mjs` (full parse), Dart/Swift structural audits (constants +
sha256).

## 6. Error Taxonomy (Law 4 — honest boundaries)

| Code | Name                     | Meaning                                   | Severity        |
|-----:|--------------------------|-------------------------------------------|-----------------|
| 0    | `HPL1_OK`                | no error                                  | —               |
| 1    | `HPL1_BAD_MAGIC`         | magic ≠ `HPL1`                            | fatal, fail-closed |
| 2    | `HPL1_BAD_VERSION`       | version ≠ 1                               | fatal, fail-closed |
| 3    | `HPL1_NOT_LITTLE_ENDIAN` | `flags.LE_REQUIRED` clear                 | fatal, fail-closed |
| 4    | `HPL1_CAPACITY_MISMATCH` | derived vs stored geometry disagreement   | fatal, fail-closed |
| 5    | `HPL1_LANE_OUT_OF_RANGE` | laneIndex ≥ laneCount                     | caller error    |
| 6    | `HPL1_TORN_SEQLOCK`      | retries exhausted; value NOT returned     | retryable       |
| 7    | `HPL1_EPOCH_CHANGED`     | producer restarted; snapshots reset       | informational   |
| 8    | `HPL1_PLANE_DETACHED`    | buffer gone / zero-length / worker died   | fatal for view  |
| 9    | `HPL1_CONTEXT_LOST`      | GPU context lost; fallback view shown     | recoverable     |
| 10   | `HPL1_TAB_HIDDEN`        | Page Visibility hidden; scheduler paused  | informational   |
| 11   | `HPL1_WORKER_CRASH`      | producer worker terminated                | recoverable     |
| 12   | `HPL1_BAD_RENDER_ENGINE` | engine missing required interface         | caller error    |
| 13   | `HPL1_INVALID_SAMPLE`    | NaN/±Infinity rejected at produce time    | caller error    |
| 14   | `HPL1_RING_UNDERRUN`     | reading older than ring retains           | recoverable     |

## 7. Laws Map

| Law | Enforcement in HPL1                                                        |
|-----|-----------------------------------------------------------------------------|
| 1   | Zero allocation on hot paths: seqlock acquire reads preallocated views; stats live on the plane; consumers pass preallocated `out` structs; no BigInt (u64 = lo/hi u32) |
| 2   | Every multi-byte access is explicit little-endian; lo-last publish ordering; LE_REQUIRED flag fail-closed |
| 3   | HPL1 is managed-side only; `core/c/weft.{c,h}` untouched; CI stage 1 diffs the kernel |
| 4   | §6 taxonomy: tears, epoch changes, detach, context loss, tab hidden, worker crash are explicit coded events, never silent |

## 8. Integration Seam (Engineer 1's bridge)

Engineer 1's native SAB bridge (and the WT-series `weft_tensor_ring_t` when it is the
backing memory) attaches via one descriptor:

```js
attachPlane(buffer, {
  layoutVersion: 1,          // HPL1
  byteOffset: 0,             // plane may live inside a larger region
  byteLength: totalBytes,    // optional; default = derived from header
})
```

If Engineer 1's final offsets differ from HPL1 §2–§4, the adapter maps their
descriptor onto HPL1 semantics in `attachPlane` — HPL1 itself stays byte-frozen.
