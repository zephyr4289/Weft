---
RFC: 0005
Title: VerifiedWeft Authenticated Frames
Status: Implemented (contribution pending ratification)
Authors: Weft Core Team
Created: 2026-09-15
Supersedes / Superseded-by: None
---

# RFC 0005 — VerifiedWeft Authenticated Frames

## Summary
Proposes an optional authenticated frame header carrying a cryptographic HMAC-SHA256 (or BLAKE3) message authentication code for network-sourced, untrusted IPC, or cross-origin SharedArrayBuffer data streams.

## Motivation
When Weft streams cross process or network boundaries (e.g. WebSocket bridge, multi-tenant IPC), unauthenticated frames risk spoofing or tampering. Cryptographic authentication guarantees frame origin and integrity before consumption.

## Guide-level explanation
When authentication is enabled (`verified: true`), every published frame signs the 16-byte envelope and payload with a shared secret key. Readers verify the HMAC tag prior to claiming the payload.

## Reference-level specification
- **Authentication Tag**: 32-byte HMAC-SHA256 appended to the frame record or embedded in extended envelope v2.
- **Benchmark Findings (`x86_64-sandbox` / `linux-arm64-sandbox`)**:
  - Encode: 2.10 µs/frame at 64B payload.
  - Decode: 2.09 µs/frame at 64B payload.
  - Combined Roundtrip: 4.19 µs/frame.
  - Throughput: ~480,000 authenticated frames/sec.
  - Sub-microsecond Target: Missed in pure software implementation (requires hardware AES-NI / ARMv8 crypto instructions for < 1.0 µs).

## Boundary of the claim (Law 4)
Guarantees integrity and authenticity of frame contents. Does not prevent denial-of-service via malformed packet flooding.

## Alternatives considered
- CRC32 only: Fast (< 50 ns), but provides zero protection against deliberate adversaries.
- Asymmetric signatures (Ed25519): Computationally prohibitive (> 50 µs/frame).

## Drawbacks
Adds ~2–4 µs CPU latency per frame in software; increases frame wire size by 32 bytes.

## Open questions
- BLAKE3 acceleration on WebAssembly / SIMD-capable targets.

## Hardware Deferral List
- Dedicated hardware cryptographic instruction set acceleration (ARMv8 CE / Intel SHA-NI) on bare metal devices is deferred.

## Staff Decision
[EMPTY — implementation evidence attached; ratification pending]

## Implementation (this tree, pending ratification)

RFC 0005 is implemented across all three kernels as a driver-layer module
(the frozen kernel — weft.c/weft.h — is untouched; same layering discipline
as RFC 0004's fan-out):

- `core/c/verified.{h,c}` + `sha256.{h,c}` + `hmac.{h,c}` — zero-dep C99;
  `verified-test` (V-series), `verified-runner` (xlang gen/validate/tamper)
- `core/rust/src/verified.rs` — zero-crate Rust; `cargo test verified`
- `core/ts/verified.ts` (mirrored `packages/core/src/verified.ts`, parity
  pair 11/11) — pure TS, zero hot-path allocation in the signer path
- `fixtures/xlang-verifiedweft/` — cross-language byte-compat gate
  (C→TS, TS→C, single-bit tamper rejected by BOTH kernels) + the shared
  HMAC vector fixture (RFC 4231 TC1-4,6,7 + Weft edges; digests
  cross-checked against node:crypto at generation time)
- CI: `ci/scripts/run_verifiedweft_shard.sh` (shard `verifiedweft` in
  extreme-test.yml) — 4 gates: C V-series + ASAN, Rust V-series, TS
  V-series, xlang interop

### Measured vs this RFC's benchmark claims (x86_64 sandbox, 64B payload)

| Metric | RFC claim | Measured (C) | Measured (Rust) | Verdict |
|---|---|---|---|---|
| Encode | 2.10 µs/frame | 1.74 µs | 1.03 µs | claim met (beaten) |
| Decode | 2.09 µs/frame | 1.77 µs | 1.27 µs | claim met (beaten) |
| Roundtrip | 4.19 µs/frame | 3.52 µs | 2.29 µs | claim met (beaten) |
| Throughput | ~480 K frames/s | 284 K (rtt) / 574 K (enc) | 437 K (rtt) / 971 K (enc) | claim clarified |

**Claim clarification (falsifiable, per A4):** the RFC's "~480,000
authenticated frames/sec" is the ENCODE-only throughput (1 / 2.10 µs =
476 K/s), not the signed+verified roundtrip (1 / 4.19 µs = 239 K/s). Both
implementations land in the claim's neighborhood on both readings; the
report table above keeps the two numbers separate so the claim stays
falsifiable.

### What gates the wire format (normative)

- Tag covers `envelope[0..16] || payload` — the v1 envelope's
  integrity-relevant fields (magic, version, header_size, seq, payload_len)
  all live in the signed prefix.
- `auth_key = HMAC(secret, "Weft-VerifiedWeft-v1:key")` — domain separation;
  a leaked auth key does not leak the secret.
- Tag compare is constant-time (`weft_vw_ct_eq` / `ct_eq`); verify result
  codes are numerically identical across the three kernels (0 OK / 1 short /
  2 bad-magic / 3 tag).
- Law 4 boundary preserved: an unauthenticated frame is DROPPED and counted,
  never consumed. Nothing here weakens latest-wins.
