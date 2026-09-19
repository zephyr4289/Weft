---
RFC: 0017
Title: weftc — Universal Zero-Serialization Schema Compiler and Static Layout Engine
Status: Draft
Authors: weft-contributor
Created: 2026-09-20
Supersedes / Superseded-by: None
---

# RFC 0017 — weftc: Universal Zero-Serialization Schema Compiler and Static Layout Engine

## Summary

Defines `.weft`, a schema language for declaring message types whose memory
representation is **computed at compile time and frozen**: byte-exact sizes,
field offsets, alignments and padding. A new tool, `weftc`, compiles one
`.weft` file into (a) a deterministic **IR** (JSON and binary) that downstream
generators consume, (b) a per-type 64-bit **ABI hash** enabling zero-cost
layout handshakes across IPC / FFI / GPU / network-mapped memory, and (c) a
C11 **verify header** that pins `sizeof`/`offsetof` with `_Static_assert`, so
any drift between schema and consumer code becomes a *compile-time* error.

Reading a Weft message from shared memory, an FFI boundary, GPU VRAM or a
network-mapped ring is then a single pointer cast (`*const T`) or DataView —
**zero decode, zero allocation, zero copies**. There is no runtime library:
everything the representation needs is known before the program starts.

## Motivation

The Weft core already moves payloads as flat byte ranges through triad rings
and fan-out slots (RFC-0001, RFC-0004). What the stack still lacks is a
*typed* view of those bytes that every language surface can share without a
codec in the hot path. The standard industry options each fail a specific
Weft law:

| Option | Failure at Weft's latency class |
|---|---|
| Protobuf / Thrift | Varint + tag decoding on every read; deserialization allocates a full object tree; P99 becomes GC/allocator territory. |
| FlatBuffers | Vtable indirection on every field; buffers valid only at their own alignment; still a runtime dependency per language. |
| Cap'n Proto | Message traversal via pointer chasing; data not naturally aligned on all hosts (x86 readers of 64-bit words at odd offsets); heavy generated API. |
| Raw C headers | Great layout, but no cross-language contract, no machine-checkable identity, silent drift when either side edits the struct. |
| JSON Schema + reflection | Orders of magnitude slower; text parsing on the hot path. |

None of them give a **single number** that two processes can compare to know
their memory views agree — that check has to be hand-rolled per project and
invariably rots. And none can emit compile-time layout pins into arbitrary
consumer builds.

The kernel is byte-frozen; this RFC touches no kernel file. weftc is a
build-time tool: `tools/weftc/`, C11 + libc only, no dependencies (Law 4),
zero panics on any input (Law 2), byte-deterministic across hosts and runs
(Law 3), and every generated layout is flat and fixed-bound (Law 1).

## Guide-level explanation

An engine declares its types once:

```weft
// frames.weft
endianness little;

enum Kind : u8 { Render = 0, Present = 1, Telemetry = 2 }

struct Header {
    seq: u64,
    kind: Kind,          // 7-byte hole follows: natural alignment
    stamp: u64,
}

@optimize(packing)
struct TelemetryMsg { flags: u8, temp: f64, pressure: f32, humidity: u8 }

struct BigFrame {
    hdr: Header,
    pos: Vec3,
    name: str[32],
    samples: [f32; 8],
    blob: span<u8>,      // { offset, len } into the ring's payload region
}
```

`weftc inspect frames.weft` prints the frozen layout as a terminal memory
map — offsets, padding holes, cacheline boundaries:

```text
struct Header — 24 B, align 8  abi_hash 0x8427797a68caa357
  #   offset  size  align  type   field
  0        0     8      8  u64    seq
  1        8     1      1  Kind   kind
  2       16      8      8  u64    stamp
  internal padding: 7 B in 1 hole
    [9, 16)  7 B
  bytes (1 char = 1 B, '.' = padding, words of 8):
  0x0000  ssssssss  k.......  tttttttt
```

`weftc compile frames.weft` emits `frames.weftir` (binary IR — the artifact
generators consume), `frames.verify.h` (the compile-time pin), and with
`--dump-ir` a JSON IR for review and tooling.

The handshake: each generated type carries `WEFT_ABI_HASH_<T>` (a 64-bit
fingerprint of its frozen layout). A C process and a TypeScript DataView
reader exchange that one constant at setup; equality means their byte views
are identical — no schema negotiation, no version parsing, 0 ns on the hot
path. The verify header additionally pins the layout inside every consumer
build:

