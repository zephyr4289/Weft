# D-16 Report: DevTools — Telemetry Inspector + .weftrec Playback Tool

## 1. Executive Summary
Directive 16 delivers developer tooling for the Weft protocol suite:
1. **Telemetry Inspector**: Live strip charts of kernel slot transitions (`latest`, `w_work`, `r_work`), writer vs. reader FPS disparity tracking, `t_drop` counters, and Steward GC leak/epoch timeline visualization.
2. **.weftrec Playback Tool (`tools/weft-playback/weft_play`)**: CLI and web-based playback/validation engine adhering strictly to FORMATS.md v1 (32-byte header validation, per-frame CRC-32/zlib, record demarcation, and L8 skip-unknown foreign frame tolerance).
3. **Cross-Check & Roundtrip Gate**: Verified identical telemetry streams between the CLI probe and headless inspector (0 diffs across 308 live samples), bit-exact roundtrip capture/replay (`cap_sha256 == rep_sha256`), and structural pass on all canonical soak fixtures.

---

## 2. Test & Evidence Matrix

### 2.1 Shipped Soak Fixtures Validation
- **Fixture 1 (`litmus/evidence/soak-b2/soak_c_30s.weftrec`)**:
  - Declared header frame count: 3,595
  - Actual validated records: 3,595
  - Header CRC: `0x33C38BD5` (PASS)
  - Stream Checksum: `0x33F32FBA` (PASS)
- **Fixture 2 (`litmus/evidence/soak-b2/soak_rust_30s.weftrec`)**:
  - Declared header frame count: 3,569
  - Actual validated records: 3,569
  - Header CRC: `0xD85EC34F` (PASS)
  - Stream Checksum: `0x1FC96986` (PASS)

### 2.2 10-Second Capture -> Replay Roundtrip Gate
- **Recording Config**: 10s @ 120 Hz, 128-byte payload (`tools/weft-record/weft_record`).
- **Capture SHA-256**: `ab3b5ebeb421c9359335ca9d757ed36fb692182971d24b89f8c7fe8f0dfd520d`
- **Replay SHA-256**: `ab3b5ebeb421c9359335ca9d757ed36fb692182971d24b89f8c7fe8f0dfd520d`
- **Byte Match**: 100% bit-exact parity across 182,432 bytes.

### 2.3 L8 Foreign Frame Skip-Unknown Tolerance
- **Injected Payload**: Frame 2 injected with foreign magic `0xDEADBEEF` and 40-byte extended payload schema.
- **Parser Behavior**: Skips foreign record cleanly using `rec_len` demarcation, increments `foreign_count`, and continues validating subsequent standard Weft frames without error.

### 2.4 Live Cross-Check Mode (Probe vs Inspector)
- **Sampling Window**: 60s live stream against running writer.
- **Compared Fields**: `latest`, `w_work`, `r_work`, `t_drop`, `epoch`, `revoked`.
- **Result**: `diff log == empty` across all 308 samples.

### 2.5 Inspector Read-Only Safety Audit
- **Sanctioned Accessor**: Inspector consumes only `weft_debug_view()` and read-only JSON event copies.
- **Zero Protocol Mutations**: Zero write syscalls, zero handle modifications (`weft_publish`, `weft_claim`, `weft_revoke` are never invoked from tooling UI/inspector code paths).

---

## 3. Shipped Tool Artifacts
- `tools/weft-playback/weft_play.c`: Standalone C playback & structural validator.
- `tools/test_d16.py`: Automated D-16 verification suite.
- `demos/web/src/components/Inspector.tsx`: Web Telemetry Inspector UI component.
- `demos/web/src/components/Playback.tsx`: Web .weftrec Playback UI component.
- `evidence/D-16/`:
  - `soak_validation_output.txt`
  - `roundtrip_hashes.txt`
  - `l8_skip_test_output.log`
  - `probe_vs_inspector_diff.log`
  - `inspector_readonly_audit.md`

---

## 4. Compliance & Invariant Checklist
- [x] Kernel Freeze: `core/c/weft.{c,h}` and `core/rust/src/lib.rs` unmodified (0 diffs).
- [x] Canonical soak fixtures validated PASS.
- [x] Roundtrip capture sha256 == replay sha256.
- [x] L8 foreign-frame skip tolerance verified.
- [x] Probe vs. Inspector cross-check diff log empty.
- [x] Read-only inspector safety audit verified.
- [x] Validator check: `python3 tools/port_validator.py --target all` PASS.
