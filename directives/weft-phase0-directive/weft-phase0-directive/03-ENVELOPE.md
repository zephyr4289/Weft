# Frame Envelope Specification — Tier 0 Frozen

**Normative.** The envelope is the versioned contract between writers and readers across languages, processes, and protocol generations. It freezes **once** and never changes shape (Tier 0, roadmap §0.4). Any change = new version via the negotiation mechanism, never an edit here.

---

## 1. Bit-exact layout (version 1, "triad-1")

```
offset  size  field        value / meaning
──────  ────  ───────────  ───────────────────────────────────────────────────────
0       4     magic        ASCII "WEFT" = bytes 57 45 46 54 (LE u32: 0x54464557)
4       2     version      1 = triad-1
6       2     header_size  16  (self-describing; the growth mechanism — see §3)
8       4     seq          frame sequence, u32; 0 = null frame; publishes start at 1,
                           monotonically increasing per Weft, wraps at 2^32−1 (accept wrap)
12      4     payload_len  payload bytes following the header
──────  ────  ───────────  TOTAL = 16 bytes
```

**All fields are little-endian.** No exceptions, all languages, forever.

**Access rule:** read/write via `memcpy` (C), `to_le_bytes`/`from_le_bytes` (Rust), `DataView` with `littleEndian=true` (TS). Never type-pun, never assume alignment.

**Canary** (kernel-level, not part of the envelope): u64 LE at `buf_size−8`, value = seq, written in `w_publish` after the envelope. It exists so L6/L7 can detect out-of-ownership writes that stop short of the payload.

## 2. Decode rules (normative for every reader, every language)

A reader given `buf, avail`:

1. `avail >= 16` else `DECODE_SHORT`.
2. `magic == "WEFT"` else `DECODE_BAD_MAGIC`.
3. Read `version`, `header_size`.
4. `header_size >= 16` and `header_size <= avail` else `DECODE_BAD_HEADER`.
5. **The payload begins at `header_size` — never at 16.** A reader that hardcodes 16 fails L8 by construction.
6. `payload_len <= avail − header_size` else `DECODE_SHORT`.
7. Fields the reader's version does not know (i.e., anything between 16 and `header_size`) are **skipped without validation**. Unknown means unknown — no size checks, no pattern checks, no warnings in the hot path.

## 3. Version negotiation (bind-time)

At bind, the reader offers a set `S` of supported versions; the writer declares `W`:

```
chosen = max({ v ∈ S : v ≤ W })        # highest mutually supported
if the set is empty → refuse the bind (BIND_INCOMPATIBLE)
```

The chosen version is stamped into every envelope this Weft produces. Runtime re-negotiation per frame is **forbidden** — the envelope is per-frame stateless by design; negotiation happens once at bind.

**Coexistence:** a process may host a triad-1 Weft and a triad-2 Weft simultaneously; negotiation is per-Weft. Phase 0 implements version 1 only, but the negotiation function and header_size-driven decode must be complete — L8 exercises both with synthetic v2 envelopes.

## 4. Evolution rules (how triad-2 will happen without breaking triad-1)

- New fields → larger `header_size`, same 16-byte prefix, bump `version`. Old readers skip what they don't know (rule §2.7). This is the **only** sanctioned mechanism.
- Semantic change to an existing field → new version, never an in-place edit.
- `header_size` exists so that "unknown trailing fields" is a *tested, working* path (L8b), not a promise.

## 5. L8 hooks (tested surface — details in 04-LITMUS)

| Subcheck | What must work |
|---|---|
| round-trip | encode → decode reproduces fields byte-identically |
| unknown trailing fields | decode an envelope with `header_size=24` and 8 opaque bytes → succeeds, payload found at +24, extras ignored |
| negotiation table | (W=1, S={1})→1 · (W=2, S={1,2})→2 · (W=2, S={1})→BIND_INCOMPATIBLE · (W=3, S={1,2})→2 |
| coexistence | one process holds a v1-stamped and a v2-stamped Weft; both decode correctly with the same reader code |
