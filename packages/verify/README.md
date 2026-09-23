# @weft/verify

Weft developer verification suite — the ergonomic front door to the Weft
zero-copy ecosystem's correctness machinery. Pure TypeScript, **zero runtime
dependencies** (`dependencies: {}`), Node 22+ ESM, erasable-syntax-only
sources runnable directly via `node` / `npx @weft/verify`.

## Command surface

```
weft verify --all [--states N] [--seed N] [--out DIR]   full pipeline + scorecard
weft verify --lint-alloc [paths...]                      cross-language zero-alloc scanner
weft verify --formal [--states N] [--deadline-ms MS]     formal model checks + state tally
weft verify --chaos [thermal|bus|network|all]            synthetic silicon chaos suites
weft verify --report html|json [--input FILE]            verification scorecard
weft verify --help | --version
```

Exit codes: `0` verified, `1` verification findings/failures, `2` usage error.
All commands are deterministic and fail-closed.

## What it checks

- **Zero-allocation hot paths** — `--lint-alloc` scans `@hot` /
  `__attribute__((weft_hot))` / `#[weft_hot]` / `@weft_hot` / `@weftHot`
  annotated functions across TypeScript, C, C++, Rust, Swift and Dart and
  rejects heap allocation primitives (malloc, new, closures, heap boxing…)
  with caret diagnostics.
- **Formal protocol proofs** — exhaustive bounded model checks of the triad
  slot-handoff and seqlock-parity publication disciplines (single-writer
  exclusivity, writer/reader slot exclusion, deadlock freedom, bounded
  progress, parity-gated publication) plus a >= 1e7 state-space exploration
  tally on a wide two-producer ring.
- **Synthetic silicon chaos** — deterministic thermal throttling
  (3.2 GHz -> 800 MHz), memory-bus saturation and network packet-drop /
  split-brain recovery simulations with a cluster resilience score.
- **Scorecards** — self-contained dark-theme HTML + schema-versioned JSON.

## Governance hooks

`tools/verify/hooks/pre-commit-alloc-lint.sh` (<= 50 ms, POSIX sh + awk)
blocks commits that introduce heap allocation in hot-path code;
`install.sh` wires it into `.git/hooks`.

## Boundary law

This package never touches `core/c/` (Engineers 1/2 territory). It consumes
exposed seams only.
