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
- ~~Dedicated hardware cryptographic instruction set acceleration (ARMv8 CE / Intel SHA-NI) on bare metal devices is deferred.~~ **Realized in Series 6** (SHA-NI + ARMv8 CE runtime dispatch, `core/c/sha256_hw.c`); BLAKE3 remains deferred.

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

### Series 6 — hardware acceleration + batch verification (this tree)

The Hardware Deferral List item below is now REALIZED for SHA-NI and
ARMv8 CE: `core/c/sha256_hw.c` adds runtime-dispatched accelerated
compression (x86 SHA extensions via `target("sha,sse4.1")` function
attributes, aarch64 CE via `-march=armv8-a+crypto` + HWCAP probe), with the
scalar path kept as the normative reference. `verified.{h,c}` add a
pre-keyed verifier (`weft_vw_verifier_t`) and a stream batch API
(`weft_vw_batch_decode_verify`). Measured on the same sandbox shape as the
table above (x86_64, SHA-NI present, 64 B payload, 200k frames —
`verified-runner bench`, evidence `litmus/evidence/verified/series6-hw-bench.log`):

| Path | Series 5 (scalar) | Series 6 (SHA-NI) | Target > 1,000 K/s |
|---|---|---|---|
| Sign (encode) | 659 K/s (1.52 µs) | 1,934 K/s (0.52 µs) | met (2.9x) |
| Verify one-shot (per-call key) | 576 K/s (1.74 µs) | — | superseded by the two below |
| Verify pre-keyed (stream) | — | 1,935 K/s (0.52 µs) | met (3.4x vs one-shot) |
| Batch decode-verify (one walk) | — | 1,936 K/s (0.52 µs) | met |

Digest equivalence between regimes is GATED, not asserted: V8 sweeps the
shared fixture vectors plus 512 randomized buffers under both
`weft_sha256_force_scalar()` and runtime dispatch and requires
byte-identical tags; the xlang C<->TS gate runs green under the accelerated
regime. The aarch64 CE transform is compile-guarded and NOT
executable-tested in the x86_64 sandbox — declared (ARM CI covers it).

BLAKE3 remains deferred (open question unchanged).


### Series 7 — SIMD batch verification + vectorized envelope scanning (this tree)

The batch path's remaining serialization is now parallelized:
`sha256_mb.{h,c}` add a multi-buffer SHA-256 transform (8-way AVX2 on x86-64,
4-way NEON compile-guarded on aarch64, scalar lane-loop reference) with
SNAPSHOT lane semantics — lanes declare different block counts and each
lane's state is captured at its own finish, so mixed-length records batch
together. `verified_mb.{h,c}` add `weft_vw_batch_decode_verify_mb` (the
multi-buffer mirror of the Series-6 batch API: one ipad/opad amortization
per key, two lane-batched transforms per 8-record group, semantics pinned
identical by VMB3/VMB4 sweeps under both regimes) and `weft_vw_scan_magic`
(a vectorized "WEFT" magic scan for crash-tolerant stream resync — the
primitive the resync idiom in the verified_mb.h header builds on).

Measured on the same sandbox shape as the Series-6 table (x86_64, SHA-NI
present, 20k frames, `verified-runner bench-mb`, evidence
`litmus/evidence/verified-mb/bench-mb-payloadsweep.log`):

| Payload | Series 6 batch (SHA-NI, serial) | Series 7 batch (AVX2, 8-way) | Speedup |
|---|---|---|---|
| 16 B | 2,457 K/s | 5,221 K/s | 2.12x |
| 64 B | 1,937 K/s | 4,014 K/s | 2.07x |
| 256 B | 1,569 K/s | 2,299 K/s | 1.46x |
| 1024 B | 894 K/s | 867 K/s | 0.97x (parity) |

The crossover is honest and stated: multi-buffer wins where records are
SHORT (the flight-recorder / bridge-ingestion hot case — the ipad/opad pad
blocks dominate the math); at ~1 KiB payloads SHA-NI's single-stream
multi-block advantage balances the lane packing, and the cap boundary
(WEFT_VW_MB_MAX_PAYLOAD) routes larger records to the serial path with
identical semantics (VMB8). The magic scan measures 6.5-7.4 GB/s on a
64 MB haystack vs 2.6 GB/s scalar (byte-mask AND chain, 29-byte window
seams covered exactly — VMB6 plants a needle at EVERY offset).

Equivalence is GATED, not asserted: VMB1 sweeps the transform against the
per-lane scalar reference across staggered/zero lane counts (both regimes);
VMB2/VMB3/VMB4 sweep the batch API against the Series-6 serial path across
149 generated streams and every corruption class — codes, counters, and
views byte-identical. The NEON 4-way transform and phase-shifted dword scan
are compile-guarded aarch64 and NOT executable-tested in the x86_64 sandbox
— declared (ARM CI covers them), mirroring the Series-6 CE declaration.

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
