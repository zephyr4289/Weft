# Weft CI — Routing Contract & Shard Ownership

This document is the contract between every CI gate in this repository and
the changes it claims to guard. It exists so that two properties hold at the
same time, permanently:

1. **No gate ever misses a change that can affect it.** (Strictness is
   preserved by construction — filters err toward running MORE.)
2. **No gate wakes up for a change that cannot affect it.** (Runner-minutes
   are spent only where they carry information.)

## The problem this solves

Before path routing, every push and every PR woke **40 jobs** (the full
27-shard Extreme Test Matrix plus the npm / Apple / Flutter / Android /
demo package workflows), regardless of what changed. A typo fix in a
markdown file paid the same CI bill as a kernel protocol rewrite — and a
`core/**` push to `main` additionally fired the entire 13-job Nightly Deep
matrix on the same SHA the Extreme matrix was already testing. Every PR
series had been adding shards under `any_code` semantics, so the bill grew
monotonically with each merge.

## How routing works now

`extreme-test.yml` starts with a fast `changes` job (`dorny/paths-filter`)
that classifies the change set and exports one boolean per route. Every
matrix shard carries a `filter:` key naming its route; it runs iff that
route is on. Manual `workflow_dispatch` forces every route on — a dispatch
is an explicit human intent to run the full matrix.

The other package workflows (npm, Apple, Flutter, Android packages, demo,
Android emulator) carry workflow-level `paths:` filters declaring their
ownership, which is the table below's second column.

## Shard ownership table

| Route | Trigger paths | Shard(s) / workflow |
|---|---|---|
| `any_code` | `core/**`, `litmus/**`, `bench/**`, `tools/**`, `probes/**`, `packages/**`, `fixtures/**`, `heddles/**`, `android/**`, `apple/**`, `demos/**`, `scripts/**`, `Makefile`, `package.json`, `pnpm-lock.yaml`, `pnpm-workspace.yaml`, `ci/**`, `.github/workflows/extreme-test.yml` | build gatekeeper, CodeQL, litmus-c/rust/ts, bench-b-c/rust/ts, wsuite, thermal-proxy, tools-interop, ports-validate, browser-sab, fanout-concurrent, fanout-native, turbo-native, gpu-native, verifiedweft, perf-regression, chaos, chaos-parity, simd-blend, qos-worklet, guardian, sanitizers, trace-standard, revoke-stress, ffi-fuzz, memory-model-formal, wcet-audit, recovery-self-stabilizing, ipc-mesh |
| `formal` | `formal/**`, `core/**`, `ci/scripts/run_formal_shard.sh` | formal (TLA+ / TLC proofs) |
| `forensic` | `reports/**`, `ci/scripts/run_forensic_scan_shard.sh` | forensic-scan (PAST-CROPBOX PDF scan) |
| `site` | `tools/make_site.py`, `bench/results.json`, `bench/results/**`, `Makefile`, `ci/scripts/run_site_determinism_shard.sh` | site-determinism |
| `canonical` | `bench/results.json`, `ci/scripts/run_canonical_audit_shard.sh` | canonical-audit (R1 Path C invariant) |
| `silent_green` | `ci/**`, `.github/workflows/**` | silent-green-audit (pipefail discipline) |
| — (workflow-level `paths`) | `packages/**`, `core/ts/**`, `fixtures/**`, `heddles/**`, `package.json`, `pnpm-lock.yaml`, `pnpm-workspace.yaml`, `ci/scripts/run_binding_parity.sh` | npm-packages.yml (Node 20/22/24 matrix) |
| — (workflow-level `paths`) | `Package.swift`, `core/swift/**`, `apple/**` | apple-packages.yml (macOS SPM + iOS Simulator) |
| — (workflow-level `paths`) | `packages/flutter_weft/**`, `core/c/**`, `core/dart/**`, `ci/scripts/run_binding_parity.sh` | flutter-packages.yml (3-OS FFI matrix) |
| — (workflow-level `paths`) | `android/**`, `core/kotlin/**`, `core/c/**`, `ci/scripts/run_binding_parity.sh` | android-packages.yml (Gradle) |
| — (workflow-level `paths`) | `android/**`, `core/c/**`, `core/kotlin/**` | android-emulator.yml (API 26/33/34 matrix) |
| — (workflow-level `paths`) | `demos/**`, `packages/**`, `core/ts/**`, `package.json`, `pnpm-lock.yaml` | demo-showcase.yml |

Deliberately unowned (no gate consumes them): `docs/**` (except site
inputs above), `rfcs/**`, `evidence/**`, `directives/**`, `spikes/**`,
`D-report/**`, `patches/**`, `contrib/**`, `LICENSE`, `SHA256SUMS`,
`.gitignore`. A change confined to these paths runs zero CI — correctly.

## Rules for adding a new shard (the anti-regrowth contract)

1. **Declare ownership in the same PR.** A new shard must (a) pick an
   existing route, or (b) add a new named filter to the `changes` job and
   a row in the table above. A shard without an owner is a bug.
2. **Err toward running more.** When unsure whether a path can affect your
   shard, add the path to your filter. Over-triggering costs minutes;
   under-triggering silently disables a gate — the second is never
   acceptable.
3. **Never widen `any_code` to mean "everything".** `any_code` is the set
   of paths that can affect *kernel / harness / cross-language* behavior.
   Documentation and audit-trail trees are not in it, by design.
4. **Manual dispatch is the escape hatch.** `workflow_dispatch` forces the
   full matrix; the `deep=true` dispatch input now really triggers the
   nightly-deep workflow on the same branch.

## Nightly Deep

`nightly-deep.yml` runs the expensive matrix at 00:00 UTC daily, on manual
dispatch, and on pushes whose head commit carries the `[nightly]` marker.
It no longer fires on ordinary `core/**` pushes — that behavior double-billed
the most expensive jobs we have on the exact SHA the Extreme matrix was
already testing. The nightly cron still covers `main`'s HEAD every night:
same coverage, no duplicates.

## Strictness proof sketch

For every gate G with input set I(G) (the files G reads, compiles, or
executes) the routing guarantee is: **G runs on every change set that
intersects I(G), and I(G) ⊆ (paths of G's filter) ∪ (files G's scripts
read at runtime that are themselves gated).** Each filter in the table
above was derived by enumerating I(G) from the shard script's `open()`,
`make`, `cargo`, `gcc`, and `glob` targets — not by intuition. Where
enumeration was uncertain, the path was added (see rule 2). The removals
are exactly the changesets whose intersection with I(G) is empty for
every gate: vacuous with respect to every check, therefore skipping them
cannot flip any verdict from red to green.

## Wall-clock guarantee

- Path routing adds one ~10-second job that runs **in parallel** with the
  multi-minute build gatekeeper — it is not on the critical path.
- Shards no longer wait for the gatekeeper to finish (they never needed
  its output; each rebuilds its own kernels, and they already ran on build
  failure via `if: success() || failure()`). The matrix now starts
  immediately, which strictly reduces end-to-end latency.
- New caches (pip downloads, tectonic binary, pnpm store, Gradle, pinned
  TLC jar) only remove time; a cache miss costs the same as the previous
  cold path.
- No test, threshold, retry count, or matrix axis was removed or relaxed.

## Developer & Agent Diagnostic Guide

For full rules on how to format multiplatform patches, ensure binding parity, satisfy API Extractor, avoid sanitizers/link issues, and run pre-push local gates, see the comprehensive [Patch Engineering & CI Diagnostic Guide](../docs/PATCH_ENGINEERING_AND_CI_GUIDE.md).

