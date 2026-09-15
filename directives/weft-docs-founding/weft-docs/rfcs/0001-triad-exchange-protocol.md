---
RFC: 0001
Title: The Triad Protocol — ownership by atomic exchange
Status: Accepted (2026-09-11, per WO-P0A §11 + WO-P1 §1)
        Evidence triad: (1) loom exhaustive model clean — litmus/evidence/loom/loom.txt;
        (2) TSAN clean ×5 runs ×8 tests = 40 executions; (3) 24/24 litmus cells green
        (8 tests × 3 languages, v1.1 predicates). Phase 0.5 complete.
Authors: Zephyr (@zephyr4289)
Created: 2026-09-11
Supersedes: the two-variable candidate protocol in the founding spec §5 (withdrawn — see §3)
---

# RFC 0001 — The Triad Protocol

## Summary

Weft's writer–reader synchronization protocol: **three off-heap buffers, one shared atomic, and
ownership transfers by atomic exchange.** The writer never blocks; the reader never blocks; no
read is ever torn; the reader always has the freshest published frame; intermediate frames are
dropped by design. The protocol is wait-free on both sides by construction, and this RFC replaces
an earlier two-variable design that was formally unsound.

## Motivation

Display state needs now, not history (see [PHILOSOPHY.md §1](../docs/PHILOSOPHY.md)). A naive
single shared buffer tears: a 60 Hz writer writing 4 KB while the draw thread reads produces
half-old/half-new frames. A mutex fixes tearing but blocks — and an audio writer blocked on a
UI thread mid-draw is priority inversion and audible clicks. Double buffering fixes tearing
without locks but adds back-pressure: a slow reader stalls the writer.

Triple buffering removes both problems — *if* the handoff is designed so that no party can ever
observe a stale view of who owns which buffer. That is the entire content of this RFC.

## 2. Guide-level explanation

Think of the three buffers as three cards on a table, and one shared index (`latest`) as a single
pointer that both parties can atomically swap:

- The **writer** always owns exactly one card (its working card). To publish, it finishes writing
  its card, then atomically swaps its card with whatever the pointer indicates. The pointer now
  names the freshly published card; the writer walks away holding the previous published card as
  its next scratch space.
- The **reader** also owns exactly one card. To claim, it atomically swaps its card with the
  pointer. The pointer now names the reader's old card (recycled); the reader reads the returned
  card for as long as it likes — until its next claim, nobody else can touch it.
- Because every transfer is a single atomic read-modify-write on **one** variable, there is no
  pair of loads that can go stale relative to each other. Ownership is exclusive at every instant,
  by construction.

## 3. Why the previous design was withdrawn

The founding spec §5 computed the writer's target buffer from two independent relaxed loads:

```text
latest_now  = latest.load(Relaxed)
claimed_now = claimed.load(Relaxed)
next        = min({0,1,2} \ {latest_now, claimed_now})
```

The safety argument was: "the writer will not touch `buffers[idx]` because it is `latest` (so
excluded) and it is `claimed` (so excluded)." The first exclusion is sound — the writer is the
sole storer of `latest`, so its load is never stale. The second is not: the writer's load of
`claimed` carries no cross-thread visibility guarantee, so the writer may legally fail to observe
an in-progress claim.

Failure timeline (all steps individually legal under relaxed ordering):

1. `latest=1`, `claimed=-1`. Reader claims buffer 0 (CAS −1→0), begins a slow draw (a heavy frame
   holds the buffer for milliseconds).
2. Writer publish P1: `claimed` load misses the claim, excludes {1}, writes buffer 2, publishes
   `latest=2`. Still harmless.
3. Writer publish P2: loads `latest=2` (fresh), loads `claimed` — stale −1 again formally legal.
   Candidates = {0,1}, min = 0 — **the buffer the reader is still drawing.** Torn frame.

Real hardware's cache coherence shrinks this window to nanoseconds, so naive benchmarks pass —
until a thermal throttle, debugger attach, or scheduler hiccup stretches the reader's hold past
the writer's propagation delay. A protocol whose safety depends on propagation latency is not a
protocol; it is a timing bet. The lesson generalizes into project policy: **never derive
exclusivity from two loads when one atomic RMW will do.**

