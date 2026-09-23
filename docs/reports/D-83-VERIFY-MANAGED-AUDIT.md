# D-83 — VERIFY-MANAGED AUDIT REPORT

**Directive:** WEFT-DIRECTIVE-PILLAR-8-ENG3 (Principal Architect → Senior Engineer 3)
**Pillar:** 8 — weft-verify (Developer CLI, Git Pre-commit Hooks & Verification Scorecard Dashboard)
**Date:** 2026-09-22
**Status:** DELIVERED — ALL 7 STAGES GREEN, exit 0
**Branch:** `feat/verify-managed` (off `origin/main` @ 7391fee5)
**Deliverable:** `weftc-pillar8-managed.zip` (+ `.sha256`)

---

## 1. Executive Summary

weft-verify connects Engineer 1's formal verification machinery and Engineer 2's
synthetic silicon lab into an ergonomic, zero-friction developer verification
suite: one CLI (`weft verify`), a cross-language zero-allocation linter, an
embedded formal model checker with a >= 1e7 state-space tally, deterministic
synthetic chaos suites, dark-theme HTML/JSON scorecards, and a sub-5-millisecond
git pre-commit gate. The package is pure TypeScript with **zero runtime
dependencies**, **zero third-party imports** (only `node:` builtins), and a
**99 KiB payload** against the 100 KiB budget. The engine dogfoods its own
linter: the package's own `@hot` probe source is part of the clean-scan gate.

Every number in this report is reproduced by
`tools/verify/tests/run_verify_managed_suite.sh` (fail-closed, exit 0,
~9 s wall clock on the development sandbox).

## 2. Mandate Compliance Matrix

| # | Mandate | Deliverable | Evidence | Status |
|---|---------|-------------|----------|--------|
| A1 | `packages/verify/` pure TS, zero runtime deps, Node 22+ ESM, `npx @weft/verify` | `package.json` (`dependencies:{}`, `peerDependencies:{}`, `devDependencies:{}`), `bin/weft-verify.mjs` launcher (probes `--experimental-strip-types` for Node 22.6–22.x, native strip on 23.6+) | Stage 7 dependency audit: `{d:{},pe:{},dev:{}}`; import audit: only `node:` + relative `.ts` | ✅ |
| A2 | `weft verify --all` | Full pipeline: formal proofs + alloc linter + chaos + scorecard.{json,html} + formal.json | Stage 4 case 15–16: `--all --states 200000` writes all three artifacts, verdict PASS | ✅ |
| A2 | `weft verify --lint-alloc [files...]` | Cross-language scanner: TypeScript/JS, C, C++, Rust, Swift, Dart | Stage 2: 31/31 poisoned primitives flagged, precision 1.0, recall 1.0 | ✅ |
| A2 | `weft verify --formal` | Exhaustive triad-handoff + seqlock-parity models + >= 1e7 wide tally | Stage 5: 10,000,000 distinct states in 2.5 s; 9/9 theorems PROVED | ✅ |
| A2 | `weft verify --chaos [profile]` | thermal / bus / network / all, deterministic, `--bench` real-time overlay | Stage 4: profile JSON shapes validated; resilience 90/100 | ✅ |
| A2 | `weft verify --report [html\|json]` | Self-contained dark-theme HTML + schema-versioned JSON, byte-deterministic | Stage 5: two-run sha256 equality (JSON + HTML) | ✅ |
| B1 | `pre-commit-alloc-lint.sh` < 50 ms, staged-file inspection, hot annotations, rejection with diagnostic pointers | POSIX sh + awk (no Node startup), rule ids mirror the engine | Stage 6: poisoned commit REJECTED (WV-C-001 pointer), clean PASSED, best-of-3 **4 ms** | ✅ |
| B1 | TS / Rust / C / Swift / Dart (+ C++) coverage | 6 language grammars in hook + engine | fixtures ×12 across all languages | ✅ |
| C1 | Scorecard: Formal Theorems HUD | 9 theorem cards + evidence basis, section id `formal-hud` | HTML markers checked; schema validator (stage5) | ✅ |
| C1 | Allocation Audit Matrix | per-file hot-function/findings rows, 0B compliance chips, id `alloc-matrix` | self-scan of 15 package sources: 0 findings, compliant | ✅ |
| C1 | Synthetic Silicon Performance Curves | thermal 3.2 GHz→800 MHz + memory-bus saturation SVG curves, id `silicon-curves` | 7-point thermal curve; bus curve to 95% utilization | ✅ |
| C1 | Cluster Resilience Score | packet-drop tolerance + split-brain recovery breakdown, id `resilience` | resilience 90/100 (delivery 100%@5% drop, recovery component 50/100) | ✅ |
| C1 | Zero external runtime deps (report itself) | inline CSS + inline SVG; no JS/CSS assets, no fonts, no CDN | HTML is one self-contained string | ✅ |
| D | 7-stage fail-closed suite at `tools/verify/tests/run_verify_managed_suite.sh` | stages 1–7 as specified below | ALL GREEN, exit 0, ~9 s | ✅ |
| E | `docs/reports/D-83-VERIFY-MANAGED-AUDIT.md` | this document | — | ✅ |
| — | Boundary isolation (`packages/verify`, `tools/verify`, `tests/verify/managed`, `docs/reports/D-83-*`) | `git status` scoped to territory | Stage 1 boundary law gate | ✅ |
| — | Zero modifications to `core/c/` | no payload path under `core/`; zero `core/c/` references in verify code | Stage 1: 0 hits | ✅ |

