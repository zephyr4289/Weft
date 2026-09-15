# @weft/core

High-frequency, zero-copy state-synchronization kernel for the Triad Protocol —
the TypeScript port of the canonical C kernel (`core/c/weft.{h,c}`).

One writer, one reader, three off-heap buffers, one atomic. The writer is
wait-free; the reader is wait-free; old frames are garbage, not debt
("display state is a river, not a ledger" — see the project
[Philosophy](../../docs/PHILOSOPHY.md)).

- **License**: Apache-2.0
- **Substrate**: `SharedArrayBuffer` + `Atomics` (Node >= 18, all modern
  browsers; cross-origin isolation required on the web — COOP/COEP)
- **Conformance**: the litmus suite (L1–L8) is the canonical gate; this port
  passes it (`make litmus` / CI `litmus-ts` shard)

## Quick start

```ts
import { Weft, PubResult } from '@weft/core';

const FLOATS = 1024;                  // payload budget in floats
const w = new Weft(FLOATS * 4);       // payload_max in BYTES

// --- writer thread (or Worker) ---
const cursor = w.wBeginFloat32();     // typed live write cursor (zero-copy)
cursor[0] = performance.now() / 1000; // write up to floor(payload_max/4) floats
cursor[1] = Math.sin(cursor[0] * 10);
w.publish(seq++, FLOATS * 4);         // wait-free O(1); never blocks

// --- reader (Draw phase only — requestAnimationFrame / vsync callback) ---
w.claim();                            // swap in the freshest frame; never fails
const view = w.rReadSlice(16, FLOATS * 4);  // live view, not a copy
draw(view);
```

Bulk copy variant (mirrors C's `weft_w_write_payload`):

```ts
const src = new Uint8Array(4096);
if (w.wWritePayload(src) === 0) w.publish(seq++, src.length);
```

## API surface (triad-1)

### Writer (`w_*`)

| Method | Contract |
|---|---|
| `wBegin(): Uint8Array` | Live write cursor over the working payload region. View is cached per buffer slot — zero allocation per call. Valid until the next `publish()`. Parity: `weft_w_begin`. |
| `wBeginFloat32(): Float32Array` | Typed cursor over the same region (`floor(payload_max/4)` elements). Zero allocation per call. |
| `wWritePayload(src): 0 \| -1` | Bulk-copy `src` into the working buffer at payload offset. `-1` (nothing written) if `src.length > payload_max`. Parity: `weft_w_write_payload`. |
| `publish(seq, payloadLen): PubResult` | Wait-free O(1) publish. `Ok`, or `DroppedRevoked` after `revoke()`. The single atomic exchange — see RFC 0001. |
| `fillPayload(seq, payloadLen)` | Litmus test-pattern filler (04-LITMUS §0.1). Production writers use the cursor API. |
| `revoke()` / `reclaim(preRevokedEpoch, timeoutMs)` | I6 writer-revocation handshake. Call `revoke`, wait for the epoch ACK, then tear down. |

### Reader (`r_*`) — Draw phase only

| Method | Contract |
|---|---|
| `claim(): number` | Swap in the freshest frame. NEVER fails; before any publish it yields the null frame (`seq=0`). |
| `rSeq()` / `rMagic()` / `rPayloadLen()` / `rHeaderSize()` | Envelope fields of the held buffer, read live. |
| `rReadSlice(offset, len): Uint8Array` | Live view (not a copy) into the held buffer. Valid until the next `claim()`. |

### Inspection (advisory — AXIOM T)

| Method | Contract |
|---|---|
| `debugView(): WeftDebugView` | Advisory snapshot of kernel state: slot owners, envelope samples, telemetry counters, `midPublishSample` flag. Cold path — never call per frame. Parity: `weft_debug_view`. |
| `tPublish()` / `tClaim()` / `tDrop()` / `epoch()` | Telemetry counters (bigint). Advisory, never a correctness reference. |

## Laws you inherit by using this

1. **The reader is always right; the writer is never blocked** — no API here
   waits, locks, or applies back-pressure.
2. **Zero is a contract** — zero allocations and zero locks on the hot path.
   The cursor views are cached per slot at construction; `publish`/`claim` are
   pure atomic exchanges.
3. **Mechanism, not policy** — the kernel has no opinions about what you draw,
   which framework you use, or how you thread. Heddles
   (`@weft/react`, `@weft/vue`, `@weft/svelte`) provide the Draw-phase bindings.
4. **Honesty** — no performance claims without a labeled environment and a
   reproducible script. See `bench/` before quoting numbers.

## Boundaries (stated, not implied)

- Single writer, single reader per Weft. Multi-consumer fan-out is an
  RFC-0004 driver-layer pattern, not kernel surface.
- `SharedArrayBuffer` requires cross-origin isolation (COOP/COEP headers) on
  the web. In Node, it works everywhere.
- This package is the kernel only. Lifecycle (the Steward), framework
  bindings (Heddles), and recording live elsewhere in the monorepo.