## 4. Reference-level specification

### 4.1 State

```text
Shared:
  latest   : AtomicInt                     // the ONLY shared mutable index

Writer-private:
  w_work   : Int                           // index the writer will fill next

Reader-private:
  r_work   : Int                           // index the reader holds / will hand back
```

Initialization: the Steward allocates three off-heap buffers, zero-fills the envelope
(`seq = 0`), and sets `latest = 2`, `w_work = 0`, `r_work = 1`. Any permutation of
{0,1,2} across the three slots is valid.

Per-writer control (the I6 contract, §6): each writer also holds a `token`, whose
`revoked : AtomicBool` is checked once per publish.

### 4.2 Writer protocol (wait-free)

```text
publish(token, data) -> PUBLISHED | DROPPED_REVOKED:
  1. if token.revoked.load(Relaxed):  return DROPPED_REVOKED     // one relaxed load
  2. buffers[w_work].write(data)                                 // private, unsynchronized
                                                                 // + envelope.seq += 1
  3. old = latest.swap(w_work, AcqRelease)                       // publish + acquire old
  4. w_work = old                                                // recycled via latest
  return PUBLISHED
```

Bounded work: one relaxed load, one buffer write, one `swap`, one integer assignment. No spin, no
allocation, no syscall. An audio-thread (SCHED_FIFO) writer can run this with zero risk of
priority inversion.

### 4.3 Reader protocol (wait-free)

```text
claim():
  1. mine   = latest.swap(r_work, AcqRelease)
     // returns the newest published index; the reader's PREVIOUS buffer (r_work)
     // is now named by `latest` — handed back and recyclable by the writer
  2. r_work = mine                                   // the reader now owns `mine`
  3. return BufferRef(buffers[mine], seq_of(mine))   // read in place; exclusive
                                                     // until the next claim()
```

Readers may hold the returned buffer until their next `claim()` — typically one Draw-phase pass.
Reading in place is safe for exactly that long, guaranteed by ownership (§5), not by timestamps.
A reader that misses several publishes still claims successfully — it simply receives the newest
frame. There is no error case; `claim()` cannot fail.

### 4.4 Worked trace

Init: `latest=2`, `w_work=0`, `r_work=1`. Every buffer is owned by exactly one of
{**writer**, **reader**, **available** (named by `latest`)} at every instant.

| Step | Action | `latest` | `w_work` | `r_work` | buf 0 | buf 1 | buf 2 |
|---|---|---|---|---|---|---|---|
| init | — | 2 | 0 | 1 | writer | reader | available |
| W publishes f1 | write 0; swap → old=2 | 0 | 2 | 1 | available | reader | writer |
| W publishes f2 | write 2; swap → old=0 | 2 | 0 | 1 | writer | reader | available |
| R claims | swap → mine=2 | 0 | 0 | 2 | available | writer | reader |
| W publishes f3 | write 0; swap → old=1 | 0 | 1 | 2 | available | writer | reader |
| W publishes f4 | write 1; swap → old=0 | 1 | 0 | 2 | writer | available | reader |
| R claims | swap → mine=1 | 0 | 0 | 1 | available | reader | writer |

Note step "W publishes f3": the writer's new scratch (`w_work=1`) is the buffer the *reader just
retired* — acquired through the exchange chain, never taken directly. This is the mechanism doing
the heavy lifting, so it deserves its normative statement:

- **Ownership is exclusive at every instant.** The only operations that change ownership are the
  two swaps, and an atomic exchange on a single location serializes every concurrent attempt:
  first swapper wins, second swapper operates on the result. No buffer is ever owned by two
  parties, even transiently.
- **`latest` never names a buffer mid-write or mid-read.** The writer writes `w_work` *before*
  swapping it into `latest`; the reader reads `r_work` *after* swapping it out of `latest`.
  During any write or read, `latest` names a third buffer.
- **The writer can never reach the reader's current hold.** The reader's held buffer is not named
  by `latest`; it re-enters circulation only when the reader itself swaps it away. The writer
  therefore cycles the two non-held buffers — visibly in the trace, where the writer alternates
  between 0 and 2 until the reader swaps and frees the other side.
