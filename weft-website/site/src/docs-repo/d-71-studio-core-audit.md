# D-71 — Weft Studio Core Audit (Pillar 7, Engineer 1)

**Scope:** `weft-lsp` headless language server, `weftc` in-memory/WASM
schema compiler, `.weftrec` v1 flight-recorder trace engine.
**Status:** ALL GATES GREEN (G1–G6, `tools/studio/tests/run_studio_core_suite.sh`,
`tools/studio/evidence/suite-full.log`).
**Toolchain:** gcc 14.2.0-19 and clang 19.1.7 (dual-compiler strict),
2-core sandbox Xeon, `-O2` for measurements.

---

## 1. Delivered surface

| Artifact | Contents |
|---|---|
| `core/c/include/weft_studio.h` | frozen C-ABI v1: 5-code status ladder, type tags, `weft_ast_node_t` (40 B), `weft_field_layout_t` (64 B, one cacheline per reflected field), `weft_struct_layout_t` (88 B), `weft_diag_t` (40 B), `weftrec_header_t` / `weftrec_index_entry_t` / `weftrec_frame_header_t` (64 B each), reader/walker/builder + compiler + LSP API. 40+ `_Static_assert` size/offset pins, `extern "C"` guarded, C++17-include-safe (G6). |
| `core/c/studio/src/weftc_inmem.c` | in-memory `.weft` compiler: RFC-0017 lexer/parser (panic-mode recovery), layout engine, WH64 + WAB1/WID1/WDC1 manifest fingerprints, cache-line diagnostics, 7-language code previews. `<string.h>` only. |
| `core/c/studio/src/weft_lsp.c` | JSON-RPC 2.0 + LSP engine over memory buffers: initialize/initialized/shutdown/exit, didOpen/didChange(incremental)/didClose, publishDiagnostics, hover, completion, semanticTokens/full; Content-Length stdio transport over a callback seam (separate read/write user pointers). |
| `core/c/studio/src/weftrec_engine.c` | WEFTREC1 v1: reader with O(log n) timestamp seek over the in-file 64 B index, in-place CRC-verified frame walker, builder with in-buffer two-pass dzv codec; CRC-32C SSE4.2 kernel + load-time dispatch + slicing-by-8 reference. |
| `core/c/studio/src/weft_crc32c_tables.h` | generated slicing-by-8 tables (`tools/studio/gen_crc32c_tables.py` provenance; check vectors 0xE3069283 / 0x8A9136AA verified in-suite). |
| `tests/studio/core/` | C/L/R/A/F series: 5 binaries, 17,855+ checks plain + sanitizer legs. |
| `tools/studio/` | `weft-lsp-server` (stdio CLI), `weftrec-inspect` (trace dump CLI), `Makefile`, gate suite + fixtures + evidence. |

## 2. Law compliance ledger

| Law | Contract | Evidence |
|---|---|---|
| **1 — zero allocation on query & replay** | no malloc/realloc/calloc in LSP sync, diagnostics, layout queries, hover/completion/semanticTokens, or `.weftrec` playback | malloc interposition (dlsym `RTLD_NEXT`, ISO-safe memcpy symbol resolution): **0 events over 200,000 cycles** (100k LSP + 100k trace; 812 ms); `mallinfo2` uordblks delta 0 B; all engine pools are fixed arrays inside caller-provided contexts, exhaustion = `EBOUNDS` refusal |
| **2 — bit-exact 64B/128B cache-line alignment** | per-field byte offsets, padding gaps, `cache_line_idx`, crossing tripwires | every `weft_field_layout_t` carries offset/size/align/`padding_after`/`cache_line_idx` + `CROSSES_CL64`/`CROSSES_CL128`/`FALSE_SHARING` flags; golden frames schema: `blob` @120 size 16 → flags 0x23 (crosses 64, crosses 128, false-sharing); diagnostics 2101/2102/2103/2104 with exact LSP bounds; `weftrec` header/index/frames pinned at 64 B by static asserts |
| **3 — dual-compiler strict discipline** | `-std=c11 -Wall -Wextra -Werror -pedantic` under GCC **and** Clang, ASan/UBSan clean | G1 builds all engines, tests, and tools under gcc 14.2 **and** clang 19.1.7 (declared boundary: the directive names Clang 21; the sandbox provides 19.1.7 — same strict flag set, no suppressions anywhere in the tree); G5: ASan+UBSan green on all oracles + 2.5 M-cycle reduced fuzz; memcpy-only wire loads; no x86 intrinsics outside the guarded kernel |
| **4 — strict ABI freezing & C++ compatibility** | immutable header contracts, `_Static_assert` offsets, `extern "C"`, C++17 inclusion | G6: header compiled from C++17 TU with every assert firing; all offsets pinned in-table below; C++17 inclusion log in evidence |

### 2.1 Frozen ABI offset table (runtime-verified, G6)

