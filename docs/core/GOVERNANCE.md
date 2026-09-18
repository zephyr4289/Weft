# Governance

One page, on purpose. Governance that takes longer than five minutes to read will not be read,
and unread governance is decoration.

---

## Charter

The permanent clauses — amendable only by a supermajority of maintainers (see below), never by
ordinary PR:

1. The Four Laws ([docs/PHILOSOPHY.md](docs/PHILOSOPHY.md#2-the-four-laws)) are the project's
   constitution. No release, RFC, or maintainer decision may violate them.
2. The litmus suite is the canonical core. "Weft" is a conformance claim, not a lineage claim.
3. The benchmark suite and its published results stay open and reproducible, forever.
4. The core stays Apache-2.0, forever. The future Pro tier is a separate product in a separate
   repository; nothing contributed here migrates into it.

## Roles

| Role | Who | Powers |
|---|---|---|
| **User** | Anyone using or evaluating Weft | Open issues, discussions; everything else builds on their feedback |
| **Contributor** | Anyone with a merged PR | May take ladder rungs 1–3 tasks ([CONTRIBUTING.md](CONTRIBUTING.md)) |
| **Platform maintainer** | Named in CODEOWNERS for a platform directory (e.g. `core/swift/`, `heddles/swiftui/`) | Review and merge within their directory; nominate rung-3+ contributors; vote on RFCs |
| **Kernel maintainer** | Named in CODEOWNERS for `core/` and `rfcs/` (minimum two, always) | Merge kernel changes; accept/reject RFCs; vote on charter amendments |
| **Lead maintainer** | Zephyr, until the project votes otherwise | Breaks ties; guards the charter; does the boring coordination. Deliberately weak: cannot merge against a substantiated maintainer objection |

## How decisions are made

In escalating order, each used as rarely as possible:

1. **Lazy consensus (default).** PRs and RFCs merge when no substantiated objection stands after
   7 days. Most decisions should end here — the ladder and the Laws pre-decide most disputes.
2. **Maintainer vote.** When consensus stalls, platform and kernel maintainers vote; simple
   majority wins; the lead breaks ties. Kernel changes additionally require both kernel
   maintainers' approval, always.
3. **Charter amendment.** Supermajority (two-thirds) of all maintainers, 14-day discussion window.
   The Four Laws are expected to outlive every current participant.

A "substantiated objection" cites a Law, an invariant, a litmus test, or a documented boundary —
"the Laws forbid this," not "I don't like this." Aesthetic objections lose to working code; the
Laws never do.

## Code ownership

- `CODEOWNERS` maps directories to maintainers and is the single source of truth for review
  routing. It mirrors the kernel/driver model: two owners for `core/`, at least one per platform
  directory.
- Platform maintainers are **granted, not grown, when possible** — an expert who ships a
  litmus-green port can own that port from day one, because the seams bound the blast radius.
  Competence in one directory is never presumed to transfer to another.
- Kernel maintainer seats never go unfilled: if a kernel maintainer steps down, the RFC queue
  freezes for kernel changes until a successor is voted in. A frozen kernel is the safe state.

## Modules of last resort

- **Roadmap changes** (adding/reordering phases in [ROADMAP.md](ROADMAP.md)) — maintainer vote.
- **New platform phases** — require a litmus-green port plan and a volunteer maintainer *before*
  the phase starts, not after. No orphan platforms.
- **Security-relevant changes** — follow [SECURITY.md](SECURITY.md); disclosure windows override
  normal merge timing.

## Changes to this document

Treat governance like the kernel: an RFC touching `GOVERNANCE.md` is itself governed by the
process it proposes to change. Amendments require the charter's supermajority if they touch the
charter; otherwise lazy consensus with lead-maintainer sign-off.