- **Both parties swapping "simultaneously" is defined.** The RMWs serialize on the one location;
  every serialization order yields a valid ownership permutation. This is the core advantage over
  the withdrawn two-variable design, where two loads could observe different epochs of two
  different variables.

Implementations must assert the ownership invariant continuously in debug builds (litmus L6).

### 4.5 Envelope and sequence numbers

Each publish increments the buffer's envelope `seq` (u32, wrapping) *before* the swap. Readers use
`seq` for freshness telemetry and for tear detection in litmus tests — a reader observing a
half-updated payload will observe a `seq`/payload mismatch under litmus instrumentation.

## 5. Invariants

| # | Invariant | Mechanism |
|---|---|---|
| I1 | **No torn reads.** A claimed buffer is fully written. | Payload written before the swap (AcqRelease orders it); reader reaches a buffer only via a swap that transferred exclusive ownership. |
| I2 | **Wait-free writer.** Bounded O(1) steps, no spin, no block. | §4.2 is a fixed instruction sequence; `latest.swap` is a single RMW. |
| I3 | **Wait-free reader.** Bounded O(1), no retry loop, cannot fail. | §4.3 is a single swap; there is no CAS-retry anywhere. |
| I4 | **Latest-wins.** Every successful claim returns the newest published frame at claim time. | `latest` names the freshest publish by construction; claims take `latest`. |
| I5 | **No back-pressure.** Writer progress is independent of reader hold time. | Writer needs only its private `w_work`, refreshed by its own swap — the reader's hold duration never enters the writer's path. |
| I6 | **No use-after-free across FFI.** A released Weft is never written. | `token.revoked` set (Release) *before* free; writer checks per publish (one relaxed load); free deferred until writer quiescence (§6). |

## 6. Writer revocation and the release sequence (I6)

Native writers hold raw pointers; no runtime can panic them out of a use-after-free. The contract:

1. `Steward.attachWriter` returns a `token` (control block: `revoked: AtomicBool`, `epoch: u32`).
2. `Steward.release(weft)`:
   a. `token.revoked.store(true, Release)` — happens **before** any deallocation;
   b. marks the Weft `RELEASED`; buffers become invisible to new readers;
   c. defers actual free until **writer quiescence**: either `detachWriter` is called, or the
      Steward's reaper observes `token` unclaimed across one full `FrameClock` tick after
      revocation (the writer checks `revoked` at the top of every publish, so a writer running at
      any rate goes quiescent within one tick).
3. A revoked publish is a no-op returning `DROPPED_REVOKED`. Writers are expected to treat that as
   "stop" — the Steward logs it in debug.

Worst-case in-flight writes at revocation: one. The reaper's one-tick wait covers it. This is the
entire cost of I6: one relaxed load per publish and a deferred free.

## 7. Edge cases

- **First frame.** Before the writer's first publish, the reader's first `claim()` returns the
  zero-filled initial buffer (`seq=0`). Readers render zeros; no special casing.
