# Formats — Weft Tooling Artifact Specs

> **Normative for all tool implementations.** Written before any recorder code (WO-P2-TOOLS T2). Format is the contract.

---

## 1. `.weftrec` v1 — Capture/Replay File Format

Bit-exact, little-endian, all languages forever.

### 1.1 File header (32 bytes)

```
offset  size  field             value / meaning
──────  ────  ───────────────   ───────────────────────────────────────────────────────
0       4     magic             ASCII "WREC" = bytes 57 52 45 43 (LE u32: 0x43455257)
4       2     format_version    1
6       2     header_size       32 (self-describing; growth mechanism — unknown trailing fields skipped)
8       4     flags             all reserved, 0
12      4     envelope_version  triad-1 = 1 (the envelope version used by the recorded frames)
16      4     frame_count       patched at close; 0 ⇒ scan to EOF after crash (crash-tolerant)
20      4     crc32             CRC-32/zlib of bytes 0..20 (reflected poly 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF)
24      8     reserved          0
```

### 1.2 Frame record (repeated, frame_count times or until EOF)

```
offset  size  field             value / meaning
──────  ────  ───────────────   ───────────────────────────────────────────────────────
0       4     rec_len           total record length including this field and crc (forward-extensible)
4       16    envelope_header   the 16-byte WEFT envelope header as claimed (verbatim)
20      N     payload           N = envelope.payload_len (the payload bytes as claimed)
20+N    4     crc32             CRC-32/zlib over envelope_header + payload (bytes 4..20+N)
```

### 1.3 Rules

- `rec_len` demarcates records (forward-extensible; unknown record kinds skipped by length, mirroring the L8 skip-unknown philosophy).
- Semantic change to an existing field → `format_version` 2, never an in-place edit.
- No timestamps in v1 (roadmap spec is envelope + seq + payload; pacing belongs to the replay tool; a v2 may add them).
- Stale returns are recorded — the recorder is a protocol reader (WO-P2 §0.2); it claims frames and serializes what it claimed, stale included. The file is the honest capture.
- **Byte-identical criterion (roadmap):** capture computes the stream hash while writing; replay recomputes it; the two must match exactly.
- **Crash tolerance:** if a write fails mid-session, close with `frame_count==0` semantics (crash-tolerant scan path) and report the failure — never emit a silently truncated file that claims completeness.

### 1.4 CRC-32/zlib implementation contract

Reflected polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Same parameters as zlib's `crc32()`. C implementations link `<zlib.h>` or implement the table-based algorithm. Rust implementations must produce identical output (same poly, same init/final — verified by the cross-language interop in T6).

### 1.5 `.weftrec` v2 — Fan-Out Ring Flight-Recorder Capture

v1 records kernel-envelope claims: the recorder is THE sole reader of a 1:1 Triad (§1.3). v2 records FAN-OUT RING claims (`tools/weft-fanout-rec/weft_fanout_rec.c`): the flight recorder is ONE OF N readers of an RFC-0004 ring — the consumer named in that RFC's motivation. Per §1.3's versioning rule, v2 is a new format version, not an in-place edit: **v1 tooling MUST reject v2 files** (version gate), and vice versa (v2 rejects v1).

**Header (32 bytes)** — same layout as v1, these fields differ:

```
offset  size  field             value / meaning
───
4       2     format_version    2
8       4     flags             bit 0 = FANOUT (0x1); rest reserved, 0
12      4     envelope_version  0 (fan-out frames carry NO envelope —
                                 the slot stamp IS the frame id, RFC 0004)
```

All other header fields keep their v1 meaning (magic, header_size=32, frame_count patched at close with the crash-tolerant `0 ⇒ scan-to-EOF` rule, crc32 over bytes 0..20, reserved).

**Frame record (repeated; the only v2 record kind so far):**

```
offset  size  field         value / meaning
───
0       4     rec_len       total record length incl. this field and crc
4       2     kind          1 = fanout claim record (growth: skip by rec_len)
6       2     reserved      0
8       8     seq           claimed frame seq (u64, ring frame counter)
16      8     dropped       frames completed without the recorder observing
                               them (per-claim RFC-0004 accounting — the
                               honest capture: drops visible, never hidden)
24      4     payload_len   bytes of payload copied (= ring payload_bytes)
28      4     reserved      0
32      N     payload       the claimed frame's payload bytes
32+N    4     crc32         CRC-32/zlib over bytes 4..32+N (kind..payload)
```

