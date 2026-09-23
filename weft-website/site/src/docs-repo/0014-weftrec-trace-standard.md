---
RFC: 0014
Title: .weftrec v4 — Kernel Trace Events (format, parity scenario, JSON schema)
Status: Draft (implementation + cross-port evidence attached; ratification pending)
Authors: Weft Core Team
Created: 2026-09-18
Supersedes / Superseded-by: None (complements RFC 0010; v1–v3 untouched)
---

# RFC 0014 — .weftrec v4 Kernel Trace Events

## Summary

Defines `.weftrec` **format version 4**: fixed-width kernel trace events
(publish / claim / drop / revoke / ack / stall / tear / canary_fail) with a
deterministic cross-port parity scenario, a JSON schema for human-readable
export, and a C reference codec (`core/c/trace_rec.{h,c}`) that is the
container's single source of truth. The acceptance bar comes from issue #20
task 1: **all ports emit byte-identical traces for the same seed**.

## Motivation

The capture formats record PAYLOADS: v1 stores kernel-envelope claims, v2
stores fan-out ring claims, v3 compresses both. None of them answer "what
did the kernel DO" — the decision stream that the debug visualizer (issue
#20 task 5), the Prometheus exporter (task 2), and post-mortem replay need.
What exists today is per-port printf archaeology: three runtimes, three
incompatible logs, zero cross-port comparability.

v4 records EVENTS. Because the stream has no timestamps and no scheduler
noise, byte-identity across ports becomes a provable property, not a
statistical one — the same trick the PC3 cadence logs (RFC-0009/0012
parity) and the litmus suite already use, now applied to the kernel's own
run history.

## Guide-level explanation

A port runs the **parity scenario** (below), records one 8-byte event per
kernel decision, and emits the packed stream as hex. `run.sh` in
`fixtures/xlang-trace/` byte-compares every port against the TS reference.
The C reference codec wraps the stream in the `.weftrec` container
(header + per-event CRC-32) for storage, and unwraps + validates it for
replay; the JSON exporter turns it into schema-validated lines for the
Inspector and the visualizer.

## Reference-level specification

### Header (32 bytes) — same layout as v1 (FORMATS §1.1), these fields differ

```
offset  size  field             value / meaning
4       2     format_version    4
8       4     flags             bit 2 = TRACE (0x4); rest reserved, 0
12      4     envelope_version  0 (v4 carries no payload frames)
16      4     frame_count       event_count (name kept for layout stability)
20      4     crc32             CRC-32/zlib over bytes 0..20
```

### Event record (12 bytes, fixed width)

```
offset  size  field     meaning
0       2     kind      u16 (see kinds below)
2       2     aux       u16, kind-scoped (publish = payload_len; else 0)
4       4     data      u32, kind-scoped (seq / epoch / attempts)
8       4     crc32     CRC-32/zlib over bytes 0..8 of the record
```

Kinds: 1 publish (aux=plen, data=seq) · 2 claim (data=seq) ·
3 drop (data=seq refused) · 4 revoke (data=pre-revoke epoch) ·
5 ack (data=epoch after ACK) · 6 stall (data=attempts) ·
7 tear (data=seq) · 8 canary_fail (data=seq).

Fixed width by definition — trace events are fixed-shape; forward
extensibility is a new format version (the same trade v1 made for the
16-byte envelope, now made explicitly for events). Version gate: v4
tooling rejects v1/v2/v3 headers and vice versa.

### The parity scenario (§parity-scenario, normative)

```
payload_max = 64, seed = 0x00C0FFEE (xorshift32, 04-LITMUS §0.2)
seq = 0
for step in 0..N-1:
  state = xorshift32(state); plen = state % 65
  seq += 1
  payload = pat(seq, 0..plen-1); r = publish(seq, plen)
  if r == DROPPED_REVOKED: emit DROP(data=seq); emit ACK(data=epoch)
  else:                    emit PUBLISH(aux=plen, data=seq)
  state = xorshift32(state)
  if state % 3 == 0: claim(); emit CLAIM(data=r_seq())
                    (canary_check MUST be OK — TIER4 §3 holds)
  if step == N/2: emit REVOKE(data=epoch); revoke()
```

Single-threaded by design: no scheduler noise, no wall clock. The stream
is a pure function of (N, seed) — which is what makes byte-identity a
*property* instead of a hope. Wall-clock belongs to the replay tool, never
the event stream (the v1 rule "no timestamps" applied to the whole format).

### The byte-identity surface

The **packed stream WITHOUT per-record CRCs** (8 bytes/event) is what ports
byte-compare. The container is produced by the C reference codec; ports
without a filesystem hand the stream to the reference tool. This keeps the
cross-port contract minimal (one pack function per port) while the messy
parts (CRC, header, JSON) stay single-sourced.

### JSON schema

`schemas/weftrec-trace.schema.json` validates the human-readable export:
a header line (format/version/kind/events) followed by one object per
event `{"i":N,"kind":"publish|claim|drop|revoke|ack|stall|tear|canary_fail","aux":N,"data":N}`.
`weft_trace_to_json` (C) is the reference exporter; the schema is the
contract for every other exporter.

## Drawbacks

- Fixed-width events cannot carry free-form context (a stall's slot index,
  a tear's suspected field). The `aux` word covers the common cases;
  anything richer is a v5 conversation, guided by the visualizer's needs.
- The parity scenario is a fixed script — it exercises the kernel's
  decision stream but not every crash path. It complements (not replaces)
  the litmus suite and the chaos shards.

## Rationale and alternatives

- JSON-as-primary was rejected: event streams at 8 kHz are ~7 MB/minute of
  text; binary-with-JSON-export keeps both worlds honest (the same split
  v1 made, and it has aged well).
- Emitting from inside the kernel (hot-path hooks) was rejected: Law 2
  (zero is a contract) and the kernel freeze. Emission lives in the
  DRIVER layer over the public API — the scenario proves the API alone is
  enough to reconstruct everything the visualizer needs.

## Prior art

PC3 cadence logs (byte-identical cross-port decision logs), RFC-0008
freshness telemetry, RFC-0011 flight recorder, Chrome tracing's JSON
export model.

## Unresolved questions

- Should the visualizer grow a v4 stream importer directly in JS (the
  pack function is 10 lines), or always go through the C codec? (Lean:
  direct JS import; the container stays optional.)
- Do VM ports want the container writer natively, or is C-as-container-
  authority stable enough? (The fixture's toolchain-honesty pattern
  already answers the operational half.)
