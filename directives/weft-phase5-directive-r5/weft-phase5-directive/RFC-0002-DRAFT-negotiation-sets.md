# RFC-0002 (DRAFT, DEFERRED) — Negotiation by Capability Sets

**Status:** Draft — **do not implement**. Trigger: the moment triad-2 design work begins (a semantic version break per 03-ENVELOPE §4 line 2, or any writer that cannot honestly advertise the scalar ceiling).
**Amends:** 03-ENVELOPE §3. **Supersedes:** the scalar formula `max({v ∈ S : v ≤ W})`.
**Preserves:** wire format (Tier 0 untouched — this is bind-API semantics only), all L8 vectors re-issued from the set table below.

---

## 1. Problem

03-ENVELOPE v1.1 confirms the scalar formula normative, with the writer-ceiling commitment: a writer declaring `W` vouches it can emit every `v ≤ W`. That commitment is mechanically free while evolution stays *additive* (bigger `header_size`, skip-unknown). It stops being honest at the first **semantic** version break: a writer at v4 (semantic change) may be unable to emit v3, yet the scalar form `W=4` advertises exactly that. Lying in the capability declaration is worse than refusing the bind.

## 2. Decision (when triggered)

Both sides advertise **sets**; bind is set intersection:

```
bind(writer_supported: set<version>, reader_supported: set<version>)
    chosen = max(writer_supported ∩ reader_supported)
    ∅  → BIND_INCOMPATIBLE
```

- Writers advertise exactly what they can emit. A v4-only writer: `{4}`. A v4 writer that also kept the v3+v1 additive paths: `{4, 3, 1}`.
- The reference kernels advertise `{current}` by default — downgrade paths are **opt-in**, never implied. This keeps Tier-0 honesty: frozen means exactly one version.
- Rolling upgrades (new writer, old reader fleet) become an explicit writer capability: `{2, 1}` negotiated against `{1}` binds 1. The mechanism that makes upgrades deployable is a declared capability, not an assumption.

## 3. Decision table (replaces 03 §5 when triggered)

| writer_supported | reader S | chosen |
|---|---|---|
| {1} | {1} | 1 |
| {1, 2} | {1, 2} | 2 |
| {2} | {1} | BIND_INCOMPATIBLE |
| {2, 1} | {1} | 1 |
| {3} | {1, 2} | BIND_INCOMPATIBLE |
| {3, 2} | {1, 2} | 2 |

(This table subsumes 03 v1.1 §5: the scalar form is the special case `writer_supported = {1..W}`, valid precisely when all evolution is additive.)

## 4. Migration

1. Implement `bind` as set intersection behind the existing call sites; the scalar form becomes a convenience wrapper (`bind_ceiling(W, S) = bind({1..W}, S)`).
2. Re-issue L8 vectors from §3 (six rows replace four).
3. No wire change; no kernel protocol change; envelope stamping rule unchanged ("chosen version stamped into every envelope this Weft produces").
4. History note for the RFC: row 3 of the v1.0 table (`(W=2,S={1}) → BIND_INCOMPATIBLE`) was the *correct verdict for this table's semantics* — a {2}-only writer cannot serve a {1}-only reader. The v1.0 error was presenting that semantics in a scalar formula that contradicted it; WO-P0A §3 has the full proof.

## 5. Why defer

Until a second version exists, every writer declares `W=1` and every reader `{1}`; scalar and set semantics are extensionally identical, so implementing sets now is speculative machinery with no testable behavior difference. Litmus discipline: nothing ships without a test that can distinguish it.