| Struct | Size | Pinned offsets |
|---|---|---|
| `weft_ast_node_t` | 40 | kind 0, parent 4, first_child 8, next_sibling 12, name_off 16, name_len 20, line 24, col 28, value 32 |
| `weft_field_layout_t` | 64 | name 0, type_str 8, type_tag 16, orig_index 20, offset 24, size 32, align 40, padding_after 48, cache_line_idx 56, flags 60 |
| `weft_struct_layout_t` | 88 | name 0, fields 8, holes 16, abi_hash 24, size 32, align 40, internal_pad 48, trailing_pad 56, optimize_hint 64, field_count 72, hole_count 76, flags 80, cache_line_span 84 |
| `weft_diag_t` | 40 | message 0, fix_suggestion 8, code 16, severity 20, flags 22, start_line 24, start_col 28, end_line 32, end_col 36 |
| `weftrec_header_t` | 64 | magic 0, version 8, flags 12, index_offset 16, index_count 24, frame_count 32, first_ts 40, last_ts 48, stream_cardinality 56, crc32c 60 |
| `weftrec_index_entry_t` | 64 | timestamp_ns 0, frame_offset 8, next_offset 16, frame_seq 24, stream_id 32, payload_size 40, flags 44, ts_delta_prev 48, reserved0 56 |
| `weftrec_frame_header_t` | 64 | magic 0, timestamp_ns 8, stream_id 16, frame_seq 24, payload_size 32, stored_size 36, codec 40, flags 42, crc32c 44, next_offset 48, header_crc32c 56, reserved0 60 |

## 3. weftc in-memory compiler — reference parity (the interop proof)

The studio compiler reproduces the **Pillar-1 `weftc` reference
compiler's fingerprints bit-for-bit** on the golden `frames.weft`
schema (WH64 over the canonical WAB1/WID1/WDC1 manifests, plus the
independent FNV-1a-64 cross-check):

| Fingerprint | Studio engine | Pillar-1 golden |
|---|---|---|
| schema abi_hash | `0x0e7280ce8f313931` | `0x0e7280ce8f313931` |
| schema_id | `0x5b31c5b9f9f7b3eb` | `0x5b31c5b9f9f7b3eb` |
| fnv1a64_abi | `0x94a2f711e9ad2f16` | `0x94a2f711e9ad2f16` |
| `Kind` / `Caps` / `Vec3` | `0xf7714909a63a66ad` / `0x70dfc168b7e518cc` / `0x61ddb568acf95b0f` | identical |
| `Header` / `TelemetryMsg` | `0x8427797a68caa357` / `0x5f12f453dd569495` | identical |
| `CachelineFrame` / `BigFrame` | `0x79f04e9440822e70` / `0x94c196087085eef4` | identical |

Layout numbers match the golden JSON exhaustively (C1: 203 checks),
including the `@optimize(packing)` reorder with preserved `orig_index`
and the `@align(64)` effect-only manifest property. Consequence: a
schema laid out live in the Studio carries the *same* ABI handshake
number as one compiled offline by weftc — Engineers 2/3 can exchange
that one u64 across the FFI boundary with zero negotiation.

**Compile SLA:** 17.5 µs per compile + two codegen targets
(best-of-31 batches of 100; budget 1,000,000 µs) — 57× headroom.

## 4. weft-lsp — protocol surface and goldens

- **Position encoding:** `utf-8` declared in capabilities; byte offsets
  are character offsets — the L-series fixtures pin exact character
  positions on every construct (hover on fields/structs/primitives,
  diagnostic bounds, semantic-token deltas).
- **Sync model:** recompile-on-change (a full compile is ~18 µs, three
  orders below budget), so `didChange` incremental sync is purely a
  text-splice concern (in-buffer `memmove`, `EBOUNDS` on capacity) and
  the engine carries no incremental-AST state to drift.
- **Golden transcript (G6):** the stdio server's full framed session
  (initialize → didOpen → publishDiagnostics → hover → completion →
  shutdown → exit) is byte-identical to
  `tools/studio/tests/tools/fixture.lsp.golden`.
- **Robustness:** malformed JSON → `-32700` + `EPARSE`; unknown methods
  → `-32601` for requests, silence for notifications; missing method
  with id → `-32600`; negative error codes render as JSON negatives.
- The L-series caught and fixed two real defects pre-delivery: a
  response-envelope brace imbalance (invalid JSON on field hovers) and
  a single-user transport seam that forced stdin onto the write path.

## 5. `.weftrec` v1 — format, integrity, SLAs

**File layout:** 64 B global header (magic `"WEFTREC1"` ASCII —
hexdumps spell the format name; the ABI constant `0x5745465452454331`
is its big-endian reading exactly as the directive pins it) →
64 B-aligned frames (`frame header + stored payload + zero pad`) →
64 B-aligned index (one 64 B entry per frame) → final 64 B pad.

**Integrity tripwires (all fail-closed):**
1. header CRC-32C over bytes [0,60);
2. per-frame payload CRC-32C over the *stored* bytes;
3. per-frame header CRC-32C over [0,48) — deliberately excludes
   `next_offset` ([48,56)) because the builder back-patches it when the
   next frame appends; the walker compensates with a step bound
   (`index_count + 1`) that closes the corrupted-chain cycle hazard the
   fuzzer surfaced.