```c
#include "frames.verify.h"   /* after the generated struct definitions */
_Static_assert(sizeof(Header) == 24, "weftc: `Header` size drifted");
_Static_assert(offsetof(Header, stamp) == 16, "weftc: offset drifted");
```

If either side edits a field, the consumer's **build breaks** — drift is
detected before any binary exists.

## Reference-level specification

### R1. Lexical grammar

Source files are UTF-8 with LF/CRLF line endings; all grammar lives in the
ASCII subset. Tokens: identifiers (`[A-Za-z_][A-Za-z0-9_]*`, ≤ 255 bytes),
keywords (`struct enum bitflags span str endianness little big packing` and
the 12 primitive names), integer literals (decimal, `0x` hex, `0b` binary,
`_` digit separators; values saturate at u64-max with WE021), punctuation
(`{ } [ ] ( ) < > : ; , = @ -`), and comments (`//` line, `/* */` block,
nested-unclosed block comment is WE022). Any byte outside the printable
ASCII set is a `WE023` invalid-character error; scanning continues.

Identifiers ≤ 255 bytes is a **hard bound** (WE036) because manifest name
fields are u16-prefixed and generated C identifiers must stay sane.

### R2. Syntactic grammar

```ebnf
schema    := ( endianness_decl | decl )* ;
endianness_decl := "endianness" ( "little" | "big" ) ";" ;
decl      := attrs ( struct_decl | enum_decl | bitflags_decl ) ";"? ;
attrs     := attr* ;
attr      := "@" "align"    "(" INT ")"
           | "@" "simd"     "(" INT ")"
           | "@" "packed"
           | "@" "optimize" "(" "packing" ")" ;
struct_decl := "struct" IDENT "{" field ( "," field )* ","? "}" ;
field     := attrs IDENT ":" type ;
enum_decl := "enum" IDENT ":" int_prim "{" variant ( "," variant )* "}" ;
bitflags_decl := "bitflags" IDENT ":" uint_prim "{" variant... "}" ;
variant   := IDENT "=" [ "-" ] INT ;
type      := prim | IDENT                       (* named reference   *)
           | "[" type ";" INT "]"               (* fixed array       *)
           | "str" "[" INT "]"                  (* fixed string      *)
           | "span" "<" type ">" ;              (* reference into a buffer *)
```

Trailing commas allowed everywhere. One `endianness` declaration per file
(WE018); default is `little`, the canonical Weft wire order. The parser is
recursive-descent with panic-mode recovery synchronizing on `,` `}` `;` and
declaration keywords, so one run reports as many real errors as exist. A
type expression may nest ≤ 64 levels (WE027); struct-struct *containment*
may nest ≤ 256 levels (WE038).

`span<T>` element may not be `span` (WE024) — a span is a reference, it
cannot nest. Fixed lengths are ≥ 1 (WE012; variable length is what
`span<T>` is for) and ≤ 2^32−1 (WE029).

### R3. Type system and canonical type strings

| Type | Size | Align | Notes |
|---|---|---|---|
| `u8 i8 u16 i16 u32 i32 u64 i64 f16 f32 f64 bool` | 1,1,2,2,4,4,8,8,2,4,8,1 | = size | `bool` is a `u8` carrying 0 or 1 |
| `str[N]` | N | 1 | inline bytes, not NUL-terminated |
| `span<T>` | 16 | 8 | `{ u64 offset, u64 len }` (WEFTC_SPAN) |
| `[T; N]` | N · stride(T) | align(T) | stride = sizeof(T) incl. trailing pad |
| named struct | computed | computed | inline value: full layout cascade |
| enum / bitflags | backing prim | backing prim | discriminant / mask word |

Canonical type strings — used in IR, manifests and diagnostics — are
`u64`, `Frame`, `[u8; 4096]`, `str[64]`, `span<u8>` exactly (space after
`;`, no spaces inside `span<...>`). They are part of the frozen IR
contract.

Forward references are legal (a schema is a set of declarations, not an
ordered dependency). Duplicate declaration names are WE005; duplicate field
names within a struct WE004; duplicate variant names WE010.

### R4. The layout engine (normative)

The engine walks fields in **final order** (declaration order, or the
`@optimize(packing)` order below), maintaining a cursor:

```
offset_i = align_up(cursor, align_i)     // hole recorded at [cursor, offset_i)
cursor   = offset_i + size_i
size     = align_up(cursor, struct_align)
trailing = size - cursor
```

All arithmetic is exact u64 with checks against the 2^48-byte total-size
ceiling (WE033) — every intermediate stays representable on 32-bit hosts.

**Natural alignment.** A field's natural alignment is its type's align
(table R3). A struct's natural alignment is the maximum of its fields'
effective alignments (1 for the empty struct, which is legal: size 0).

**Attributes** (placement errors are WE006; duplicate use on one subject
WE040; values must be powers of two in 1..4096 — WE025/WE026):

| Attribute | On struct | On field | On enum/bitflags |
|---|---|---|---|
| `@packed` | every field placed at align 1; struct align 1 | that field at align 1 | invalid |
| `@align(N)` | struct alignment **floor** (raise only) | field alignment set to N; must be ≥ natural (WE030) | alignment floor |
| `@simd(N)` | struct floor N **plus** every field whose size ≥ N is raised to align N | raise to N if field size ≥ N | invalid |
| `@optimize(packing)` | relayout fields by align ↓, decl index ↑ | invalid | invalid |

Conflicts are hard errors, not precedence rules: `@packed` struct with a
field carrying `@align`/`@simd` is WE009 (remove one — an aligned member of
a packed struct is a self-contradiction), as is `@packed @align` on one
field. `@packed` + `@optimize(packing)` is legal but the reorder is the
identity — WW001 warns. The rule "alignment can only be raised" (WE030)
keeps every field dereferenceable by natural-width loads on strict hosts.

**`@optimize(packing)`** sorts by effective alignment descending, ties by
declaration index ascending — a *pure function of the source*, so the
resulting layout is as frozen as any other. `reordered` is recorded in the
IR, and each field keeps its `orig_index`. The classic case:

```weft
// declared order: 24 B (7-byte hole + trailing pad)
struct A { flags: u8, temp: f64, pressure: f32, humidity: u8 }
// @optimize(packing): 16 B — the hint weftc prints for the above
```

Unoptimized structs carry an `optimize_hint` in the IR (0 = no improvement
available). Codegen may not apply it silently: applying the reorder changes
the ABI hash — it is a schema edit, done in the source.

**`span<T>` is a reference, not a containment edge.** `struct Node { next:
span<Node> }` is legal and finite (16 B); `struct Node { next: Node }` is
WE008 (recursive value type). This is what makes linked topologies —
chains, trees, DAGs over a ring — expressible without unbounded types.

