# WO-P2-TOOLS — Phase 2: weft-probe + weft-record (Debug Tooling)

```
Directive:   weft-phase2-directive stream
Doc ID:      WO-P2-TOOLS
Version:     v1.0
From:        Staff adjudication
To:          Executor
Status:      ACTIVE — Phase 2 restored to sequence per WO-P1-CLOSURE §7.1 (2<->3 swap)
Depends:     Phase 3 CLOSED (whitepaper v1.0.1), RFC-0001 Accepted, contracts v1.3
Budget:      ~21h ≈ 2.5 engineer-days (incl. the 1h WO-P3-CLOSURE A1-A3 batch)
Kernel:      FROZEN except the single narrow surface defined in T1
```

---

## 0. Authority, context, and order of work

Roadmap 2a/2b contract: `weft-probe` (state inspector, ~400+400 LOC) and `weft-record`
(capture/replay, ~600+600 LOC), both pure C and Rust, both in-sandbox; `.weftrec` is the
project's **first non-Tier-0 artifact** and inherits the kernel public API's semver
discipline; formats documented with round-trip tests; deliverables are
`tools/weft-probe/`, `tools/weft-record/`, `tools/FORMATS.md`,
`Weft-Phase2-Tools-Report.pdf`.

**Order of work:** (0) execute the WO-P3-CLOSURE amendment batch A1–A3 and re-typeset the
whitepaper to v1.0.1 — the citable artifact must be final before tooling forks off it.
Then this WO.

**Standing decisions for this phase (staff-ruled, implement as specified):**

1. **Attachment model: in-process.** The roadmap's "connects via a shared-memory name"
   presumes cross-process kernels; the shipped kernel is in-process (pthreads / SAB
   workers). In-sandbox, both tools link the kernel and operate on the handle. The
   shared-memory-name interface is documented in FORMATS.md as the Phase 6+ forward plan —
   specified, not implemented.
2. **The recorder is a protocol reader.** The Triad is single-reader (Law 1 regime);
   a capture session therefore has the recorder as the *sole reader*: it claims frames and
   serializes what it claimed, stale returns included. It can never block the writer
   (claim is one swap). Recording everything claimed — including dips — is the honest file.
3. **Probe never touches payload or dead buffers.** The debug surface reads only the two
   live buffers' 16-byte envelope headers and the kernel's own bookkeeping words. It must
   not dereference buffers owned by nobody (freed/poisoned under I6) — a debug tool that
   causes the use-after-free L7 exists to prevent would be a Law-2/Law-4 violation in one move.
4. **TS kernel: out of scope** for tools (roadmap: C + Rust only). State that in FORMATS.md.

## 1. Tasks

### T0 — WO-P3-CLOSURE amendments (~1h)

A1–A3 applied verbatim; whitepaper re-typeset as v1.0.1; RFC-0001 appendix line added;
report the WHITEPAPER.md diff. Exit item in §4.

### T1 — Kernel debug-view surface (the one permitted API addition, ~2h)

Add a **read-only, wait-free, allocation-free** inspection accessor to both kernels:

- C: `void weft_debug_view(const weft_t *w, weft_debug_view_t *out)`
- Rust: `pub fn debug_state(&self) -> WeftDebugView` (no unsafe in public API, per roadmap 0c)

The view struct contains, per sample: `latest` (via relaxed atomic load), `w_work`, `r_work`
(exposed for inspection — they are thread-private words; reading them from another thread is
advisory-only sampling, which the doc must say), telemetry counters, and for each of the two
**live** buffers: slot index, the 16-byte envelope header (seq, version, header_size,
payload_len), and the owner designation. Rules:

- Reads of envelope headers use the same acquire ordering the kernel itself uses; a header
  sampled mid-publish may be internally inconsistent — the view reports
  `mid_publish_sample: true` with the raw bytes rather than failing. Diagnostics, not truth.
- Buffers with no live owner are reported by index/state only — **no dereference** (I6 rule).
- The view is a set of individually-consistent samples, **not** a consistent snapshot; say
  so in the doc comment and in FORMATS.md, with the AXIOM T cross-reference (telemetry
  advisory).
- Anything beyond this surface (payload reads, lock addition, protocol changes) = RFC, not
  code. Semver: minor bump on both kernels, noted in the report.

### T2 — `.weftrec` v1 format spec + `tools/FORMATS.md` FIRST (~2h)