**SLAs (native build, best-of-31 batches, precomputed target arrays —
see `R9`):** seek **93 ns** over a 4096-frame index (budget 100 ns);
walk **12 ns/frame** with CRC verification (budget 50 ns). Under
sanitizers the same gates are correctness-only (instrumentation ≈ 2×
latency; declared in the suite). The walk SLA is measured on 64 B raw
frames — dzv decode is O(payload) on top; the CRC kernel is SSE4.2
when present, slicing-by-8 otherwise (`weftrec_crc32c_hw_active()`).

**Fuzz telemetry (10 M cycles, fixed seeds):** 6 M schema cycles
(soup/mutation/adversarial; 92 determinism re-checks, all stable) +
4 M trace cycles — **1,000,629 clean traces walked bit-exact
(13,509,473 frames verified)**; corrupted traces only ever return
ladder codes: ECRC 1,963,549; EPARSE 344,265; ETRUNC 333,814;
EALIGN 11,063; zero crashes, zero leaks (ASan leg).

## 6. Defect log (self-review, caught by own gates before delivery)

1. line-comment lexer path did not advance the cursor (infinite token
   loop) — caught by the first parity smoke;
2. `line_starts[]` recorded stale positions (diagnostic ranges shifted
   by 1–2 columns) — caught by golden diagnostic bounds;
3. message-pool strings lacked NUL terminators (diagnostics ran
   together) — caught by golden messages;
4. WE004 duplicate-field check was dead code behind an early
   `continue` — caught by the C4 battery;
5. frame-header `reserved0` never written by the builder (stale bytes
   failed validation on buffer reuse) — caught by the R-series walk
   after earlier traces dirtied the buffer;
6. frame-header CRC originally covered the back-patched `next_offset`
   (invalidating every earlier frame) — found in review, fixed to
   [0,48) + walker step bound;
7. walker cycle hazard on corrupted `next_offset` (outside CRC region)
   — surfaced by the random-flip battery; closed by the step bound;
8. `weft_lsp_serve` single user pointer forced stdin onto the write
   callback — API split into read/write user pointers;
9. field-hover response envelope brace imbalance — caught by the
   byte-exact L-series goldens;
10. diagnostic code names rendered 2-digit (`WE07`) instead of the
    RFC's 3-digit (`WE007`) — caught by the WE007 fixture;
11. `weftrec-inspect` used plain `malloc` (16 B alignment) against the
    reader's 64 B EALIGN gate — moved to `aligned_alloc`.

## 7. WASM readiness (declared boundary)

The engines use `<string.h>` only — no stdio, no malloc, no threads,
no atomics, no syscalls. The SSE4.2/ARMv8 CRC kernels compile away
under `-DWEFT_STUDIO_WASM_PORTABLE` (the software slicing-by-8 path
remains; G1 builds that profile strict). All byte offsets are u32 and
all wire math is explicit-LE. Big-endian hosts are refused at compile
time (`WEFT_STUDIO_ALLOW_BE` override documented, untested). **No emcc
exists in the sandbox** — the wasm32-unknown-emscripten /
wasm32-unknown-unknown targets are compile-ready by construction but
runtime-unverified; declared as the follow-up alongside the Studio UI
integration (Engineer 3 owns the browser harness).

## 8. Boundaries and handoffs

- **Engineer 2 (SHM Inspector):** consume `weft_studio.h` + the
  `.weftrec` reader/walker; the trace file maps directly (page-aligned
  mmap satisfies the 64 B EALIGN gate); `weftrec-inspect` is the
  reference CLI. dzv decode is exported for the Inspector's replay.
- **Engineer 3 (Studio UI):** drive `weft_lsp_handle` per keystroke
  (recompile-on-change makes diagnostics + hover + semantic tokens
  uniformly fresh); the semantic-token legend and diagnostic code
  spaces are frozen in the header; code previews via `weftc_codegen`.
- **Not done (deliberate):** emcc build + browser runtime (no
  toolchain in sandbox); CI workflow wiring (outside the directive's
  allowed territory this pillar — the suite is self-contained under
  `tools/studio/`); workspace/multi-document LSP (single in-memory
  document per context, per the Studio's single-schema-tab model);
  SBE/ITCH surfaces belong to Pillar 6.

## 9. Evidence index (`tools/studio/evidence/`)

`suite-full.log` (complete gate transcript) · `g1-*-gcc/clang.log`
(strict builds) · `g2-inmem/lsp/rec.log` (oracle suites with SLA
prints) · `g3-fuzz.log` (10 M-cycle telemetry) · `g4-alloc.log`
(interposition + mallinfo2) · `g5-*.log` (sanitizer legs) ·
`g6-cpp17.log` / `g6-lsp.log` / `g6-inspect.log` (ABI, stdio
transcript, round-trip).
