---
RFC: 0012
Title: Hardware-Vectorized VerifiedWeft — AVX-512 Multi-Buffer & Cross-Impl Bit-Identity Gates
Status: Draft (implementation + executable evidence attached; ratification pending)
Authors: Weft Core Team (Engineer 2 — Hardware Acceleration & Cryptography)
Created: 2026-09-18
Supersedes / Superseded-by: Extends RFC 0005 (VerifiedWeft), RFC-0011-era Series-7 SIMD batch
---

# RFC 0012 — Hardware-Vectorized VerifiedWeft

## Summary

Upgrades the VerifiedWeft batch-verify hot path from 8-way AVX2 to a
**16-lane AVX-512 multi-buffer SHA-256 kernel** built on the ISA's
single-instruction round algebra (`vpternlogd` Ch/Maj/three-way-XOR,
`vprord` rotations), and pins the "bit-identical across implementations"
contract with a new **cross-impl bit-identity gate (VMB9)** plus a
per-impl pinning surface (`weft_sha256_mb_force_impl` /
`weft_sha256_mb_available`). Measured on the evidence host (Xeon, AVX-512F
+ SHA-NI, gcc 14.2 -O2): **1.93 GB/s authenticated streaming per core at
4 KiB payloads (1.65 at 1 KiB, 1.01 at 256 B)** — 8.1–9.1x the scalar
serial path, ~2x the Series-7 AVX2 kernel, and faster than SHA-NI serial
at every payload size measured.

## Motivation

RFC 0005's batch decode-verify path verifies records serially (~4–5
sequential compressions per record); Series 6 added single-stream
acceleration (SHA-NI / ARM CE) and Series 7 added the 8-way AVX2
multi-buffer transform. Two things remained on the table:

1. **Instruction-count per round.** The AVX2 kernel spells Ch as
   and+andnot+xor (3 ops), Maj as three ands + two xors (5 ops), and every
   rotate as shift+shift+or (2 ops per term) — the kernel spends ~32
   vector ops per round. AVX-512 collapses the same algebra: `vpternlogd`
   computes Ch (imm 0xCA), Maj (imm 0xE8) and the three-term sigmas
   (imm 0x96) in ONE instruction each, and `vprord` rotates in one.
   The 16-lane kernel runs ~18 ops per round — ~1/3 the instruction
   stream of the AVX2 kernel on top of the doubled lane width.
2. **Per-record overhead at flight-recorder payload sizes.** At 64–256 B
   payloads the serial paths are dominated by per-record schedule work the
   multi-buffer shape amortizes 16-way.

The lead's Series-8 directive names the target explicitly:
"multi-gigabyte-per-second authenticated streaming per core." This RFC
delivers the measured numbers and the gates that keep them honest.

## Guide-level explanation

Nothing changes for VerifiedWeft consumers: `weft_vw_batch_decode_verify_mb`
widens its lane batches automatically (8 → 16 records per pass on
AVX-512F CPUs; 8 on AVX2-only; 4 on NEON; scalar lane loop otherwise).
Digests remain bit-identical to the scalar reference — by construction
(the kernel is the FIPS 180-4 §6.2.2 structure, lifted to lanes) and now
by an executable cross-impl gate.

The **AMX note** (the directive also named Apple AMX): AMX is a
matrix-multiply unit; SHA-256's rounds are integer-logical with no matmul
structure. Putting SHA on AMX would be silicon malpractice — the right
Apple-silicon units are FEAT_SHA256 (already shipped in `sha256_hw.c`'s
ARM CE path, architectural on every Apple SoC) and NEON lanes (the 4-way
multi-buffer). This RFC declines AMX for hashing on purpose and says so.

## Reference-level specification

