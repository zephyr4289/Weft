# Contributing to Weft

First: read [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md). It is short, and every review decision
downstream of it will make sense only if the Four Laws are already in your head. Then come back
here. This document tells you how the project actually runs: who can do what, how decisions get
made, and what a good pull request looks like.

---

## 0. Why contribute here specifically

Three reasons, in the order most contributors care about:

1. **The blast radius is bounded by design.** The four seams (`RawBuffer`, `Atomic`, `FrameClock`,
   `DrawScope`) mean a platform port is four small classes plus a binding. You cannot break the
   kernel by writing a driver, because the kernel does not know your driver exists.
2. **The evaluation criteria are mechanical.** Conformance is the litmus suite. Zero-alloc is an
   assertion. Whether your port is "good" is not a matter of a maintainer's taste; it is a matter
   of tests you can run before you open the PR.
3. **The ladder is public.** There is a defined path from "fixed a typo in the docs" to "owns a
   platform directory," and it does not run through being personally known to the author.

## 1. The contribution ladder

Each rung grants access to the next. The gates exist to protect the kernel and to make expertise
transferable — you review kernel PRs only after you have felt the seams in a real port.

| Rung | You can touch | Gate |
|---|---|---|
| 1. Docs | Everything in `docs/`, README clarity, translations, the glossary proposals | None — open PR |
| 2. Demos & bench | `demos/`, new benchmark workload configs, `bench/site` content | One maintainer approval; workloads must pin baseline + measured/predicted labels (Law 4) |
| 3. Heddles | `heddles/<framework>/` — new framework bindings | One platform-maintainer approval; no kernel changes; binding must pass the framework's smoke demo |
| 4. Platform port | A new `core/<lang>/` + `steward/<lang>/` implementation | **Litmus suite green on that target** (L1–L8), RFC not required if all four seams are implemented without semantic changes |
| 5. Kernel | `core/` semantics, frame envelope, protocol changes | Accepted RFC + two kernel-maintainer approvals + litmus suite green on all targets |

Nobody lands in rung 5 without having shipped rung 4 (or an equivalent port elsewhere). This is
not gatekeeping for its own sake: the kernel is ~500 lines per language, and its review burden
should approach zero because its change rate should approach zero.

## 2. Finding work

Good first entries, in rough order of current need:

- **Litmus runners.** The suite is the canonical core — every new language runner directly
  strengthens the project's foundation.
- **Benchmark harness code** (`bench/`) — Phase 1 needs the Android A/B/C/D harness.
- **Demo polish** — each workload demo doubles as benchmark subject; a clear side-by-side
  reactive-vs-Weft toggle is the project's best salesperson.
- **Docs that survive a hostile reviewer.** If you find an unbounded claim, a missing baseline, or
  a projection dressed as a measurement: that is a Law 4 bug, and it is treated like one.

Issues labeled `good-first-issue` and `port-help-wanted` are maintained against this list.

## 3. The RFC process

Required for: kernel semantics, frame envelope changes, new protocol versions (`triad-N`),
Steward lifecycle semantics, and anything that adds a Law or an invariant.

1. Fork the doc, copy [`rfcs/TEMPLATE.md`](rfcs/TEMPLATE.md) to
   `rfcs/NNNN-short-name.md` (number assigned on PR open).
2. The RFC must state: the problem, the proposed mechanism, the invariants touched, the litmus
   tests that would prove it, alternatives considered (including "do nothing"), and the boundary
   of the new claim (Law 4).
3. **Lazy consensus, 7 days.** Silence is consent. Any maintainer may block with a substantiated
   objection; objections are resolved by amendment or by an explicit maintainer vote (see
   [GOVERNANCE.md](GOVERNANCE.md)). There is no informal "the author merged it himself" path for
   kernel changes.
4. Accepted RFCs get `Status: Accepted` with a link to the tracking issue; implemented RFCs get
   the tracking issue closed. The RFC folder is the project's institutional memory — a future
   maintainer who reads `rfcs/` in order should understand *why* every big decision happened.

## 4. Pull requests

### 4.1 The checklist

The PR template embeds these; they restate the Four Laws as concrete gates:

- [ ] **Law 1** — does this add any wait, spin, lock, or back-pressure to the writer or reader
      path? (If yes: reject, or justify why the path is cold.)
- [ ] **Law 2** — does the hot path allocate? Run the zero-alloc assertion locally; paste the
      counter deltas in the PR body.
- [ ] **Law 3** — does this add a dependency on a UI framework, renderer policy, or layout
      concern to `core/` or `steward/`? (If yes: it belongs in a Heddle or it does not belong.)
- [ ] **Law 4** — every claim in new docs carries a baseline, a measured/predicted label, and a
      source for platform limitations.
- [ ] Kernel or Steward changes: link the RFC and the green litmus run.
- [ ] **Multiplatform & CI Conformance**: Follow the [Patch Engineering & CI Diagnostic Guide](../PATCH_ENGINEERING_AND_CI_GUIDE.md) to ensure mirror parity, API Extractor signatures, xlang bit-identity, and zero sanitizer findings.
- [ ] New public API: documented in the same PR, with an example that would survive the founding
      spec's honesty review.
- [ ] DCO sign-off present (`git commit -s`) — see §5.

### 4.2 What review is like

Reviewers will read your PR against the Laws before they read it against the diff. Expect blunt
feedback — the project's tone is direct, not unkind, and "this is a Law 2 violation in disguise"
is considered a complete and helpful review comment. Labeled projections, cited limitations, and
admitted unknowns get fast approvals; unbounded superlatives get slow ones.

### 4.3 Code style principles

- **Zero dependencies in `core/`.** The kernel must be readable in one sitting; a dependency
  graph in the kernel is a maintenance lien.
- **Every module starts with a one-paragraph "why exists" header** that cites the section of
  ARCHITECTURE.md or an RFC that justifies it.
- **Naming is governed.** New concepts take a term from [docs/GLOSSARY.md](docs/GLOSSARY.md) or
  propose one via RFC. Do not invent adjacent vocabulary in a PR; that is how a codebase ends up
  with four words for "buffer."
- Comments explain why; the code already explains what. If a comment explains a concurrency
  invariant, it cites the RFC section.

## 5. Licensing and the DCO

Weft is Apache-2.0 and stays that way. Contributions use the **Developer Certificate of Origin**:
sign every commit with `git commit -s`. There is no CLA, no IP assignment, and no contribution
terms that can be quietly changed later — a first-time contributor signs exactly what a core
maintainer signs. The future Weft Pro tier is a separate closed repository; nothing you contribute
here migrates into it.

## 6. Recognition

- Sustained rung-3+ contributors in a platform directory are offered platform-maintainer status
  (write access to that directory + review rights). The bar is a shipped, litmus-green port or a
  body of merged Heddle work — not tenure.
- Benchmark results from forked runs on real hardware are first-class contributions: a new device
  row with a reproducible script beats a patch almost every time.
- The changelog credits every contributor by handle, forever.

## 7. Questions

Open a discussion thread rather than a private message; answers to design questions belong in the
repository where the next contributor will find them. The founding spec, ARCHITECTURE.md, and the
RFC folder answer most "has this been considered" questions — including the ones where the answer
is "yes, and here is why that path was rejected."
