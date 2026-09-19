# Security Policy & Threat Model

Weft is a memory-safe, real-time shared-memory message exchange substrate. Its security model is defined by construction invariants, the Tier-4 validation wall, and explicit threat boundaries.

---

## Threat Model

### In Scope (Protected & Proven)
- **Untrusted API inputs**: `payload_max`, `payload_len`, and NULL pointers at external API boundaries are rejected fail-closed (RFC 0015).
- **Malformed session / trace headers**: Handled with strict version-gated rejection (`.weftrec` v4, RFC 0014).
- **Tampered record streams**: Keyed cryptographic HMAC verification at driver boundary (`VerifiedWeft`, RFC 0005).
- **Zero torn reads / No Use-After-Free**: Invariants I1 & I6 verified through litmus suites L1–L8 and exhaustive TLA+ formal proofs.
- **Worker Crash Containment**: FFI boundary isolation with bounded timeouts and fork-mode process containment (`ffi_host`).

### Out of Scope (Documented Boundaries)
- **External heap corruption**: Rogue code executing within the same address space mutating kernel atomics (`latest`, `w_work`, `r_work`) directly is out-of-scope; the kernel cannot defend against an arbitrary code execution compromise in the host address space.
- **Caller violating protocol contracts**: Violating single-writer or single-reader ownership without the Fan-out coordinator is a caller contract violation.
- **Big-Endian architectures**: All wire formats, envelopes, and canaries assume Little-Endian architectures (ARM64, x86_64, RISC-V). Big-endian hosts are explicitly unsupported without a dedicated Tier-0 RFC.
- **Denial-of-Service via publication flood**: Governed by rate regulation and drop-telemetry accounting (Law 4: counted and backpressured, not magically unbounded).

---

## Architectural Enforcement

- **Tier-4 Validation Wall**: All external-facing bindings (FFI, WASM, JVM, Swift, Dart) route untrusted input through validation before passing parameters to core ring buffers.
- **`weft_init` Guard**: Construction-time geometry verification (RFC 0015) prevents integer wraps and heap overflows.
- **Litmus L1–L8 & Sanitizer Matrix**: Dedicated ASan + UBSan + TSan shard in continuous integration ensures zero-findings memory safety.

---

## Reporting a Vulnerability

**Do not open a public issue for security reports.** 
Please use GitHub's *Report a vulnerability* private security advisory or email `security@weft.dev`.
- Acknowledgment within **72 hours**.
- Mitigation or resolution plan within **30 days** for any memory corruption or boundary escape vulnerability.