## 3. Seven-Stage Test Evidence (final run, exit 0)

```
[STAGE 1/7] subsystem integrity, file manifest, boundary law
  [ok] manifest: 41/41 required files present
  [ok] boundary law: zero core/c/ references, zero core/ payload paths
  [ok] charset integrity: payload free of the filtered byte pair
  [ok] CLI runs with zero warnings
  [ok] hook scripts are executable
[STAGE 2/7] AST allocation linter — cross-language precision & recall
  [ok] poisoned set exit 1 as required; clean set exit 0
  [ok] precision 1.0 / recall 1.0 — 31 flagged == 31 expected, 0 clean findings
[STAGE 3/7] zero-allocation runtime execution probe (1,000,000 iterations)
  [ok] heap growth -0.4 KiB <= 64 KiB gate (min-of-3 GC rounds: -0.4/-0.4/+0.1)
  [ok] negative control (allocating loop) blows the gate — exit 1
  [ok] dogfood: the engine's own @hot probe source lints clean
[STAGE 4/7] CLI command suite & flag parser verification
  [ok] 16/16 cases: help/version/exit-code matrix/chaos/formal/report/--all artifacts
[STAGE 5/7] scorecard determinism + schema + >= 1e7 state-space tally
  [ok] state-space tally: 10000000 distinct states (>= 1e7) in 2.5 s
  [ok] determinism: JSON + HTML byte-identical across runs
       (sha256 5b040c16e0c2ad48653177d0f668e7e3e6342c06903478e5b2d1535dd4d36559)
  [ok] network chaos determinism (seeded); scorecard schema validation
[STAGE 6/7] git hook integration
  [ok] poisoned commit REJECTED with diagnostic pointer (WV-C-001)
  [ok] clean commit PASSED
  [ok] hook latency best-of-3: 4 ms < 50 ms budget
  [ok] non-source staged files pass instantly
[STAGE 7/7] package payload & zero-runtime-dependency audit
  [ok] dependencies {} / peerDependencies {} / devDependencies {}
  [ok] payload: 101578 bytes (99 KiB) < 100 KiB
  [ok] import audit: only node: builtins and relative .ts modules
ALL 7 STAGES GREEN — weft-verify managed suite (exit 0) — wall clock 9 s
```

## 4. Formal Verification Summary

| Model | Kind | States | Transitions | Max depth | Deadlocks | Verdict |
|-------|------|--------|-------------|-----------|-----------|---------|
| triad-handoff (3 slots, ver mod 4) | exhaustive BFS + invariants + bounded liveness | 6,336 | 17,280 | 44 | 0 | PROVED |
| seqlock-parity (3 slots, ver mod 8) | exhaustive BFS + invariants + bounded liveness | 5,760 | 11,520 | 62 | 0 | PROVED |
| wide two-producer seqlock ring (4 slots, ver mod 256) | BFS state-space tally, exact 64-bit keys, 2^24 open-addressing table | 10,000,000 (stop-at-target) | 29.9M | — | — | tally ≥ 1e7 ✅ (2.5 s, load factor 0.596) |

Theorems HUD (all PROVED): TH-01 single-writer exclusivity · TH-02
writer/reader slot exclusion · TH-03 deadlock freedom (0 deadlock states
across both models) · TH-04 bounded commit progress ≤ 10 (max 3) · TH-05
bounded read progress ≤ 10 (max 3) · TH-06 parity-gated publication · TH-07
bounded clean-read ≤ 8 (max 6; worst-case derivation = torn latch + writer
lap + relatch = 7) · TH-08 bounded writer progress ≤ 3 (max 0 — writer never
blocks in the seqlock model) · TH-09 state-space tally ≥ 1e7.

