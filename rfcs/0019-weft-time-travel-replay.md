---
RFC: 0019
Title: weft_replay — Deterministic Time-Travel Replay & State Reconstruction
Status: Draft
Authors: Engineer 3 (Observability Fabric & Reactive Dataflow)
Created: 2026-09-20
Supersedes / Superseded-by: None
Requires: 0014 (.weftrec v4 event stream), 0009 G5 (cross-language trace parity pattern), 0002 (xorshift32 canonical generator)
---

# RFC 0019 — weft_replay: deterministic time-travel replay & state reconstruction

## Summary

A lock-free trace tells you WHAT the kernel decided; the black-box complaint
is that it cannot tell you what the kernel LOOKED LIKE when it decided it.
This RFC closes that gap with a **pure fold**: a deterministic state machine
that consumes a `.weftrec` v4 event stream and reconstructs, after every
event, the full shadow kernel state — `latest`, `epoch`, `w_work`, `r_work`,
the per-buffer `(seq, payload_len, version)` triple, and the telemetry
counters — reduced to a **64-bit state hash per step**.

Because the fold is a pure function of the event prefix:

- **Replay** = fold the stream. **Time travel** = the hash log IS the
  timeline; jumping to step *k* means restoring the nearest checkpoint and
  folding forward (bounded, declared cost).
- **Cross-runtime** = every port (C, Rust, TypeScript, Kotlin, Swift, Dart)
  implements the SAME fold over the SAME event stream and MUST produce
  byte-identical hash logs — the G5 parity pattern, applied to state.

## The modeling contract

The fold is a **shadow model** of the kernel, not the kernel. Three declared
abstractions make it deterministic without payload bytes:

1. **Null frame payload_len is modeled as 0.** The real kernel inits the
   null frame with `payload_len = payload_max` (04-LITMUS §0.6); the trace
   does not carry `payload_max`, so the fold models LOGICAL payload and the
   null frame contributes 0. All ports share the convention — parity is
   unaffected.
2. **A successful PUBLISH implies an unrevoked writer.** Post-revocation
   publishes surface in the stream as DROP events (the kernel ACKs and
   refuses); the first successful PUBLISH after a revocation window is the
   rebind. The fold clears `revoked` on PUBLISH.
3. **Claim validation is load-bearing.** CLAIM carries the claimed seq;
   the fold checks it against the shadow's own reconstruction. A mismatch
   is a fold error (`WEFT_REPLAY_DISAGREE`) — the trace and the model
   disagree, which is exactly the class of bug a replay debugger exists to
   surface. Never silently ignored.

## Fold specification (normative — every port implements EXACTLY this)

Initial state:

```
latest = 0; epoch = 0; w_work = 1; r_work = 2; revoked = 0
buf[i] = { seq: 0, len: 0, ver: 1 }  for i in 0..2
t_publish = t_claim = t_drop = t_invalid = t_wsteps = t_rsteps = 0
t_stall = t_tear = t_canary = 0; step = 0
```

Steps (kind → transition):

```
PUBLISH (1, aux=len, data=seq):
    buf[w_work] = { seq, len: aux, ver: 1 }
    old = latest; latest = w_work; w_work = old        // THE exchange
    revoked = 0                                        // modeling rule 2
    t_publish++; t_wsteps++

CLAIM (2, aux=0, data=seq):
    mine = latest; latest = r_work; r_work = mine      // THE exchange
    REQUIRE buf[mine].seq == data  else DISAGREE
    t_claim++; t_rsteps++

DROP (3, aux=epoch_at_ack, data=seq_refused):
    epoch = aux; t_drop++

REVOKE (4, aux=0, data=pre_revoke_epoch):
    revoked = 1

ACK (5, aux=0, data=epoch_after_ack):
    epoch = data

STALL (6, data=attempts):  t_stall++   (no other state)
TEAR (7, data=seq):        t_tear++
CANARY_FAIL (8, data=seq): t_canary++
```

State hash — FNV-1a 64 (offset `0xcbf29ce484222325`, prime
`0x100000001b3`) over the canonical little-endian byte serialization:

```
u32 latest; u32 epoch; u32 w_work; u32 r_work; u8 revoked;
for i in 0..2: u32 buf[i].seq; u32 buf[i].len; u16 buf[i].ver;
u64 t_publish; u64 t_claim; u64 t_drop; u64 t_invalid;
u64 t_wsteps; u64 t_rsteps;
u32 t_stall; u32 t_tear; u32 t_canary; u32 step;
```

Total: **111 bytes** (4·4 + 1 + 3·10 + 6·8 + 4·4).

## Time travel

- **Hash log**: `hashes[k]` = state hash after k+1 events. The log IS the
  debugger's timeline: equality of two hashes ⇒ (the model declares) equal
  states; divergence between two runtimes pinpoints the exact step.
- **Checkpoints**: a struct copy every `WEFT_REPLAY_CHECKPOINT = 64` steps
  into a caller-provided ring. `jump(k)`: restore checkpoint `k/64`, fold
  forward `k % 64` steps. Bounded: ≤ 64 folds per jump, O(1) memory per
  checkpoint. No allocation (Law 2).
- **Post-mortem**: a `.weftrec` captured in production replays identically
  to the moment it was cut — the event stream is clock-free and
  byte-identical across ports (RFC-0014), so the reconstructed state is
  portable evidence, not a local guess.

## Laws

- **Law 1**: the fold has no loops beyond the event list itself; jump is
  bounded by the checkpoint interval.
- **Law 2**: zero allocation — state and checkpoints are caller storage.
- **Law 3**: the kernel is untouched; the fold reads a trace file.
- **Law 4**: this RFC IS a determinism contract — the xlang-replay fixture
  byte-compares hash logs across all six runtimes.

## Test plan

`core/c/weft_replay_test.c` (R-series): fold semantics per kind (exchanges
verified against hand-computed states), claim-disagreement detection,
null-frame convention, hash stability vectors (pinned first-hash values),
checkpoint jump equivalence (jump(k) hash == fold(k) hash for all k in a
sweep), and a 100k-event soak whose final hash is pinned.
