# weftc-lint — Weft Pillar 8 static allocation linter

`weftc --lint-alloc` proves Law 1 (zero heap allocation on the hot plane) at
compile time. It is the first module of the `weftc` compiler toolchain.

## What it flags (inside functions tagged hot)

Hot annotations recognized:

| Language   | Annotation                                             |
|------------|--------------------------------------------------------|
| C          | `/* @hot */`, `__attribute__((weft_hot))`              |
| C++        | `[[weft_hot]]`, `[[clang::annotate("weft_hot")]]`, `__attribute__((weft_hot))`, `/* @hot */` |
| Rust       | `#[weft_hot]`, `#[hot]`                                 |
| TypeScript | `@hot` decorators                                       |
| Swift      | `@WeftHot`, `@weft_hot`                                 |
| Dart       | `@hot`, `@weft_hot`                                     |

Rules (severity):

- `alloc-heap` (error) — `malloc`, `calloc`, `realloc`, `free`,
  `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`,
  Rust `Box::new`, `vec!`
- `alloc-string` (error) — `strdup`, `strndup`, Rust `String::from`
- `alloc-fmt` (error) — `asprintf`, `vasprintf`, Rust `format!`
- `alloc-mmap` (error) — `mmap`, `mmap64`, `sbrk`
- `alloc-new` (error) — C++/TS `new` / `delete`
- `alloc-concat` (error) — managed-language dynamic string concatenation
  (`+`/`+=` against literals), template `${...}`, Swift `\(...)`, Dart
  `$var` interpolation
- `alloc-recursion` (error) — self-recursion and mutually recursive cycles
  that include a hot function
- `alloc-rust-std` (warning) — `.to_string()`, `.to_owned()`, `.clone()`,
  `.collect()`, `.push()`, `Vec::new`, `println!`

## Usage

```
weftc-lint [--lint-alloc] [--lang=c|cpp|rust|ts|swift|dart|auto]
           [--format=gcc|json] [--bench=N] [--rounds=N]
           [--no-strict] [--version] FILE...
```

Exit codes (fail-closed): `0` clean, `1` diagnostics found, `2` usage/IO
error. Output is GCC/Clang diagnostic format:

```
src/parse.c:41:17: error: call to 'malloc' in @hot function 'parse_frame' [alloc-heap]
src/parse.c:41:17: note: Law 1: preallocate from a stack buffer or an arena slot; the hot plane never touches the heap allocator
```

## Performance SLA

The SCAN pass is a single zero-allocation walk over a flat token arena:
**> 100,000 AST nodes in < 15 ms** (measured 431,775 nodes in ~1.5 ms ≈
3.3 ns/node on the 2-core CI box — see `--bench=150000`). The scan is
proven zero-allocation by the G4 `--wrap=malloc` probe.

## Engine API

See `weftc_lint_alloc.h` (frozen ABI v1.0):
`weftc_lint_parse()` (setup, allocation allowed through the caller's arena
hook) then `weftc_lint_scan_ast()` (zero-allocation analysis pass).

Known heuristic boundaries and the false-positive ledger are documented in
`docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md` §5.
