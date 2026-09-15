# Security Policy

Weft is a memory-safety product, so its security policy is a working document, not a formality.

## Supported versions

| Version | Supported | Notes |
|---|---|---|
| v0.x (all pre-1.0) | Best-effort | Pre-alpha; security fixes land on main only. Do not ship pre-1.0 to production without your own review. |
| 1.x (future) | Latest minor + previous minor | Normal policy begins at 1.0. |

## Reporting a vulnerability

**Do not open a public issue for a security report.** Use GitHub's *Report a vulnerability*
(private security advisory) for this repository, or email **security@weft.dev** once the domain is
provisioned. Include: affected component (kernel / steward / heddle / tool), platform, a
reproducer (a failing litmus-style test is the ideal form), and your assessment of impact.

- Acknowledgment within **72 hours**.
- Fix or mitigation plan within **30 days** for anything that can corrupt memory or cross a
  trust boundary.
- Credit in the release notes unless you prefer otherwise; coordinated disclosure with a 90-day
  window by default.

## Threat model (what we protect, and explicitly what we do not)

**In scope — the guarantees the protocol exists to provide:**

- **No torn reads.** A claimed buffer is always a fully written frame (invariant I1, litmus L1).
- **No use-after-free.** Released buffers are never written, including by native writers holding
  raw pointers (invariant I6, litmus L7). This is the sharpest edge in the design: FFI writers
  cannot be panicked out of a dangling write, so revocation is a protocol contract, not a runtime
  check.
- **No priority inversion on real-time writers.** The wait-free publish path must never block an
  audio-rate thread (invariant I2/I5, litmus L2/L5).
- **Leak detection integrity.** The Steward's leak reports must not be suppressible by untrusted
  input (e.g., malformed envelope headers must not crash the detector).

**Out of scope — stated plainly (Law 4):**

- **Cross-process isolation.** A Weft is same-process shared memory. Any process, library, or
  plugin with read access to the buffer can read or corrupt the hot state. Weft does not defend
  against a hostile co-tenant of your address space; that is your process boundary's job.
- **Cross-origin isolation (Web).** `SharedArrayBuffer` mode assumes you have shipped COOP/COEP
  correctly. The default Transferable path exists partly because many sites cannot.
- **Network-source authenticity.** A Weft fed by network input trusts its writer. Unauthenticated
  tick injection is an accepted risk at v0.x; a `VerifiedWeft` variant (HMAC per frame, ~µs cost)
  is a tracked open question (ARCHITECTURE.md Q4).
- **Debug tooling in production.** `weft-record`, leak traces, and debug assertions are
  development-surface only; they are compiled out of release builds and must not be relied on as
  runtime security controls.

## Memory-safety self-checks

Debug builds continuously assert the ownership invariant (litmus L6 canaries). Release builds
perform no per-frame safety work — the safety is in the protocol's construction, not in runtime
checking. That division is deliberate and is the reason the litmus suite is the canonical core:
the project trusts proofs backed by adversarial tests more than it trusts overhead.

## Cryptography

None in the core, by design. If `VerifiedWeft` (Q4) lands, it will use platform-standard HMAC
primitives and its spec will carry its own security review section.
