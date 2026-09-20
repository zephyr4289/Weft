---
RFC: 0017
Title: weft_flow — Zero-Allocation Declarative Stream Operators
Status: Draft
Authors: Engineer 3 (Observability Fabric & Reactive Dataflow)
Created: 2026-09-20
Supersedes / Superseded-by: None
Requires: 0002 (Law 2 hot-path discipline), 0004 (ring views), 0008 (freshness semantics)
---

# RFC 0017 — weft_flow: zero-allocation declarative stream operators

## Summary

Every reactive framework gives developers `map`, `filter`, `window`, `zip` —
and every one of them allocates on the hot path: closures heap-captured,
intermediate arrays, subscription machinery, GC pressure exactly at frame
time. That is the deal Rx/Combine/Flow offers, and for a 120 Hz pipeline it
is a deal with the devil.

`weft_flow` keeps the declarative shape and refuses the deal. Operators
compose over **borrowed views** of ring payloads — no copies, no closures
with hidden captures, no intermediate allocation, no executor. Every buffer
is caller-provided at setup; the hot path is arithmetic and the developer's
own transform function. The result feels like reactive streams and runs
like a `for` loop, because under the operator sugar it IS a `for` loop.

## The view (the only type that flows)

```c
typedef struct {
    const uint8_t* ptr;   // BORROWED payload pointer — valid until the next
                          // publish on the source ring (A3 live-buffer rule)
    uint32_t       len;
    uint32_t       seq;
    uint64_t       t_ns;  // injected (determinism — never read a clock here)
} weft_flow_view;
```

Views are values. They are copied through the operator chain, never owned;
the lifetime discipline is the kernel's own (the reader must consume before
the writer laps the triad). A view is 24 bytes; passing one is a register
affair, not a malloc.

## Operators (normative shapes)

- **map** — `size_t (*fn)(ctx, src_view, dst_bytes, dst_cap)` writes the
  transformed payload into CALLER scratch and returns its length. An output
  that does not fit is refused WHOLE (the TIER4 refusal precedent: no
  partial frames, ever). Output view = `{dst, len, seq, t_ns}` — identity
  metadata carries through, so a mapped stream stays joinable by `zip`.
- **filter** — `int (*fn)(ctx, view)` → keep/drop. Pure predicate.
- **window** — tumbling or sliding, over a caller-provided view ring:
  tumbling emits `cap` views when full then resets; sliding emits the last
  `min(count, cap)` views every step. `flush()` drains a partial tumbling
  window (end-of-stream semantics without null sentinels).
- **demux** — routes each view to one of K ≤ 8 caller-provided sinks by a
  pure route function (`0..K-1`, `-1` = drop). A full sink is LOSSY with
  the overflow counted (`t_overflow` per sink) — Law 1: backpressure
  cannot become a stall, and the loss is visible, not silent.
- **zip** — pairs two streams arrival-wise with a declared timestamp
  tolerance: holds at most ONE unpaired view per side (latest-wins — a new
  view from one side coalesces the stale unpaired one, counted), emits a
  pair when both sides are present within tolerance, counts a `t_gap` when
  the tolerance fails. For full N-way timestamp alignment, use RFC-0018's
  `weft_sync` — zip is the two-stream sugar.
- **pipeline** — filter → map → window in one struct, one `step(view)`,
  per-stage telemetry. The composition the HUD sample and the demos use.

## Laws

- **Law 1**: window/demux/zip state is finite and declared; overflow is
  counted, never spun on.
- **Law 2**: the ONLY allocation in this RFC is a typo. All operator state
  and all outputs live in caller storage set up once at bind time.
- **Law 3**: pure sugar over views — no kernel contact, no policy.
- **Law 4**: fixtures drive the operators with deterministic callbacks
  (pat/xorshift — 04-LITMUS §0.1/0.2) and byte-compare output view streams
  across ports (`fixtures/xlang-flow/`).

## Port surface (the DSL story)

The operators above are the CORE. Each runtime wraps them in its native
fluent idiom — same core, same fixtures, zero allocation maintained:

| Port | Shape |
|---|---|
| TypeScript | `flow<T>().map(f).filter(p).window(3)` — builder over the same views |
| Kotlin | `weftFlow<View> { map { }; filter { }; window(3) }` — receiver DSL |
| Swift | `WeftFlow.builder().map(f).filter(p).window(3).build()` |
| Dart | `WeftFlowBuilder().map(f).filter(p).window(3).build()` |

The DSLs are compile-time sugar over the port's operator core; they add no
allocation on the hot path (the builder is a stack value; `build()` returns
the pipeline struct by value).

## Test plan

`core/c/weft_flow_test.c` (O-series): per-operator semantics
hand-verified; refusal-whole on map overflow; window flush; demux overflow
accounting; zip tolerance/coalescing; pipeline end-to-end determinism
(10k views, pinned output checksum); structural zero-alloc (all state
caller-declared — the test allocates NOTHING beyond its own frame).
