# The Litmus Suite

> **One protocol, many implementations, one conformance suite.**
> This directory is the project's canonical core. The kernel is re-implemented per language
> because the platform primitives share no abstraction — so what makes an implementation "Weft"
> is not its lineage. It is this suite.

---

## 1. What this is

A language-agnostic catalog of protocol scenarios, each with:

- a **deterministic thread script** (who does what, in what interleaving class),
- **adversarial parameters** (injected holds, rate ratios, stretched timings),
- a **mechanical verdict** (assertions on values, sequence numbers, step bounds, or canaries),
- and the **invariant it proves** ([rfcs/0001 §5](../rfcs/0001-triad-exchange-protocol.md)).

Per-language **runners** execute the catalog against each implementation. The runners are the only
platform-specific code here. A port that passes the full catalog on its target may truthfully call
itself Weft; a port that passes nine of ten tests has failed, and there is no partial credit.

## 2. Why the suite, not the code, is the core

Three reasons, in ascending importance:

1. **Portability.** A Swift expert contributes a port without reading Kotlin. The suite is the
   interface between language communities.
2. **Evolution.** When the protocol changes (`triad-2`), every implementation is re-validated by
   the same gate, and drift between language communities becomes mechanically impossible.
3. **Honesty.** The founding draft's protocol passed happy-path tests and was still formally
   unsound. Happy-path tests certify luck; litmus tests certify the property. The entire design
   philosophy of the project — claims with boundaries, zeros as contracts — is downstream of
   tests that can actually fail.

## 3. The catalog (L-series)

| Test | Proves | Scenario (condensed) | Verdict |
|---|---|---|---|
| **L1-tear** | I1 — no torn reads | Writer at 2× display rate; reader hold artificially stretched to 5 / 10 / 50 ms per claim; payload is a function of `seq` | Every claimed payload fully consistent with its envelope `seq`; zero partial frames |
| **L2-writer-steps** | I2 — wait-free writer | Instrumented publish under reader holds swept 0–100 ms | Publish step count ≤ hard bound (one relaxed load + buffer write + one swap), regardless of reader behavior |
| **L3-reader-steps** | I3 — wait-free reader | Instrumented claim under writer storms (4× rate) | Claim step count ≤ hard bound (one swap); claim never retries, never fails |
| **L4-freshness** | I4 — latest-wins | Writer at 4× reader rate | Claimed `seq` ≥ newest published `seq` at claim time, every frame |
| **L5-progress** | I5 — no back-pressure | Reader hold swept 0–100 ms, then reader suspended entirely | Writer publish throughput flat (±noise) in every configuration |
| **L6-ownership** | I1, I5 — exclusivity | Canary word per buffer; both parties run under randomized interleavings | A buffer is written only by its current owner; canary mutation by a non-owner fails the run; ownership permutation always valid across concurrent swaps |
| **L7-revocation** | I6 — no use-after-free | `release()` under a concurrently spinning native writer; freed pages poisoned | Zero writes to poisoned/freed pages; writer observes `DROPPED_REVOKED` within one publish |
| **L8-envelope** | Tier 0 stability | Envelope round-trip; headers carrying unknown trailing fields; version negotiation | Round-trip byte-identical; unknown fields ignored; `triad-1` and hypothetical `triad-2` coexist |

Runner-level requirements for all tests:

- **Debug and release builds both run the catalog.** Debug asserts the canaries; release asserts
  the timing bounds (debug instrumentation violates step-count bounds by construction).
- **L1's stretched holds are injected by the harness**, not by `Thread.sleep` folklore — a hold
  must suspend the reader *between* its swap and its read-completion, which is the actual
  adversarial window.
- Every test is **run for a minimum wall time** (30 s) and **repeated** (5×) — concurrency bugs
  are probabilistic, and single passes certify nothing.

## 4. The Phase 0 spike is L1–L8

The project's first milestone — before any release, before any API freeze — is the Android
reference implementation passing this catalog on a Pixel 7a against a fake 120 Hz writer:
torn reads = 0, steady-state alloc/frame = 0, writer step bound holds on release builds. That
spike is also the acceptance test for [rfcs/0001](../rfcs/0001-triad-exchange-protocol.md); the
RFC's `Status` flips to `Accepted` when the suite goes green.

**The spike must be adversarial.** The founding draft's protocol would have passed a naive spike
and remained formally broken. If the spike plan contains no stretched holds, no 2× writer rates,
and no canaries, it is not a spike of this suite.

## 5. Adding a test

Litmus additions follow the RFC-lite path: a PR with the scenario definition, the invariant it
proves (or the regression it pins), the adversarial parameters, and a demonstration that the
current implementation passes. A test that no implementation can fail is documentation; a test
that all implementations fail is an RFC against the protocol. Both are welcome; they are not the
same PR.

Historical note for future maintainers: L1 exists because the founding spec's two-variable
protocol had a relaxed-ordering window that happy-path benchmarks could not see. Keep the
adversarial parameters adversarial — the day the suite gets comfortable is the day it stops
protecting the project.
