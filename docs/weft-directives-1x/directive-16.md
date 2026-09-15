# DIRECTIVE-16 — DevTools: telemetry inspector + .weftrec playback tool

- Wave: 2 · Depends: D-11 (D-15 for live attach) · Effort: ~10 h · Status: ISSUED

## 1. Context

Two developer-experience gaps: (1) no live inspector for slot state, FPS disparity, drops, and Steward leak traces; (2) `.weftrec` files (spec: FORMATS.md v1 — 32 B header, `[rec_len][16 B envelope][payload][crc32/zlib]`, L8 skip-unknown via `rec_len`) have no playback tool. The shipped soak fixtures (`litmus/evidence/soak-b2/soak_c_30s.weftrec`, `soak_rust_30s.weftrec`) are the canonical test corpus.

## 2. Tasks

- **T16.1 Telemetry inspector (web).** Consumes the debug-view accessor (the one sanctioned kernel API) + probe JSON lines over a WebSocket bridge. Panels:
  - slot indices `w_work` / `r_work` / `latest` as a live strip chart;
  - writer vs reader FPS disparity graph;
  - `t_drop` counters (per-reader) with event markers;
  - Steward leak traces: GC-platform reclaim events + epoch/revocation timeline (I6 handshake visualized).
- **T16.2 Cross-check mode.** The inspector must be able to run headless against the same process the CLI probe observes, and its counters must match the probe's output for the same run — the probe remains the correctness reference (telemetry advisory, per standing laws), and matching counts are the evidence.
- **T16.3 Playback tool.** CLI + minimal web UI for `.weftrec`:
  - structural validation per FORMATS.md (crc32 verify per frame, `rec_len` walk, L8 skip-unknown demonstrated on a v2-foreign frame injected in tests);
  - scrub timeline + single-step; stale-frame highlighting; envelope field inspection (LE decode display);
  - the Phase-2 file validator (`tools/` structural validator) is reused, not forked.
- **T16.4 Roundtrip gate.** record 10 s → replay → frame count and per-frame CRC sequence byte-identical (the WO-P2 T4/T5 capture==replay rule, now applied to the tool).

## 3. Non-goals

No kernel diffs. No new recording format (v1 stays; extensions go through RFC). No timeline persistence/analytics backend. The inspector never mutates protocol state (tools-are-read-only canon).

## 4. Acceptance criteria (mechanical)

1. Inspector attached to a running D-15 C-mode cell for 60 s: slot-index series and `t_drop` match CLI probe output for the same window (diff log == empty; scope + window stated).
2. Both shipped soak-b2 fixtures: structural validation PASS (crc per frame, count reported — expected counts printed from file headers, not asserted from memory).
3. Roundtrip: capture sha256 == replay sha256 on a 10 s recording (`linux-sandbox` tag).
4. L8: injected foreign-frame test — parser skips via `rec_len`, logs skip events, total walk length exact.
5. Inspector is read-only: no write syscalls/protocol mutations possible from UI code (code-path audit note + test).

## 5. Evidence to return

`evidence/D-16/`: probe-vs-inspector diff log, validation outputs ×2 fixtures, roundtrip hashes, L8 skip test log, audit note.

## 6. Report

`reports/D-16-REPORT.md` per index §4.
