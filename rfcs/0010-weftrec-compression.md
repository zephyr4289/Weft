---
RFC: 0010
Title: .weftrec Payload Compression (format v3)
Status: Draft (implementation + e2e evidence attached; ratification pending)
Authors: Weft Core Team
Created: 2026-09-16
Supersedes / Superseded-by: None
---

# RFC 0010 — .weftrec Payload Compression

## Summary

Proposes `.weftrec` format version 3: per-record payload compression for
fan-out flight-recorder captures, using a self-contained
delta-zigzag-varint (dzv) codec over u32 payload words, with a
self-limiting rule that falls back to verbatim storage whenever compression
would not shrink the record. `weft-fanout-rec capture|daemon --compress`
writes v3; `validate`/`replay` read v2 and v3 transparently.

## Motivation

Flight-recorder sessions are the post-mortem record of a fan-out stream:
the daemon (`--shm`, Series 6) can run for hours. At 8 kHz × 256 B
payloads, a session writes ~7.3 MB/minute — ~26 GB/day — for payloads that
are, in real applications (canvas stroke batches, PCM frames, telemetry
curves), highly redundant frame-to-frame. The existing format stores every
payload verbatim; long sessions are needlessly expensive to keep, ship,
and load into the Inspector.

What compression does NOT target: the 04-LITMUS mixer family used across
the conformance suites is deliberately pseudorandom — it is the
incompressible control, and the codec must decline it honestly rather than
grow records.

## Guide-level explanation

`capture --compress out.weftrec` (and `daemon --compress`): each record's
payload is dzv-coded when that shrinks it, stored verbatim otherwise. The
file remains a normal `.weftrec` — same header layout (version 3, flags
carry a COMPRESSED bit), same record kinds, same CRC/accounting rules — so
`validate`, `replay`, and the JS v2 parser's version gate treat v3 as a
first-class version boundary.

## Reference-level specification

See `tools/FORMATS.md` §1.6 (normative): header deltas (version 3, flags
`FANOUT|COMPRESSED`, unknown bits rejected), record deltas (codec +
stream_len in v2's reserved fields, `payload_len` remains the ORIGINAL
length, byte-granular `rec_len`), and the dzv definition (wrapping u32
deltas, arithmetic-shift zigzag, LEB128 varints, malformed-stream
rejection).

Key normative points:

- **Self-limiting**: codec 1 only when `stream_len < payload_len`; else
  codec 0. Never grow a record.
- **Wire transparency**: `payload_len` is the ring payload's length;
  validate/replay/compare see wire-identical payloads after decompression.
- **Versioning**: v2-only tooling rejects v3 (version gate + the v2
  flags==FANOUT contract); v3 tooling reads v2 (newer reader, older
  writer). v1 unchanged.
- **No new dependencies**: dzv is ~90 lines of C; the repo's zero-dep
  tooling bar holds (no zlib-lz4-zstd linkage for the codec itself; the
  CRC remains the §1.4 contract).

## Boundary of the claim (Law 4)

Compression is a storage-cost optimization with an integrity contract: the
per-record CRC covers the codec stream, and a malformed stream is
corruption (rejected), so tampering detection is preserved. The codec
makes NO confidentiality claim — dzv is not encryption; authenticated
captures use RFC 0005 on top (a future `.weftrec` × VerifiedWeft binding
is an open question). Compression never changes drop accounting: the
telescoping identity is enforced on the same seq/dropped fields as v2.

## Falsifiable benchmark claims (e2e evidence)

`tools/weft-fanout-rec/e2e.sh` (the `make e2e` gate) measures, per run:

- Wave family (900 k-amplitude curves + 37th-harmonic ripple — the
  real-signal shape): v3/v2-equivalent ratio **1.32x** on 256 B payloads
  (19k-frame session; per-run variance is reported by the script).
- Mixer family (pseudorandom control): **0 dzv records / 100% stored** —
  the honest decline, costing only the per-record codec word.
- Round-trip: capture → validate (bit-exact payloads through
  decompression) → replay → recapture → ordered subsequence compare, all
  green; v2 backward-compat and both version gates enforced.

The dzv codec is expected to shine on larger payloads (amortized varint
overhead) and low-delta streams; the claim is deliberately scoped to what
the e2e measures, not to general-purpose compression (zstd-class codecs
are the alternative considered — rejected here for the zero-dep bar and
the honest-ratio scope).

## Alternatives considered

- **zstd/lz4 linkage**: higher ratios on mixed content, but breaks the
  tool's zero-dependency bar and pulls a codec surface far larger than the
  problem; revisit if capture workloads outgrow dzv (open question).
- **Whole-file compression (tar.zst)**: kills the crash-tolerant
  scan-to-EOF contract — a truncated file would be unreadable. Rejected.
- **Frame-level dictionary compression**: better ratios on repetitive
  sessions, but requires cross-record state, which breaks the per-record
  CRC/validate/re-sync contract. Rejected for v3; a future v4 could
  propose chunk-scoped dictionaries behind the same version gate.

## Drawbacks

Two bytes of per-record overhead when the codec declines (stored records);
a third format version to support (bounded: v3 tooling reads v2).

## Open questions

- JS-side v3 parse (the Inspector loads v3 captures: decompress in the
  browser is straightforward — the dzv reference is ~40 lines of TS;
  proposed as a Series 7 item alongside the VerifiedWeft × .weftrec
  binding).
- Should `replay --compress` re-emit v3 (currently replay writes rings,
  not files — the recapture leg owns compression)?

## Staff Decision

[EMPTY — implementation + e2e evidence attached (`litmus/evidence/fanout/`
v3 legs); ratification pending]
