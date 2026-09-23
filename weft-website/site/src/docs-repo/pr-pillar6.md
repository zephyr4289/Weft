# PR — Pillar 6: weft-adapters Core (Schemas, SBE/ITCH 5.0 Transcoders & Protocol Engines)

**Branch**: `feat/weft-adapters-core` (7 DCO-signed commits, rebases cleanly onto `main` via `git am`)
**Author**: Senior Engineer 1 — Core / Protocols / Formal Logic / Memory Layouts
**Audit**: `docs/reports/D-61-ADAPTERS-CORE-AUDIT.md`
**Suite**: `tools/adapters/tests/run_adapters_core_suite.sh` → **PASS** — 27,554,677 checks, 0 failures

## What lands

| Component | Detail |
|---|---|
| **Frozen C-ABI** (`core/c/include/weft_adapters.h`) | Single source of truth for Engineer 2 (rmw_weft, Vision DMA) and Engineer 3 (managed SDKs / L3 book UI). 82 `_Static_assert`s byte-freeze every projection; C++17-include-safe; ABI version probe. |
| **ITCH 5.0 parser** (`weft_itch50.c`) | All 14 directive-mandatory types + 5 extended (L/V/W/K/J) = 19 types; raw fixed framing AND MoldUDP64 length-prefix framing; validation-before-write (fail-closed: output untouched on any refusal). **60.8 M msg/s fixed / 65.6 M msg/s len-prefix single-core** — 1.5–1.6× over the 40 M bar. |
| **OUCH 5.0 core subset** | 6 outbound types over SoupBinTCP payload framing (spec-sheet cross-validation = declared follow-up for Engineer 2). |
| **MoldUDP64 walker** | Zero-copy message-block iterator; feeds the PCAP oracle end-to-end. |
| **SBE transcoder** (`weft_sbe.c`) | Schema-driven decode with zero-copy group/var slices, full bounds verification, canonical Weft-MD template, and `weft_sbe_md_to_itch_add` — direct projection into 64-byte ITCH-shaped messages. |
| **SIMD checksum engine** (`weft_adapter_checksum.c`) | CRC-32C: SSE4.2 runtime-dispatch (target-attributed, binary stays portable) + ARMv8 path + slicing-by-8 reference — **8.1–8.5 GB/s vs 2.1–2.3 reference (3.6–3.9×)**. Adler-32: provable SSE2 madd kernel (**up to 11.5 GB/s, 2.3–3.9×**). WAF frame integrity engine. |
| **Golden harness** (`tests/adapters/core/`) | Bit-exact oracles (1,696 + 334 + 2,659 checks), deterministic synthetic PCAP/bin fixtures with CRC sidecar, **10,000,000-cycle fuzz** (26.7M invariant checks, zero heap growth), **malloc-interposition probe: 0 allocation events across 7 surfaces × 200k cycles** (Law 1), ASAN+UBSAN clean, gcc + clang strict gates (`-std=c11 -Wall -Wextra -Werror -pedantic`). |

## Law compliance

1. **Zero-allocation on parse & transcode** — no allocation site exists in any adapter source; proven by interposition (0 events), `mallinfo2` Δ=0 over 10M cycles, and ASAN leak gates.
2. **Bit-exact 64-byte cache-line packing** — uniform 64 B stride, natural scalar alignment, explicit padding, 82 compile-time asserts, runtime `EALIGN` enforcement.
3. **Cross-architecture portability** — clean under both gcc 14.2 and clang 19.1.7 with the full strict flag set; memcpy-based wire loads (no misaligned 64-bit dereferences); ARMv8 CRC32C guarded path.
4. **Strict frozen C-ABI** — tags, structs, error codes and signatures documented in-header for direct binding.

## Handoff notes for Engineer 2 / Engineer 3

- Bind against `weft_adapters.h` only; field offsets are ABI (same numbers as D-61 §3).
- Batch outputs must be 64-byte aligned (`WEFT_ALIGNED64` / `aligned_alloc(64, n*64)`).
- `WEFT_ADAPTER_ETRUNC/EBADMSG/EALIGN/ECRC/ESCHEMA/EBOUNDS` are the only refusals; on any negative status, previously decoded batch entries remain trustworthy (D-61 §6 O9).
- SBE dimension encoding pinned to u8/u8; schemas with other encodings transcode at the seam.

## Declared boundaries (D-61 §9)

OUCH spec-sheet cross-validation; aarch64 runtime CRC dispatch (compile-time path only); SBE forward-compat block growth; NEON Adler-32; PCLMULQDQ CRC folding.
