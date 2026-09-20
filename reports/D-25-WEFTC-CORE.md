# D-25 — WEFTC-CORE: Universal Zero-Serialization Schema Compiler & Static Layout Engine

**Branch:** `feat/weftc-compiler-core` (3 DCO-signed commits, stacked on `origin/main` @ db24254)
**Spec:** `rfcs/0017-weftc-universal-schema.md` (Draft)
**Code:** `tools/weftc/` — C11 + libc only, ~5,300 lines incl. tests and RFC
**Gates:** WL/WH/WD suites + CLI + goldens + handshake demo — **plain AND ASAN legs, ALL PASS** (transcript: `litmus/evidence/weftc/shard-local-transcript.txt`)

---

## 1. What landed

Pillar 1 of the zero-copy memory fabric: a compiler that turns `.weft`
schema files into **bit-exact, frozen memory layouts** — so reading a
message from SHM, an FFI boundary, GPU VRAM, or a network-mapped ring is a
single pointer cast / DataView at **0 ns decode**.

```
source -> lexer -> parser -> AST -> layout engine -> IR + fingerprints
                                             -> JSON / binary IR (WIR1)
                                             -> C11 verify header (_Static_assert pins)
                                             -> terminal ASCII memory map
```

| Component | File | What it does |
|---|---|---|
| Diagnostics engine | `src/diag.c` | GCC/Clang-style `path:line:col: error[CODE]:` + caret rows + secondary spans + notes/help; arena, byte buffers, source loading |
| Lexer | `src/lex.c` | spans, comments, dec/hex/bin literals with `_` separators, saturation on overflow |
| Parser | `src/parse.c` | recursive descent, panic-mode recovery — one run reports every real error |
| Layout engine | `src/layout.c` | THE determinism core: natural alignment, `@packed/@align/@simd/@optimize(packing)`, holes/trailing pad, overflow walls, cycle detection, 256-level containment bound |
| Fingerprints | `src/hash.c` | WH64 (portable wyhash-style mixer) + FNV-1a-64 over canonical WAB1/WID1/WDC1 manifests |
| Emitters | `src/dump.c` | JSON IR, binary IR, verify header, inspect memory map |
| CLI | `src/main.c` | `check / inspect / compile`, `--dump-ir`, exit codes 0/1/2 |

## 2. The design decisions that matter

**1. Effect-only ABI manifest vs. name-bearing ID manifest.** The wire
hash (`abi_hash`) records layout *effects* — renaming a field, reordering
declarations, or adding a no-op `@align(1)` never breaks wire
compatibility. The identity hash (`schema_id`) records declared shape —
every rename flips it. One number for the handshake, one for tooling
diffs. Both flip on any real layout change.

**2. `span<T>` is a reference, not a containment edge.** `struct Node {
next: span<Node> }` is legal and finite; `next: Node` is WE008. This is
what makes linked topologies over a ring expressible without unbounded
types — the same trick the kernel uses (offset + len into the payload
region), now first-class in the type system.

**3. Verify header = drift becomes a build error.** `weftc compile` emits
`_Static_assert(sizeof(T) == N)` + per-field `offsetof` pins. The golden
handshake demo proves both directions: hand-written C structs compile
clean against the pins; a field-swapped drift variant is **rejected at
compile time** with `weftc: Header.kind offset drifted (expected 8)`.

**4. @optimize(packing) is a pure function of the source.** Sort by
alignment descending, ties by declaration index — deterministic,
host-independent, recorded (`reordered` + per-field `orig_index`). The
compiler never silently reorders: unoptimized structs get a *hint*
(“16 B, saves 8 B”) because applying it changes the ABI hash — it is a
schema edit, done in the source.

**5. WH64 + FNV-1a carried together.** Two independent fingerprints over
the same manifest; a producer bug in either is caught by the other
(WH8). Avalanche measured 48–52 % per output bit; zero collisions over a
1M-input corpus; pinned reference vectors + external FNV anchors.

## 3. Verification battery (all green, plain + ASAN)

| Suite | Checks | Headline content |
|---|---|---|
| WL layout | 245 | primitives, holes, cascades, array stride-includes-trailing-pad, every attribute, 30-code semantic battery, 32-case malformed-input armor, forward refs, span-cycle break, 300-deep bound |
| WH hash | 118 | pinned vectors, FNV anchors, avalanche, order/length sensitivity, 1M collision-free, full mutation matrix, comment-immunity |
| WD dumps | 36 | 4× byte-determinism, WIR1 parse-back, verify-header content, inspect sanity, 4 golden byte-compares |
| CLI | 11 | exit codes 0/1/2, artifact-golden identity through the real CLI, external python3 JSON validation |
| Handshake | 2 | consumer positive + drift negative (compile-time rejection) |

Bugs my own gates caught and fixed pre-commit (house culture, short
list): keyword-vs-ident mismatch rejecting `@optimize(packing)`;
length-prefixed encoder leaking into canonical type strings; JSON hash
tokens unquoted; duplicated diagnostic source-line rendering; containment
depth not incremented through named types (300-deep test); silently
accepted unclosed braces; `0x`-with-no-digits literal swallowed; manifest
recording attribute *spellings* instead of *effects* (the WH6 matrix
pinned the correct semantics); ASAN binaries accidentally committed and
removed via amend with `.gitignore`.

## 4. Kernel freeze + CI

Zero kernel surface: `core/`, `packages/`, all frozen files untouched
(diff empty). New `weftc` shard registered in the extreme-test matrix
(`any_code` routing row in `.github/workflows/extreme-test.yml`, script
`ci/scripts/run_weftc_shard.sh`, documented in `ci/README.md`) — runs the
full gate twice (plain + ASAN); full local run PASS, transcript committed.

## 5. What is deliberately NOT here

- **Codegen** (C11/Rust `#[repr(C)]`/WGSL — Engineer 2; TS DataView/Dart/
  Swift/Python — Engineer 3). The IR, hashes and verify headers they
  consume are frozen by RFC-0017; the generators are their pillars.
- **Schema evolution/migration** — a schema is a frozen contract;
  evolution = new `schema_id`, checked at the handshake.
- **BE-hardware leg** — endianness is declared-for and serialized
  explicitly LE; no big-endian host exists in CI (declared follow-up).
- **RFC status flip** — filed Draft; status changes are staff actions.
