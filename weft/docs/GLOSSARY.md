# Glossary

The weaving vocabulary is **load-bearing, not decorative** — it is the project's naming system and
its governance mechanism. One metaphor, four core terms, five minutes to learn. This document is
normative: new concepts *must* take a term from this glossary (or add one via RFC) before merge.

---

## Core terms

| Term | Meaning | Weaving etymology |
|---|---|---|
| **Weft** | The library. The continuous-state channel and its off-heap buffers. The hot-state plane itself. | The dynamic thread woven across the stationary warp; the thread that moves. |
| **Warp** | The static UI tree: composition, layout, the reactive plane. Reserved vocabulary — never a component name (see banned list). | The static thread held under tension on the loom; the scaffold. |
| **Heddle** | The binding layer. Reads a Weft during the Draw phase only. One Heddle per framework (Compose, React, SwiftUI…). | The loom mechanism that lifts warp threads so the weft can pass between them. |
| **Steward** | The lifecycle manager. Allocates, binds, frees, and leak-detects Wefts. | One who manages property on behalf of its owner. |
| **Triad Protocol** | The writer–reader synchronization protocol: three buffers, one atomic, ownership by exchange. Specified in [rfcs/0001](../rfcs/0001-triad-exchange-protocol.md). | A group of three; the three-buffer rotation. |

## Protocol vocabulary

| Term | Meaning |
|---|---|
| **Publish** | The writer's single wait-free operation: fill the private buffer, swap it into `latest`. |
| **Claim** | The reader's single wait-free operation: swap the freshest buffer out of `latest`, hand the previous one back. |
| **Envelope** | The frozen frame header (magic, protocol version, seq, dims, dtype, shape). Tier 0 — never changes shape. |
| **`latest`** | The single shared atomic index. The only synchronization variable in the entire kernel. |
| **`seq`** | Writer-maintained frame counter in the envelope; the backbone of freshness telemetry and litmus tear detection. |
| **Writer token** | The revocation handle returned by `attachWriter`; the use-after-free guard (invariant I6). |
| **Latest-wins** | The semantics of display: intermediate frames may be dropped silently; the freshest frame is always the one claimed. |

## Project vocabulary

| Term | Meaning |
|---|---|
| **Seam** | One of the four platform abstractions (`RawBuffer`, `Atomic`, `FrameClock`, `DrawScope`). A port implements four seams + one Heddle. |
| **Driver** | A per-platform implementation of the seams. The kernel knows nothing about drivers. |
| **Litmus suite** | The canonical conformance suite ([litmus/](../litmus/)). The project's core artifact: implementations conform to the suite, not to each other. |
| **Litmus test** | A two-thread protocol scenario with a mechanical pass/fail verdict, adversarial parameters, and an invariant it proves. |
| **Tier 0 / 1 / 2** | Stability tiers: frozen (envelope + protocol semantics) / semver (kernel, Steward APIs) / fast lane (Heddles, demos, tools, bench). |
| **The moat** | The open, reproducible benchmark site. The asset platform vendors cannot absorb. |

## Banned names — and why (naming governance)

These names are permanently retired from the project. The list is normative history: the collision
test that retired them is now a required step for any new term.

| Name | Collision | Verdict |
|---|---|---|
| ~~Warp~~ (library name) | `warp.dev` — the AI terminal's domain, same developer audience; also a popular Rust web framework | Retired 2026; renamed to Weft |
| ~~Loom~~ (binding layer) | Project Loom — JVM virtual threads; permanent confusion inside a Kotlin library | Retired 2026; renamed to Heddle |
| ~~Carder~~ (lifecycle manager) | Security slang for a credit-card fraudster ("carding") | Retired 2026; renamed to Steward |

## Rules for new terms

1. **Weaving first.** Before coining anything, check weaving/loom vocabulary — there is a large,
   mostly unclaimed supply (reed, shuttle, bobbin, selvedge, twill…). A new term that continues
   the metaphor teaches itself.
2. **The collision test is mandatory.** GitHub, npm, Maven Central, pub.dev, PyPI, crates.io, and
   a domain search — a term that fails any of these needs a scoped name (`@weft/x`) or a new term.
3. **One concept, one term, everywhere.** Docs, API, error messages, and RFCs use the same word
   for the same thing. Synonyms are bugs.
4. **English terms, for now.** Translations of the docs are welcome; translated *component names*
   are not (the vocabulary must stay greppable across the codebase).