- **Reader faster than writer.** The reader re-claims the same published buffer back-to-back
  (its claim swaps `latest` out and its old buffer back in). Draw code may re-render or skip;
  default is re-render (the founding spec's choice — motion continuity over GPU cycles).
- **Writer faster than reader.** Intermediate frames vanish. This is the semantics of display,
  not a failure mode. `seq` gaps are observable for telemetry.
- **Multi-reader.** Out of scope for triad-1's kernel: each additional reader runs its own
  claim chain against its own Steward-registered cursor, or (v2) a fan-out Heddle snapshots once
  per VSYNC. Racing single-reader claims on a shared Weft is *not* supported by this protocol and
  must be rejected at the Heddle API layer.
- **Reader holds forever.** Permitted. The writer cycles its other two buffers indefinitely
  (publish → swap → old becomes scratch). No progress coupling exists (I5).

## 8. Litmus test mapping

Every invariant is proven by a named test in [`litmus/`](../litmus/) — conformance means passing
the suite:

| Test | Proves | Adversarial condition |
|---|---|---|
| L1-tear | I1 | Reader hold stretched to 5/10/50 ms; writer at 2× display rate; `seq`/payload continuity asserted |
| L2-writer-steps | I2 | Instruction-count instrumentation; hard bound asserted |
| L3-reader-steps | I3 | Same, reader side |
| L4-freshness | I4 | Writer at 4× reader; claimed `seq` must be ≥ newest at claim time |
| L5-progress | I5 | Reader hold swept 0–100 ms; writer throughput must be flat |
| L6-ownership | I1, I5 | Canary word per buffer; any canary mutation by a non-owner fails |
| L7-revocation | I6 | Release under concurrent native writer; canary probes freed pages |
| L8-envelope | Tier 0 | Header round-trip across versions; unknown-field tolerance |

## 9. Alternatives considered

- **Two-variable candidate protocol (founding spec §5).** Withdrawn — unsound under relaxed
  ordering (§3).
- **Seqlock (writer bumps a counter, reader retries).** Readers may retry unboundedly — violates
  I3 and makes draw-time budgets impossible to state.
- **Mutex / rwlock.** Priority inversion on real-time writers; violates I2/I5 outright.
- **Double buffering + back-pressure.** Correct but the writer stalls behind a slow reader —
  unacceptable for audio-rate producers.
- **Single-atomic exchange (this RFC).** Wait-free both sides, tearing impossible by ownership,
  one word of shared state. The graphics industry has shipped this pattern for decades; Weft's
  contribution is specifying it — envelope, lifecycle, revocation, and conformance suite — for
  UI state.

## 10. Open questions

- Multi-reader fan-out snapshot policy (ARCHITECTURE.md Q2) — per-reader buffers cost an
  allocation; shared snapshot + epochs costs complexity.
- `epoch` semantics for re-attach after `detachWriter` on the same Weft (needed for writer
  restart patterns in audio engines).
- Whether `claim()` should expose `seq` staleness (`framesBehind`) to draw code for adaptive
  detail-level — telemetry first, API later.

## 11. Implementation plan

Phase 0 (weeks 1–2, pre-v0.1): Kotlin reference implementation (~500 lines, zero dependencies) +
litmus runners for L1–L8, run against a fake 120 Hz writer on a Pixel 7a. Success criteria: L1
tears = 0 under adversarial holds; steady-state alloc/frame = 0; writer instruction bound holds on
release builds. This spike is the acceptance test for this RFC; `Status: Accepted` follows it.

## 12. Loom model assertions (v2, WO-P3 T1+T2)

The loom model (v2, strengthened per WO-P1-CLOSURE §2 and WO-P3 T1) verifies
four assertions across all interleavings (3 publishes × 3 claims × 3 buffers):

| Assertion | Property | Loom check |
|---|---|---|
| (a) Ownership | Each buffer has ≤1 owner at every point | Implicit in the Mutex-serialized exchange |
| (b) No torn observation | seq + payload + canary mutually consistent | `verify_frame()` per claim |
| (c) No future | `claimed_seq ≤ published_wm` at claim time | `published_wm` fused into the same Mutex step as the writer's release — no interleaving point between "release lands" and "watermark advances" |
| (d) Join-quiescence | At join: `published_wm == MAX_PUBLISHED_SEQ` | Safety fragment — liveness (eventual-final) stays owned by litmus L4 (v1.1 drain semantics) |

Assertion (d) was renamed from "eventual-final" to "join-quiescence" per
WO-P3 T2 (WO-P1-CLOSURE §3). Loom is a safety model; it cannot prove liveness.
The reader-runs-first interleaving (reader drains only the null frame, writer
publishes everything after) is a legal schedule, and the property that matters
there is safety, not liveness. Eventual-final liveness remains owned by litmus
L4 (v1.1: post-join drain ≤4 claims @1ms, `drain_exact = (last == frames)`).

Countersigned (staff adjudication): ACCEPTED — evidence triad verified
  loom: exhaustive, assertions a-d hold (v2 strengthened run per WO-P3 T1 attaches)
  tsan: 5x8 clean | litmus: 24/24 | adjudication: WO-P1-CLOSURE §4