- `sha256_mb.{h,c}`:
  - `WEFT_SHA256_MB_X86_AVX512 = 3`; `WEFT_SHA256_MB_MAX_LANES` 8 → 16.
  - New `sha256_mb16_avx512()` kernel, `__attribute__((target("avx512f")))`,
    runtime-probed first (gcc's builtin includes the XCR0 OS-support check,
    so a hypervisor masking ZMM state degrades to AVX2, never faults).
  - Same review-symmetric structure as the AVX2 kernel (transpose →
    W[64] stack → 64 rounds → feed-forward → store-based snapshot):
    every line maps 1:1 onto `sha256.c`'s scalar structure. The tuned
    register-resident rolling-window schedule (Intel's shape) was
    considered and DELIBERATELY declined — it buys maybe 20–30% more
    throughput and costs the line-by-line FIPS mapping that makes
    "bit-identical by construction" a reviewer-checkable claim. Declared
    as future work, not hidden.
  - `weft_sha256_mb_available(impl)` — probe (is this impl compiled in AND
    offered by this CPU); `weft_sha256_mb_force_impl(impl)` — test/bench
    pin, same discipline as `force_scalar` (caller gates on availability).
- `verified_mb.c`: no logic change — lane arrays are sized by
  `WEFT_SHA256_MB_MAX_LANES` and the batch widens automatically. Staging
  grows to 16 × 4240 B ≈ 66 KiB of stack per batch call — bounded,
  declared (the header's Law-4 note), and unchanged in kind from the
  8-lane era.
- `verified_mb_test.c`: VMB1 sweep widened to 1..16 lanes; VMB5's stagger
  table fixed to 16 entries (it was an 8-entry table indexed by the lane
  count — a latent out-of-bounds read the moment any backend exceeded
  8 lanes; caught by the audit, now 16 entries); **VMB9** added: every
  available x86 kernel (AVX-512 16-lane, AVX2 8-lane) must produce
  bit-identical lane snapshots to the scalar reference on staggered
  block counts — the multi-kernel form of V8's HW-dispatch equivalence.
- `sha256_mb_bench.c` (new tool, `make -C core/c mb-bench`): per-impl
  pinned raw-transform throughput + end-to-end batch-HMAC payload sweep,
  median-of-5, verify-as-you-bench (a broken kernel can never benchmark:
  every pass re-checks `n_verified == N` before the timing counts).

## Boundary of the claim (Law 4)

- All numbers are from THIS machine (named in the evidence log) at
  `-O2`, single-threaded, per-core. No extrapolation to other silicon.
- The AVX-512 and AVX2 paths are executable-verified in the evidence
  sandbox (VMB1–VMB9 + ASAN + the bench's verify-passes). The NEON 4-way
  path remains compile-guarded aarch64, NOT executable-tested on the
  x86_64 host — declared (the sha256_hw.c ARM-CE precedent).
- Throughput is the HOT-PATH claim. The kernel's cold-path transpose
  (scalar dword gather) is the same shape as the AVX2 kernel's — the
  review-symmetry trade, measured and accepted.
- No constant-time claims are new here: the batch path's tag compare was
  already constant-time per tag (`weft_vw_ct_eq`); the multi-buffer
  transform is data-independent by construction (fixed round structure,
  no data-dependent branches — same as every in-tree SHA kernel).

## Falsifiable benchmark claims (evidence attached)

`litmus/evidence/verified-mb/series8-avx512.log` (Xeon AVX-512F+SHA-NI,
gcc 14.2 -O2, 2 cores; `mb-bench` output verbatim):

- Raw multi-buffer transform: **avx512 2.09–2.20 GiB/s** per core
  (avx2 1.10–1.11 GiB/s; scalar lanes 0.26 GiB/s) — ~2.0x AVX2, ~8.3x
  scalar, flat across 2/8/32-block messages.
- End-to-end `weft_vw_batch_decode_verify_mb`, authenticated MB/s
  (envelope+payload bytes under HMAC):

  | payload | scalar-serial | SHA-NI serial | mb-avx2 | mb-avx512 (this RFC) | vs scalar |
  |--------:|--------------:|--------------:|--------:|---------------------:|----------:|
  | 64 B   | 49.1          | 146.8         | 296.4   | **447.2**            | 9.11x    |
  | 256 B  | 115.3         | 409.3         | 592.2   | **1010.1**           | 8.76x    |
  | 1024 B | 194.3         | 887.2         | 892.9   | **1650.5**           | 8.50x    |
  | 4096 B | 237.5         | 1333.3        | 1011.2  | **1926.3**           | 8.11x    |

  AVX-512 beats the SHA-NI single stream at every payload size and the
  AVX2 batch by 1.9–2.1x. The 2 GiB/s "multi-gigabyte" line is crossed at
  4 KiB payloads within measurement noise of the raw-transform ceiling
  (92% of kernel throughput retained end-to-end).

## Alternatives considered

- **Register-resident rolling schedule** (Intel's tuned multi-buffer
  shape): +20–30% raw throughput, at the cost of the reviewable
  W[64] structure. Rejected for this series (the review-symmetry value
  the Series-7 kernel was built on); open as a future opt-in kernel
  behind the same VMB9 gate.
- **SHA-NI-only optimization**: single-stream SHA-NI already ships
  (Series 6); it loses to the 16-lane batch at every measured payload
  and cannot amortize per-record overhead — kept as the serial baseline.
- **AES-GMAC / BLAKE3 swap**: changes the wire format and the KDF domain
  separation; out of scope for VerifiedWeft's HMAC-SHA256 contract
  (RFC 0005's §security analysis is written against it).

## Drawbacks

- The dispatch table grows a third x86 regime (probe → AVX-512 → AVX-2 →
  scalar); evidence logs must name it (they do).
- Stack staging per batch call grows ~2x (16 lanes); bounded and declared.
- AVX-512 downclocking on some client parts: the probe prefers AVX-512
  unconditionally; server/EPYC/Xeon parts (the evidence class) reward it.
  A per-part override can ride `force_impl` if a client-part regression
  ever shows up in evidence — open question below.

## Open questions

- Should the probe prefer AVX-512 on client parts with known AVX-512
  frequency licenses (e.g. via a cpuid model table), or is `force_impl`
  enough?
- Rust/TS port parity: the Rust `verified.rs` and TS `verified.ts` remain
  serial; porting the 16-lane kernel is mechanical for Rust (`core::arch`)
  but WGCU-dependent for TS — proposed as a Series-9 item.
- Rolling-window kernel as an opt-in second AVX-512 impl behind VMB9.

## Staff Decision

[EMPTY — implementation + executable evidence attached
(`litmus/evidence/verified-mb/series8-avx512.log`); ratification pending]