**Rules (v1's rules carry over unless stated):**

- The stale-claim rule is N-READER-SHAPED: the recorder is one of N consumers; `dropped` records what it missed between claims, and the telescoping identity `sum(dropped) == final_seq - records` must hold for every valid file (`validate` enforces it, per-record: `dropped_i == seq_i - seq_{i-1} - 1`).
- Seqs must be strictly increasing across records.
- `validate --expect-mixer` additionally checks every payload word against the 04-LITMUS §0.1 mixer family — the shared cross-port generator (F-series batteries, xlang fixtures).
- **Replay is content-faithful, not seq-faithful** (declared boundary): `replay` republishes recorded frames into a fresh ring, which renumbers them 1..K; recorded seqs and drop accounting live in the file, enforced by `validate`.
- The capture source is a POSIX shm ring (`shm_open` by name — the §3 forward interface, realized for the fan-out ring): any port's producer may create the ring; the recorder attaches as a reader and never blocks it.

---

### 1.6 `.weftrec` v3 — Compressed Fan-Out Captures (RFC-0010 draft)

v3 extends v2 with per-record payload compression (RFC-0010). Per §1.3's
versioning rule it is a NEW format version, not an in-place edit: v2-only
tooling rejects v3 by the version gate (and by the flags contract — v2
requires flags == FANOUT exactly), v3 tooling reads both v2 and v3
(newer reader understands older writer), v1 stays in its own lane.

**Header (32 bytes)** — v2 layout, these fields differ:

```
offset  size  field             value / meaning
───
4       2     format_version    3
8       4     flags             FANOUT (0x1) | COMPRESSED (0x2); unknown
                                    bits are a version violation (reject,
                                    not skip — flags are header-level)
```

**Frame record** — v2 layout with two repurposed fields (v2's `reserved`):

```
offset  size  field         value / meaning
───
0       4     rec_len       total record length incl. this field and crc;
                               BYTE-GRANULAR in v3 (codec streams have no
                               word alignment; v2's rec_len%4==0 rule is
                               dropped — the v3 divergence)
4       2     kind          1 = fanout claim record
6       2     codec         0 = stored (payload verbatim)
                               1 = delta-zigzag-varint (dzv, below)
8       8     seq           claimed frame seq (u64)
16      8     dropped       per-claim drop accounting (unchanged)
24      4     payload_len   ORIGINAL ring payload bytes (what the ring held;
                               what validate/replay see post-decompression)
28      4     stream_len    bytes of codec stream following
32      N     stream        N = stream_len: the payload verbatim (codec 0)
                               or the dzv stream (codec 1)
32+N    4     crc32         CRC-32/zlib over bytes 4..32+N (kind..stream)
```

**Codec 1 — dzv (delta-zigzag-varint) over u32 words:**

- Compress: `d[i] = w[i] - w[i-1]` (wrapping u32, `w[-1] = 0`), zigzag
  (`z = (s << 1) ^ (s >> 31)` with the shift ARITHMETIC on the signed
  view), LEB128 varint (7 bits/byte, high bit = continuation).
- Decompress is the exact inverse; a stream that overruns, underruns, or
  leaves trailing bytes is corruption (reject).
- Payload must be u32-word shaped (payload_len % 4 == 0, > 0) for codec 1.
- **Self-limiting rule (normative)**: a writer emits codec 1 only when
  `stream_len < payload_len` — otherwise codec 0. The 04-LITMUS mixer
  family is deliberately pseudorandom and lands entirely in codec 0; that
  is the honest outcome, not a failure.

**Rules carried over from v2:** strict seq increase, per-record gap
accounting (`dropped_i == seq_i - seq_{i-1} - 1`), the exact telescoping
identity, crash-tolerant `frame_count == 0 ⇒ scan-to-EOF`, `--expect-mixer`
/ `--expect-wave` payload validation run on the DECOMPRESSED payloads
(wire-identical to a v2 capture of the same session). Replay republishes
decompressed payloads; the seq-renumbering boundary is unchanged.

---
## 2. Probe Output Contract

### 2.1 Text output (default)

Human-readable, one line per field. Telemetry labeled `advisory: true` per AXIOM T.

```
WEFT PROBE — quiesced dump
latest: 1
w_work: 0 (advisory)
r_work: 1 (advisory)
revoked: false
epoch: 0
t_publish: 600 (advisory)
t_claim: 600 (advisory)
t_drop: 0 (advisory)
mid_publish_sample: false
buf[0]: slot=0 owner=writer seq=600 version=1 header_size=16 payload_len=1024
buf[1]: slot=1 owner=reader seq=600 version=1 header_size=16 payload_len=1024
```

### 2.2 JSON output (`--json`)

One JSON object. Same fields, same types. Telemetry carries `"advisory": true`.

```json
{"tool":"weft-probe","mode":"quiesced","latest":1,"w_work":0,"r_work":1,
 "revoked":false,"epoch":0,"t_publish":600,"t_claim":600,"t_drop":0,
 "mid_publish_sample":false,
 "advisory":true,
 "bufs":[{"slot":0,"owner":"writer","seq":600,"version":1,"header_size":16,"payload_len":1024},
         {"slot":1,"owner":"reader","seq":600,"version":1,"header_size":16,"payload_len":1024}]}
```

### 2.3 Live dump (`--live`)

Repeated samples at display rate. Each sample is a JSON object on its own line. The whole output is labeled `advisory`. `mid_publish_sample` may appear. Nothing may crash, block, or allocate.

### 2.4 Revocation safety

A probe run against a revoking workload (L7-style) must survive with correct `revoked` reporting. The probe must NOT dereference freed/poisoned buffers — it reports `slot_idx=3, owner=free` for any buffer with no live owner (I6 rule).

---

## 3. Phase 6+ Forward Interface (specified; fan-out ring side REALIZED)

The in-sandbox tools link the kernel and operate on the handle directly. The shared-memory-name interface for cross-process attachment is documented here as the forward plan:

- Each Weft registers a name (e.g., `weft:audio_pcm`) with the OS.
- `weft-probe --attach <name>` opens the shared-memory region and reads the control block + buffer headers.
- `weft-record --attach <name>` opens and captures.
- Kernel-path implementation deferred to Phase 6+ (requires real Android `AHardwareBuffer` or POSIX `shm_open`).
- **Fan-out ring side: REALIZED** — `weft-fanout-rec` (§1.5) attaches to POSIX `shm_open` rings by name for capture AND replay, so the cross-process interface is exercised end-to-end for the RFC-0004 ring today (gated by its selftest in the `fanout-native` CI shard).
- **Session protocol (Series 7, RFC-0011 draft): REALIZED as a core module** — `core/c/shm_ring.{h,c}` and `core/rust/src/shm.rs` promote the shm object from a bare ring to a **session**: a 64-byte `WFSH` header (magic, version, header_size, flags, payload_bytes, slot_count, ring_bytes, advisory creator pid/created_ns) followed by the byte-identical RFC-0004 ring. Attach validates the full header contract (magic/version/reserved-zero/geometry/exact size) and refuses anything else; creators unlink on destroy; the fork-inherited anonymous road and fd-passing road are first-class. `weft-fanout-rec` maps sessions THROUGH the module (selftest + e2e re-run green), and the recorder's shm objects therefore carry the header.
  - **Declared format change** (§1.5 sessions are ephemeral OS objects, not persisted files — no version migration): producers that post rings for this recorder must create sessions via the module (one call: `weft_fanout_shm_create` / `weft_shm_create_named`). A bare ring without the header is refused at attach with guidance. The zero-syscall data-path proof (strace, 50k publishes between markers) and the multi-process torture gates run in the `fanout-native` CI shard; evidence in `litmus/evidence/shm-ring/s-series.log`.

---

## 4. Scope Statement

- **Tools are C + Rust only.** TS probe/record is out of scope (roadmap: C + Rust).
- **The recorder is a protocol reader.** It is the sole reader during a capture session (Law 1 regime). It claims frames and serializes what it claimed, stale returns included. It can never block the writer (claim is one swap). Recording everything claimed — including dips — is the honest file.
- **Probe never touches payload or dead buffers.** The debug surface reads only the two live buffers' 16-byte envelope headers and the kernel's own bookkeeping words.
- **AXIOM T applies to tooling.** Probe output labels telemetry `advisory`; no tool logic branches on a telemetry counter (contracts v1.3).
