# Advisories — Architecture review of the Sandbox-Constrained Roadmap v0.1

**Status: BINDING.** Each advisory is a decision already made. An executor implements as written; a steward may veto by RFC amendment before Step 2 begins. Rationale is included so the decision survives personnel changes.

The roadmap itself is **approved** — it correctly absorbs the Phase 0/1 review findings: the corrected single-exchange protocol replaces the withdrawn two-variable design (§5 of the founding spec), I6 ships from day one, and the adversarial hold sweep (5/10/50 ms) is exactly the injection set the protocol review demanded. The advisories below close the remaining gaps that would otherwise surface as harness bugs, false greens, or unfalsifiable verdicts.

---

## A1 — I6 reclaim handoff: free ONLY after writer acknowledgement  *(was implicit in roadmap §0a)*

The roadmap says "release sets revoked (Release) before free; free deferred until writer quiescence." That is a slogan, not a handshake. As written, a reclaim path can free (or poison) a buffer while the writer is one instruction past its revocation check — the exact use-after-free class I6 exists to kill, reintroduced by the harness itself.

**Decision — the I6 handshake is specified as four ordered steps** (normative text in `02-KERNEL.md` §6):

```
1. REVOKE   releaser:  revoked.store(true, Release)
2. ACK      writer:    on next publish, sees revoked (Relaxed load, checked FIRST,
                       before any payload/envelope write) → epoch.fetch_add(1, AcqRel)
                       → return DROPPED_REVOKED. Writer never touches buffer bytes again.
3. RECLAIM  releaser:  poll epoch (Acquire) until it advances past the pre-revoke value,
                       bounded by timeout → only now may pages be poisoned or freed.
4. DESTROY  releaser:  after poison-verify (L7) — never before ACK.
```

Every post-ack publish returns `DROPPED_REVOKED` — "within one publish" is therefore structural, not a timing measurement. Do not "verify" it with wall-clock races between threads.

## A2 — L4 freshness needs a measurement protocol, not a slogan  *(roadmap verdict string is unfalsifiable as written)*

"Claimed seq ≥ newest published at claim time" is not mechanically checkable: telemetry counters lag their `latest` swap (the increment happens after the exchange), so any naive comparison against "current count" produces false reds, and any padded comparison produces false greens.

**Decision — the freshness predicate is fixed as** `P0 ≤ S ≤ P1` with a bounded catch-up spin (normative text in `04-LITMUS.md`, test L4). `P0` is the publish-telemetry snapshot taken immediately **before** the claim's exchange, `P1` the snapshot after, `S` the claimed envelope seq. The final drain (writer quiescent → claim must return exactly the last published seq) is the sharp edge of the test.

## A3 — L1 verification must read the LIVE buffer, never a copy  *(false-green hazard)*

If the reader snapshots the claimed frame at claim time and verifies the snapshot after the hold, the test cannot tear **by construction** — it verifies the copy, not the protocol. This bug would produce a green suite that proves nothing.

**Decision:** the hold is injected *between* the claim's exchange and verification, and verification reads the live mapped buffer (in-place). Rust's public API therefore exposes `r_read_slice` (copies the *still-held* live buffer at verify time), never a claim-time snapshot. The same semantics in C (raw pointer) and TS (`Uint8Array` view). This is why the copy-on-read convenience of the retired Android v0.1 JNI surface is explicitly **not** carried into the kernels.

## A4 — Pacing: deadline schedules, not sleep-per-frame  *(false-red hazard)*

`sleep(1/hz)` per frame accumulates drift and makes writer-throughput tests (L5) fail on scheduler noise, especially with 50–100 ms reader holds stealing the CPU.

**Decision:** all paced loops use a monotonic deadline schedule: `next += period; sleep(max(0, next − now)); if (now − next > period) next = now;` — no burst catch-up. Tolerances are set per test (L5 flatness ratio ≤ 1.5) and **tolerances may only be changed by advisory**, not by an executor debugging a red run.

## A5 — Reproducible randomness: xorshift32 with shared seeds  *(new, cheap, high value)*

"Three independent implementations pass the same suite" is only strong if the runs are actually comparable. **Decision:** the PRNG is xorshift32 (Marsaglia 13/17/5), seeded from the catalog (default `0x00C0FFEE`), identical in all languages (spec in `04-LITMUS.md` §0). L6's delay schedules are therefore bit-identical across C/Rust/TS — a differential trace for free. JavaScript implements it with `>>>0` and `Math.imul` (exact u32 semantics, no doubles).

## A6 — Cross-language consistency is a *verdict*, not a vibe

Roadmap: "each test passes in all three languages, or the protocol is wrong." **Decision:** operationalized — if any cell is red while its counterparts are green, the default hypothesis is a kernel bug in the red language (check `06-PITFALLS` §2–4 first), and the second hypothesis is a spec ambiguity (check the catalog). Only after both are excluded may "platform artifact" be claimed, and it must be written into `REPORT.md` with the evidence. Silent divergence between kernels (different orderings, different pattern functions) is the failure mode A5 exists to catch.

## A7 — Roadmap errata (environment ground truth, verified in this sandbox)

The roadmap's toolchain table over-claims one cell and the executor would burn hours discovering it:

| Toolchain | Roadmap says | Verified reality | Directive |
|---|---|---|---|
| GCC 14.2.0 | YES | ✅ confirmed | none |
| Rust 1.98.1 via rustup | YES (installed) | ❌ **not installed**, `rustc`/`cargo` not on PATH | Step 0 must install: `curl -O https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init && ./rustup-init -y --profile minimal` (network to `static.rust-lang.org` verified reachable, HTTP 200) |
| Node 24.19.0 | YES | ✅ confirmed; **additionally verified**: native TS type-stripping runs `.ts` directly, `SharedArrayBuffer` and `Atomics.exchange` work on the main thread and across `worker_threads` | none — the TS kernel plan in roadmap §0e is viable as written |
| Python 3.12.14 | YES | ✅ confirmed | none |
| make / bash | YES | ✅ confirmed | none |

## A8 — Stretch gate: ThreadSanitizer column on the C suite

The runtime suite is adversarial but not exhaustive; a model checker is the formal follow-up. **Decision:** Phase 0 adds one supplementary column — the C kernel + runner rebuilt with `-fsanitize=thread` (GCC 14 supports it), running the same L1–L8 catalog. Expected: clean. If TSAN reports a race, that **blocks** sign-off even if the plain run is green — TSAN sees happens-before edges the timing tests can miss. Labeled `c11-tsan-x86_64-sandbox`. Rust loom model check (`L-loom`) is filed as follow-up, non-blocking.

## A9 — Number labeling extends to interpreters

Phase 1's Python harness compares A/B/C/D *inside CPython*. Under the GIL, implementation A (naive reactive) measures GIL contention as much as display architecture. **Decision:** the whitepaper labels those numbers `cpython-gil-x86_64-sandbox`, and the A-vs-B comparison is explicitly framed as allocation/GC behavior, not scheduling truth. Recorded now so Phase 1 inherits it silently.