**Enums and bitflags.** Backing must be an integer primitive (WE017); for
bitflags it must be unsigned (WE016). Variant values must fit the backing
(WE013; `i8` accepts −128..127, `u8` 0..255, `u64` the full unsigned range
via two's-complement re-interpretation of the i64 literal). Enum
discriminants must be unique (WE011) — they are control-flow values.
Bitflags values are arbitrary masks: aliasing is *allowed* and warned
(WW002), because `COMBINED = A | B` is a legitimate declaration. An enum
with zero variants is WE014. Variant *values* are ABI (they change
`abi_hash`); variant *names* are identity only (they change `schema_id`).

**Determinism.** No host state enters the computation: same input bytes →
same offsets, sizes, holes, hashes, IR bytes, on every conforming C11 host,
32- or 64-bit, little- or big-endian (all manifest/IR integers are
explicitly little-endian; layout math is endian-free).

### R5. Schema fingerprints

Three hashes, one algorithm:

| Hash | Domain | Content | Flips on | Stable under |
|---|---|---|---|---|
| `abi_hash` (per decl) | `WDC1` | one decl's **effects**: kind, packed/reordered flags, size, align, field offsets/sizes/aligns + canonical type strings (+ orig_index when reordered), holes; enum backing + values | any layout change, endianness | field/variant renames, decl reordering, comment edits |
| `abi_hash` (whole schema) | `WAB1` | header + all decl records in declaration order | any decl's change, decl add/remove/reorder | renames |
| `schema_id` (whole schema) | `WID1` | abi manifest + decl names + field names + variant names + declared attribute values | everything above **and** any rename or attr spelling | comment/whitespace edits |

The ABI manifest is **effect-only**: attribute spellings that do not change
layout (e.g. `@align(1)` on an all-`u8` struct) leave wire hashes unchanged
— the RFC's answer to "is a no-op annotation a breaking change?" (no). The
ID manifest records declared intent, so the same no-op flips `schema_id`:
tooling that cares about authorship diffs the ID, tooling that cares about
wire compat diffs the ABI. `fnv1a64` of the abi manifest is carried as an
independent second fingerprint — a cross-implementation check that catches
producer bugs (not collisions).

Manifests are length-prefixed binary, all little-endian: `u16` + bytes for
names/type strings, `u64` for sizes/offsets/aligns/values, `u32` for
counts, `u8` for kinds/flags/prim codes. Kind codes: struct 0, enum 1,
bitflags 2; prim codes are the `PrimKind` enum values 1..12. The three
magic domains (`WAB1`/`WID1`/`WDC1`) make cross-domain hash equality
impossible by construction.

**WH64** — the hash function. Portable wyhash-style mixer over 64-bit
little-endian chunks:

```c
static uint64_t mix64(uint64_t z) {           /* splitmix64 finalizer */
    z ^= z >> 30; z *= 0xbf58476d1ce4e5b9ull;
    z ^= z >> 27; z *= 0x94d049bb133111ebull;
    z ^= z >> 31; return z;
}
uint64_t wh64(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ mix64((uint64_t)n);
    size_t i = 0;
    for (; n - i >= 8; i += 8)
        h = mix64(h + le64(p + i) + 0x165667B19E3779F9ull);
    if (i < n) {                              /* tail: 1..7 bytes, LE fold */
        uint64_t t = 0;
        for (size_t j = i; j < n; j++) t |= (uint64_t)p[j] << (8 * (j - i));
        h = mix64(h ^ (t + 0xC2B2AE3D27D4EB4Full));
    }
    return mix64(h);
}
```

Properties (all tested, §R8): add-then-mix chaining makes whole-chunk
transpositions collide only by accident (WH4); length is mixed at both
ends (empty ≠ 8 zero bytes); avalanche measured at 48–52 % per output bit
(WH3); zero collisions over a 1M-input deterministic corpus (WH5).
Reference vectors are pinned in `tests/test_hash.c` and regenerable via
`tests/gen_vectors.c`.

**Stability contract.** WH64 and the manifest encodings are frozen at IR
version 1. Any change to either bumps `WEFTC_IR_VERSION` and thereby
invalidates every published hash — deliberately loud, so silent drift of a
wire contract is structurally impossible. 64 bits means a birthday
collision becomes likely around ~2^32 distinct schemas (~4 billion); a
project shipping more schemas than that should gate on `abi_hash` AND
`fnv1a64` together (independent algorithms, joint space 2^128).

### R6. Artifacts

**JSON IR** (`--dump-ir` / `--emit-json`): fixed key order, 2-space indent,
hashes as `"0x%016llx"` strings, LF newlines, no timestamps, no locale
dependence. Top level: `weftc_version, ir_version, path, endianness,
abi_hash, schema_id, fnv1a64_abi, decl_count, decls[]`. Struct decls carry
`size, align, abi_hash, packed, reordered, align_attr, simd_attr,
internal_pad, trailing_pad, optimize_hint, fields[{name, type, offset,
size, align, index}], holes[{offset, size}]`; enum decls carry `backing,
variants[{name, value}]`.

**Binary IR** (`WIR1`): magic `WIR1`, `u8 ir_version`, `u8 endian`, three
`u64` hashes, `str path`, `u32 ndecls`, then per decl: `u8 kind`, `str
name`, and the layout records mirroring the JSON (field: `str name, str
type, u64 offset/size/align, u32 orig_index`; hole: `u64 offset, size`;
enum variant: `str name, i64 value`). All integers little-endian. This is
the machine-facing artifact for the Engineer-2/3 generators.

**Verify header** (`--emit-header`, default on `compile`): `#pragma once`,
C11/C++11-portable `WEFT_STATIC_ASSERT`, `WEFT_SCHEMA_ID/WEFT_ABI_HASH/
WEFT_FNV1A64_ABI/WEFT_ENDIANNESS_BIG` macros, then per decl
`WEFT_ABI_HASH_<T>` plus `sizeof` and per-field `offsetof` assertions.
Included *after* the generated definitions; drift = build error at the
consumer. Names are used verbatim — the grammar admits only C-identifier-
safe names, and C-keyword collisions are the codegen layer's mapping
concern.

**inspect** (`weftc inspect`): the memory map shown in the guide section —
field table in final offset order (source `#` column), half-open hole
ranges, trailing pad, one-character-per-byte map (letters from field names,
`.` padding, 8-byte word groups, 32-byte rows, `; cacheline N` markers at
64-byte boundaries, elision above 256 B), and the `@optimize(packing)`
savings hint.

### R7. CLI

```
weftc check   <file.weft>              validate; summary on stdout
weftc inspect <file.weft>              validate + memory map on stdout
weftc compile <file.weft> [flags]      artifacts:
    -o/--out PREFIX      (default: input stem) -> PREFIX.weftir, PREFIX.verify.h
    --dump-ir            JSON IR to stdout
    --emit-json/-ir/-header FILE|-
    --color=auto|always|never   --werror   --version   --help
```

Diagnostics render GCC/Clang-style (`path:line:col: error[CODE]: msg`,
source line, caret row, secondary spans, notes/help) to **stderr**;
artifacts and summaries to **stdout**. `NO_COLOR` honored. Exit codes:
0 clean, 1 diagnostics (warnings too under `--werror`), 2 usage/fatal.
Artifacts are written only for error-free compilations.

### R8. Test gates

`tools/weftc/tests/run.sh` — one command, non-zero exit on any failure:

- **WL-series** (layout): primitive geometry; natural alignment and holes;
  nested cascades; array stride includes trailing pad; `@packed`, field
  `@packed`, `@align`, `@simd`, `@optimize` (result + preserved
  `orig_index` + hint on the unoptimized twin); empty structs; 30-case
  semantic error battery (one code each); endianness; 32-case
  malformed-input armor; forward references; `span` breaking cycles; the
  300-deep containment bound.
- **WH-series** (hash): pinned vectors; FNV external anchors; avalanche
  (64 output bits, 24 576 trials each); order/length sensitivity; 1M-input
  collision freedom; the mutation-sensitivity matrix including the
  rename-stability split; determinism and comment-immunity; fingerprint
  independence.
- **WD-series** (artifacts): byte-determinism of all four emitters; binary
  IR parse-back; verify-header content; inspect sanity; **golden
  byte-compare** of JSON / binary IR / verify header / inspect against
  `tests/golden/` (any accidental manifest or formatting change breaks the
  build — regeneration is a deliberate, IR-version-bumping act).
- **CLI gates**: exit codes (0/1/2), artifact-golden byte-identity through
  the real CLI path, JSON validated by an external parser (python3).
- **Handshake demo**: `tests/golden/verify_consumer.c` — hand-written C11
  structs per the IR — must compile clean against the generated verify
  header (positive), and a field-swapped drift variant must FAIL to
  compile with a `weftc: ... drifted` assertion (negative).
- **ASAN leg**: the entire gate re-runs with every binary built under
  `-fsanitize=address` (Law 2 memory-safety audit; zero findings).

Invariants touched: none of I1–I8 — the kernel, its wire format, and all
frozen files are untouched (this is a new build-time component). New laws
stated for this component: L1 no hidden allocations (flat, fixed-bound
layouts); L2 zero compiler panics (arena allocation, bounded recursion,
panic-mode diagnostics, ASAN-clean); L3 architectural parity (explicit LE
serialization, host-independent math, 32-bit-safe intermediates); L4 no
bloated dependencies (C11 + libc, ~5 000 lines total).

Litmus impact: none of L1–L16 changes; this introduces the WL/WH/WD
namespace. Envelope impact: none — no kernel, package, or CI surface is
modified by the core RFC; the CI shard addition rides the existing
extreme-test matrix pattern.

### R9. Diagnostic registry (complete)

| Code | Sev | Meaning |
|---|---|---|
| WE001 | err | unknown attribute |
| WE002 | err | attribute requires an argument |
| WE003 | err | attribute takes no argument |
| WE004 | err | duplicate field name |
| WE005 | err | duplicate declaration name |
| WE006 | err | attribute not valid in this position |
| WE007 | err | unknown type (with did-you-mean) |
| WE008 | err | recursive value type (containment cycle) |
| WE009 | err | contradictory alignment request (@packed vs @align/@simd) |
| WE010 | err | duplicate variant name |
| WE011 | err | duplicate enum discriminant |
| WE012 | err | fixed length must be ≥ 1 |
| WE013 | err | variant value does not fit the backing type |
| WE014 | err | enum/bitflags with no variants |
| WE016 | err | bitflags backing must be unsigned |
| WE017 | err | backing must be an integer primitive |
| WE018 | err | endianness redeclared |
| WE019 | err | expected X, found Y |
| WE020 | err | unexpected token |
| WE021 | err | integer literal out of range |
| WE022 | err | unterminated block comment |
| WE023 | err | invalid character |
| WE024 | err | span element must not be span |
| WE025 | err | @align/@simd value not a power of two |
| WE026 | err | @align/@simd value exceeds 4096 |
| WE027 | err | type expression nesting > 64 |
| WE028 | err | endianness must be little or big |
| WE029 | err | fixed length exceeds u32 |
| WE030 | err | field @align below natural alignment |
| WE031 | err | bitflags value negative |
| WE032 | err | bad attribute argument |
| WE033 | err | type exceeds the 2^48-byte limit |
| WE036 | err | identifier exceeds 255 bytes |
| WE037 | err | more than 2^32−16 declarations |
| WE038 | err | struct containment > 256 levels |
| WE039 | err | cannot open input file |
| WE040 | err | duplicate attribute on one subject |
| WE041 | err | base prefix with no digits |
| WW001 | warn | @packed + @optimize(packing) is the identity |
| WW002 | warn | bitflags value aliases an earlier variant |
| WW003 | warn | (reserved) |

## Boundary of the claim (Law 4)

weftc computes and pins memory layout; it does not serialize, encode, or
move bytes. Variable-length data is out of scope by design — it lives in
the ring payload region and is addressed by `span<T>`; callers enforce
their own bounds. The 64-bit fingerprints are collision-*resistant*
engineering hashes, not cryptographic signatures — a hostile party can
craft a colliding schema; threat models requiring authenticity need
VerifiedWeft (RFC-0005), not a faster hash. Big-endian hosts are *declared
for* (explicit LE serialization, tested by construction) but not *tested
on* (no BE hardware in CI) — the BE leg is a declared follow-up. Code
generation (C/Rust/WGSL/TS/Dart/Swift/Python) is explicitly the
Engineering-2/3 layer: this RFC freezes the IR they consume and the hashes
they embed; it ships no generator. Schema evolution (field deprecation,
forward/backward compat) is deliberately absent: a schema is a frozen
contract; evolution = a new `schema_id`, checked at the handshake.

## Alternatives considered

**Adopt FlatBuffers/Cap'n Proto** — loses natural alignment everywhere
(their readers tolerate misaligned 64-bit words; Weft targets strict hosts
and GPU uniform buffers whose alignment rules are non-negotiable), keeps a
per-language runtime dependency, and offers no compile-time drift pins.
**Adopt protobuf with a fast path** — decode cost and allocation behavior
are the problem being solved; a faster varint is still a varint. **C header
as single source of truth** — no TS/Dart/Swift/WGSL story, no machine
identity, silent drift (the exact failure the verify header exists to
prevent). **JSON Schema + codegen** — layout not expressible; alignment
and endianness absent. **Do nothing** — every surface hand-rolls struct
mirrors; the SHM mesh (RFC-0011) already needs a shared, checkable type
identity; that need grows with each port.

## Drawbacks

Another IDL in a world dense with them — justified only by the
zero-decode/zero-dependency/compile-time-pin combination, which no
incumbent provides. The fixed-length constraint pushes complexity onto
buffer design (spans must be planned against ring geometry). The
alignment-attribute surface (`@packed/@align/@simd`) imports C-isms into a
language that also targets GC runtimes — the attributes are advisory
there, and the IR records them so generators can decide. 64-bit hashes
invite over-trust; the RFC responds with the documented birthday bound and
the dual-fingerprint option rather than pretending 2^64 is infinity.

## Open questions

1. Generator contracts (Engineer 2: C11/Rust/WGSL; Engineer 3: TS
   DataView/Dart/Swift/Python) — exact naming, module layout, and
   `span<T>` bounds-check policies per surface.
2. WGSL uniform-buffer alignment (std140-style 16-byte vec rules) — likely
   a `@uniform` layout strategy on top of `@simd(16)`; needs GPU-backend
   evidence before standardization.
3. Schema composition (`use`/import across files) — deferred until two
   real schemas want to share types.
4. Hash upgrade path if WH64 ever needs replacement (IR version bump is
   defined; the migration tooling is not).