The reference TLA+ module (`packages/verify/formal/TriadBuffer.tla`) maps every
spec invariant to the explorer's checked property (see §8 Honesty Ledger).

## 5. CLI Performance Benchmarks (development sandbox, 2 vCPU)

| Command | Wall clock | Notes |
|---------|-----------|-------|
| `weft verify --version` | ~45 ms | includes Node startup + type-stripping |
| `weft verify --lint-alloc packages/verify/src` (15 files) | ~80 ms | full mask/semi-mask/scan pipeline |
| `weft verify --formal` (full default: 2 exhaustive models + 1e7 tally) | ~3.0 s | 10,000,000 states @ ~12 M states/s effective |
| `weft verify --chaos all` | ~0.5 s | 60k deterministic sim ticks + pure-function curves |
| `weft verify --report json --input formal.json` | ~1.5 s | lint self-scan + chaos + render |
| `weft verify --all --states 200000` | ~1.5 s | full pipeline, bounded tally |
| pre-commit hook (staged clean source set) | **4 ms** best-of-3 | pure sh+awk, no Node |

## 6. Bug Ledger (found & fixed during this pillar)

| # | Bug | Root cause | Fix | Gate that caught it |
|---|-----|------------|-----|---------------------|
| 1 | seqlock clean-read liveness FAILED at bound 5 | genuine model property: worst case = torn latch (1) + writer lap (3) + relatch/check/consume (3) = 7 steps | bound 8 with documented derivation in code + D-83 | Stage 2-of-formal (progress DP) |
| 2 | `@hot` inside a prose comment opened a hot region on the next function (triad.ts constructor FPs) | annotation detection lacked position validation | two-sided validation: prefix must be comment/decorator shape; suffix must be comment-closer or a signature-starter word | Stage 7 self-lint (dogfood) |
| 3 | Self-lint still flagged triad.ts after fix #2 | `scan.ts` did not forward `lang.prefixRe` to `findHotRegions` | forward it | Stage 7 self-lint |
| 4 | Probe threw "unreachable" on negative checksum | anti-DCE guard compared int checksum < 0 | `Number.isFinite` guard | Stage 3 smoke |
| 5 | CLI crashed on import (`DEFAULT_SEED` not exported) | missing re-export from chaos index | re-export | Stage 4 |
| 6 | `--json` flag rejected as unknown | parseArgs had no `--json` case despite documented surface | added case + field | Stage 4 |
| 7 | Stage 3 runner ENOENT | wrong relative import depth (`../../` vs `../../../`) | fixed path | Stage 3 |
| 8 | Suite aborted on the *expected* poisoned-commit rejection | ERR trap fired on the intentionally-failing git commit | explicit rc capture (`|| POISON_RC=$?`) | Stage 6 |
| 9 | Suite aborted on benign empty greps | `pipefail` + grep exit 1 on no matches | guarded pipelines | Stage 1/7 |
| 10 | Hook: `Syntax error: EOF in backquote substitution` | literal `'` and backtick inside sh-single-quoted awk program | octal escapes (`\047`, `\140`) | `sh -n` + Stage 6 |
| 11 | Hook: mawk lacks `\b` word boundaries | GNU-regex assumption | POSIX-safe context patterns | Stage 6 |
| 12 | Engine missed dart `calloc<Float>(64)` form | rule used `\s*\(` not `\s*[<(]` | aligned with hook pattern | Stage 2 recall check |
| 13 | Payload 111,243 bytes > 100 KiB | verbose comment prose | conservative comment diet (full-line `//` only; JSDoc contracts kept): −9,665 bytes → 101,578 | Stage 7 audit |
| 14 | ENVIRONMENTAL: sandbox filesystem filter silently drops the open-bracket + lowercase-h byte pair in file writes (both tool and shell paths), corrupting code like `blocks[head]` | sandbox write filter, not our code | zero-`[h` authoring policy + Stage 1 charset-integrity gate (`\133h` scan) + tool-view rewrites for convergence | Stage 1 |
| 15 | Stage 2 findings capture used unsupported `--out` | invented flag | stdout redirect | Stage 2 |

## 7. Cross-Platform Compatibility Notes

- **Node:** developed on 24.21 (native type stripping). The bin launcher probes
  `process.features.typescript` and falls back to
  `--experimental-strip-types` (Node 22.6–22.x); `engines.node >= 22.6`.
  Sources are erasable-syntax-only (no enums/namespaces/param-properties).
- **awk (hook):** verified with mawk 1.3.4. No GNU-only regex (`\<`, `\>`),
  no `match()` 3-arg form, no `gensub`. Compatible with gawk/BSD awk.
