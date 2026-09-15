---
RFC: 0005
Title: VerifiedWeft Authenticated Frames
Status: Draft
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
[EMPTY]