Format is the contract; write the spec before any recorder code. Bit-exact, little-endian,
all languages forever:

```
File header (32 bytes):
  0   u32  magic "WREC"            (LE u32 0x43455257)
  4   u16  format_version = 1
  6   u16  header_size = 32
  8   u32  flags                   (all reserved, 0)
  12  u32  envelope_version        (triad-1 = 1)
  16  u32  frame_count             (patched at close; 0 => scan to EOF after crash)
  20  u32  crc32 (of bytes 0..20, CRC-32/zlib, reflected poly 0xEDB88320)
  24  u64  reserved (0)

Frame record (repeated):
  u32  rec_len                     (total record length incl. this field and crc)
  16B  envelope header verbatim    (as claimed)
  N    payload bytes               (N = envelope.payload_len)
  u32  crc32 (over envelope + payload)
```

Rules: rec_len demarcates records (forward-extensible; unknown record kinds skipped by
length, mirroring the L8 skip-unknown philosophy); semantic change to an existing field =
format_version 2, never an in-place edit; no timestamps in v1 (roadmap spec is
envelope + seq + payload; pacing belongs to the replay tool; a v2 may add them). FORMATS.md
carries: this spec, the validator's checks, the probe output contract (text default +
`--json`, one object; telemetry labeled `advisory: true` per AXIOM T), the Phase 6+
shared-memory-name forward interface (specified, not implemented), and the
"tools are C + Rust; TS out of scope" statement.

### T3 — weft-probe, C + Rust (~4h, ~400 LOC each ±20%)

- **Quiesced dump (deterministic, the correctness case):** join writer + reader, then probe.
  Output must be exact: latest, w_work, r_work, counters, both envelope headers, slot states.
  This dump is diff-able across languages — C probe on C kernel and Rust probe on Rust kernel
  must produce structurally identical JSON (same fields, same types; values may differ).
- **Live dump (advisory, the demo case):** probe against a running writer at display rate.
  Repeated samples; `mid_publish_sample` flags possible; nothing may crash, block, or
  allocate. Label the whole output `advisory`.
- Round-trip test: after N publishes + join, probe's `latest` equals the last published seq,
  and the sampled envelope header equals the writer's last envelope (payload_len, version).
- One probe crash, hang, or allocation on the hot path = RED, per failure protocol.

### T4 — weft-record C: capture + replay/validate (~4h, ~600 LOC ±20%)

- `weft-record capture <weft-args> --hz 120 --payload 64 --secs 30 [--burst N]`:
  recorder runs as the sole reader; every claim (fresh or stale) becomes one frame record.
- `weft-record replay <file.weftrec>`: validator checks —
  (i) file-header magic/version/size/header CRC; (ii) per-record CRC; (iii) envelope fields:
  magic "WEFT", version, header_size == 16, payload_len == record payload length;
  (iv) rec_len chain reaches exactly frame_count (or EOF when count==0);
  (v) emits sha256 of the concatenated record stream.
- **Byte-identical criterion (roadmap):** capture computes the stream hash while writing;
  replay recomputes it; the two must match exactly. Log both.
- Capture must degrade honestly: if a write fails mid-session, close with frame_count==0
  semantics noted (crash-tolerant scan path) and report the failure — never emit a silently
  truncated file that claims completeness.

### T5 — weft-record Rust: capture + replay/validate (~4h, ~600 LOC ±20%)

Same CLI surface, same format, same validator checks. No unsafe in public API; file I/O
via std; the CRC implementation is shared-logic-equivalent (same poly, same init/final
xor — verified by the cross-language interop in T6).

### T6 — Interop matrix + 30-second soak (~2h)

- **Format interop (the real proof the format is language-neutral):** 4 combos —
  C-capture replayed by C and by Rust; Rust-capture replayed by Rust and by C. All 4 green
  (record CRCs valid, envelope checks pass, replay hash == capture hash).
  Byte-identical *content* across languages is NOT required (timings differ); format
  acceptance is.
- **Soak:** writer at 120 Hz / 64 B payload / 30 s => ~3,600 frames per language; both
  complete; replay byte-identical; stale-return counts reported (telemetry, advisory).
- Evidence into the report: per-combo validator output, the four hashes, soak stats.

### T7 — `Weft-Phase2-Tools-Report.pdf` + doc pointers (~2h)

