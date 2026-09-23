---
RFC: NNNN
Title: <short descriptive title>
Status: Draft | Accepted | Implemented | Rejected | Withdrawn
Authors: <handles>
Created: YYYY-MM-DD
Supersedes / Superseded-by: <RFC numbers, if any>
---

# RFC NNNN — <title>

## Summary

One paragraph. What is proposed, in plain language a platform maintainer can evaluate without
reading the whole document.

## Motivation

Why the current state is insufficient. Cite the Laws, invariants, litmus tests, benchmark gaps, or
documented user pain that motivate this. A motivation that cannot point at something concrete is
a wish, not a proposal.

## Guide-level explanation

Explain the proposal as a user would experience it: the API they write, the behavior they observe,
the errors they see. If the proposal changes nothing user-visible, say so and explain who *does*
feel the change.

## Reference-level specification

The precise mechanism: data structures, ordering guarantees, state machines, envelope changes,
pseudocode. This section must contain enough detail for a platform maintainer to implement it
without consulting the authors — that is the acceptance bar for reference-level sections.

Include:

- **Invariants touched** — which of I1–I6 (or which new invariant) changes, and how it is preserved.
- **Litmus impact** — which L-series tests cover this, which must be added, and which existing
  tests would fail during transition.
- **Envelope impact** — none, additive (forward-compatible), or breaking (= new protocol version).

## Boundary of the claim (Law 4)

What this proposal enables, stated with its limits. What it does not enable, and will not be
marketed as enabling. Unbounded claims are rejected at RFC stage rather than at review stage.

## Alternatives considered

Including "do nothing." For each: the mechanism, why it loses. The withdrawn two-variable Triad
design ([rfcs/0001 §3](0001-triad-exchange-protocol.md)) is the canonical example of the depth
expected here — alternatives are argued from memory-model or benchmark evidence, not taste.

## Drawbacks

Why we *shouldn't* do this. Honest drawback sections are the project's culture; an RFC without
drawbacks is incomplete, not ideal.

## Open questions

What the author does not yet know. Listed, not hidden — the founding spec set this norm and the
RFC process inherits it.

## Implementation plan

Who builds it, against which milestone (a ROADMAP phase or the litmus suite), and the mechanical
acceptance criterion that flips `Status` to `Implemented`.

---

*Process notes: lazy consensus, 7 days — silence is consent; a substantiated objection cites a
Law, an invariant, or a litmus test. Kernel RFCs additionally require both kernel-maintainer
approvals. See [CONTRIBUTING.md](../CONTRIBUTING.md) §3 and [GOVERNANCE.md](../GOVERNANCE.md).*