- **shell:** hooks are POSIX sh (dash-compatible); the suite requires bash
  (arrays-free but uses `pipefail`, `[[`-free patterns); `date +%s%N` for
  latency (GNU coreutils; macOS BSD date lacks %N — suite is a Linux CI lane).
- **git:** ≥ 2.28 for `git init -b`.
- **Windows:** not tested (WSL works in principle; hook uses `git diff --cached`
  and POSIX awk only).

## 8. Honesty Ledger

1. **The linter is "AST-lite", not a full parser.** It is a lexical scanner:
   comment/string masking (escape- and raw-string-aware, nested block comments
   where the language allows), brace-tracked hot regions, per-line rule
   matching. Consequences: (a) allocations inside masked template-literal
   interpolations are invisible; (b) rules are line-local — a primitive split
   across lines (e.g. `new` on the line, constructor on the next) is missed;
   (c) precision is defined against the published rule matrix, which the
   fixture suite pins 1:1. A true AST parser is the natural v2 (would require
   a parser dependency, which the zero-dependency mandate forbids today).
2. **The pre-commit hook is a fast approximation** of the engine rule matrix:
   it reports at most one diagnostic per rule per line and uses a slightly
   reduced pattern set for < 50 ms latency. The engine (`--lint-alloc`) is the
   source of truth; the hook is the friction gate. Bypass exists
   (`--no-verify`) and is documented in the hook banner.
3. **The TLA+ module is a reference contract, not an executed artifact.**
   `weft verify --formal` runs the embedded exhaustive explorer (no TLC/Java
   dependency). The .tla file maps each spec invariant to the explorer's
   checked property so an independent TLC run can reproduce the safety claims.
4. **Liveness is existential-bounded, not strong-fair unbounded.** From every
   reachable state, a commit is performable within 10 steps, a clean read
   within 8, a writer step within 3 — proven by layered backward propagation
   over the full reachable graph. Unbounded liveness under strong fairness
   needs an SCC-based checker and is deferred (TLC lane).
5. **The 1e7 tally is stop-at-target, not model exhaustion.** The wide model's
   reachable space is much larger than 1e7; exploration stops at exactly
   10,000,000 distinct states (truncated=true recorded). The SMALL models are
   explored to exhaustion (proof by closure). Deduplication is exact: full
   state keys are stored (2^24 open-addressing table of float64-exact 50-bit
   keys), not hash-only fingerprints.
6. **Chaos suites are deterministic synthetic simulations** — the managed twin
   of Engineer 2's real silicon lab, not a hardware harness. Thermal ns/op =
   documented 42-cycle constant ÷ frequency; bus = M/M/1-style queueing curve
   with ±2% seeded jitter; network = 3-node ring with Bernoulli drops
   (seeded mulberry32) and a store-and-forward split-brain episode (recovery
   ≈ 2,003–2,102 ticks for a 4,000-packet backlog). `--bench` attaches a real
   wall-clock overlay for thermal, explicitly excluded from deterministic
   reports.
7. **Determinism is engineered, not incidental:** no timestamps, no Math.random,
   sorted rows, fixed-decimal rounding, formal wall-clock stripped when
   embedding into the scorecard. Two runs are byte-identical (sha256-gated in
   Stage 5).
8. **Hook latency is reported best-of-3** (< 50 ms budget); single-shot runs
   in the same test were ≤ 10 ms on this sandbox. Best-of-3 suppresses
   cold-cache noise, mirroring the Pillar 6 measurement policy.
9. **Resilience score formula is documented, not magic:**
   0.5 × delivery@1% + 0.3 × delivery@5% + 0.2 × recovery, where recovery =
   clamp(0, 100, 100 − recoveryTicks/40). Pass gate ≥ 85 (achieved 90).
10. **Boundary-law scope:** Stage 1 proves the verify payload contains zero
    `core/c/` references and zero `core/` paths. Consistent with the standing
    managed-suite constraint, it deliberately does NOT assert that Engineer
    1/2's `core/c/` worktree is clean relative to any ref — only that this
    pillar's payload never references or writes it.
11. **One-file purge policy (env):** the sandbox write filter (bug ledger #14)
    forced a zero-`[h` authoring policy. Stage 1's charset gate permanently
    guards the payload so any future regression fails closed.
12. **Deferred lanes:** npm registry publish (`npx @weft/verify` works via the
    committed bin launcher once published; offline verification used direct
    node invocation); Windows hook validation; SCC liveness; true-AST parser;
    JSON-schema formal publication of the scorecard schema (currently a
    validator implementation, not a .json schema file).

## 9. Score

**98/100** — deductions: synthetic (not hardware) silicon curves are inherent
to the managed lane (−1), hook/engine rule parity is approximate by design
(−1).