Same typeset pipeline as the whitepaper (pandoc + tectonic; PDF-DEFERRED fallback rule
applies). Contents: design notes, the T1 API addition + semver note, format spec summary
with pointer to FORMATS.md, interop matrix, soak evidence, LOC vs budget table, deviations
field **mandatory** (standing rule from WO-P3-CLOSURE §1). Update repo README / docs index
to reference the tools.

---

## 2. Rules (non-negotiable)

1. **AXIOM T applies to tooling.** Probe output labels telemetry `advisory`; no tool logic
   branches on a telemetry counter (contracts v1.3).
2. **Tools never mutate protocol state.** Read-only surface (T1); the recorder mutates
   nothing but its own claim path — it is a reader, exercising the same public API as any
   consumer. No back doors.
3. **No UAF by construction.** T1's no-dereference-of-unowned-buffers rule is load-bearing;
   a probe run against a revoking workload (L7-style) must survive with correct `revoked`
   reporting. Add that as a probe test case.
4. **Honest output.** A tool that cannot fulfill its contract prints the failure and exits
   non-zero; it never prints plausible-looking guesses. Diagnostics say what they are.
5. **LOC budgets are ±20% advisory; misses are declared** (standing rule).
6. Failure protocol: 07-ACCEPTANCE §6 — report the RED, not a workaround.

## 3. Success criteria (mechanical)

- [ ] Whitepaper v1.0.1 typeset (T0) before tool code lands
- [ ] Probe quiesced dump: exact and structurally identical across C and Rust
- [ ] Probe live dump: no crash/block/alloc; `mid_publish_sample` semantics demonstrated
- [ ] Probe survives a revoking workload without dereferencing poisoned/freed buffers
- [ ] `.weftrec` v1: 4/4 interop combos green (C<->C, C<->Rust, Rust<->C, Rust<->Rust)
- [ ] 30 s soak x 2 languages: replay byte-identical (capture hash == replay hash)
- [ ] FORMATS.md complete (format + probe contract + forward interface + scope statement)
- [ ] Report PDF typeset with mandatory deviations field filled

## 4. Exit checklist

- [ ] T0 A1–A3 applied; v1.0.1 delivered
- [ ] T1 debug-view accessor in both kernels; semver noted; no other kernel change
- [ ] T2 FORMATS.md + .weftrec v1 spec committed **before** recorder code
- [ ] T3 probes pass all three probe test cases
- [ ] T4 + T5 recorders/replayers pass byte-identical criterion
- [ ] T6 interop 4/4 + soak evidence archived
- [ ] T7 report + doc pointers
- [ ] Deviations field filled (LOC, margins, anything quantitative)

## 5. Budget

| Task | Estimate |
|---|---|
| T0 whitepaper amendments | 1h |
| T1 debug view (2 kernels) | 2h |
| T2 format spec + FORMATS.md | 2h |
| T3 probes (2 x ~400 LOC) | 4h |
| T4 C record/replay (~600 LOC) | 4h |
| T5 Rust record/replay (~600 LOC) | 4h |
| T6 interop + soak | 2h |
| T7 report | 2h |
| **Total** | **~21h ≈ 2.5 engineer-days** |

## 6. Out of scope

TS probe/record (roadmap: C + Rust), shared-memory-name implementation (Phase 6+),
any recorder-side protocol checking that needs writer ground truth (noted in FORMATS.md
as future work — a bare recording cannot prove no-future without a writer watermark
sidecar; v1 validates structure + byte-identity), kernel changes beyond T1, W1–W5 suite,
Phase 4 ports.

## 7. Sign-off block (executor completes)

```
WO-P2-TOOLS execution report
- T0 whitepaper v1.0.1:  [ ] diff posted
- T1 debug view:         [ ] C + Rust, semver note
- T2 FORMATS.md:         [ ] spec before code confirmed
- T3 probes:             [ ] quiesced exact | [ ] live advisory | [ ] revocation-safe
- T4/T5 record/replay:   [ ] byte-identical both languages
- T6 interop:            [ ] 4/4 combos, hashes logged | soak: [ ] 2x30s clean
- T7 report PDF:         [ ]

Deviations/findings (mandatory field — 07-ACCEPTANCE §6):
<none | list>

Sign-off:
- Executor: ______________  date: ______
- Staff review: ______________
```
