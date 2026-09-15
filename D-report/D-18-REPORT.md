# D-18 Report: Release Engineering — Artifact Matrix & Manifest Verification

## 1. Executive Summary
Directive 18 delivers the automated, reproducible multi-platform release engineering pipeline for Weft. All release artifacts are packaged across Linux, Android, Apple/iOS, npm, and Flutter with release-time `SHA256SUMS` generation and Minisign signature verification.

The **Reproducibility Gate** was verified: two independent clean packaging runs produced bit-identical source tarballs (`8163c6ddb8a23f5aef564c834b346d9db406ea7a3f3d26ed5617ffc3311acb83`).

---

## 2. Release Artifact Matrix & Verification

### Artifacts in `dist/v0.1.0-rc1/`
| Artifact Name | Platform / Target | Verification Status |
| :--- | :--- | :---: |
| `weft-v0.1.0-rc1.tar.gz` | Canonical Reproducible Source Tarball | `sha256sum -c OK` |
| `weft-tools-linux-x86_64.tar.gz` | Static Linux Binaries (`spike`, `weft_record`, `weft_probe`, `weft_play`) | `sha256sum -c OK` |
| `weft-android-0.1.0.tar.gz` | Android AAR Matrix (`dev.weft:weft-core`, `weft-compose`, `weft-bom`) | `sha256sum -c OK` |
| `Weft-xcframework-0.1.0.tar.gz` | Apple XCFramework / Swift Package | `sha256sum -c OK` |
| `weft-core-0.1.0.tgz` | NPM `@weft/core` package | `sha256sum -c OK` |
| `weft-react-0.1.0.tgz` | NPM `@weft/react` package | `sha256sum -c OK` |
| `weft-vue-0.1.0.tgz` | NPM `@weft/vue` package | `sha256sum -c OK` |
| `weft-svelte-0.1.0.tgz` | NPM `@weft/svelte` package | `sha256sum -c OK` |
| `weft-react-native-0.1.0.tgz` | NPM `@weft/react-native` package | `sha256sum -c OK` |
| `flutter_weft-0.1.0.tar.gz` | Flutter Dart Pub package | `sha256sum -c OK` |

---

## 3. Reproducibility & Manifest Verification
1. **Reproducibility Gate**:
   - Run 1 SHA-256: `8163c6ddb8a23f5aef564c834b346d9db406ea7a3f3d26ed5617ffc3311acb83`
   - Run 2 SHA-256: `8163c6ddb8a23f5aef564c834b346d9db406ea7a3f3d26ed5617ffc3311acb83`
   - Difference: **0 bytes (100% bit-exact match)**
2. **Release Notes Auto-Filling**:
   - `dist/v0.1.0-rc1/RELEASE-NOTES.md` and `evidence/D-18/RELEASE-NOTES.md` automatically populated with the actual tarball hash and the full SHA-256 manifest table.
3. **Minisign Ed25519 Signature**:
   - `SHA256SUMS.minisig` signed with genuine Ed25519 keypair (`tools/minisign_tool.py`).
   - Public key committed in root as `minisign.pub` and verified clean against manifest.

---

## 4. Evidence Artifacts
- `evidence/D-18/SHA256SUMS`: Complete SHA-256 manifest over all artifacts.
- `evidence/D-18/SHA256SUMS.minisig`: Genuine Ed25519 Minisign signature.
- `evidence/D-18/minisign.pub`: Minisign public key.
- `evidence/D-18/RELEASE-NOTES.md`: Generated release notes with exact hashes.
- `evidence/D-18/reproducibility_pair_hashes.txt`: Two-run identical checksum proof.

---

## 5. Compliance & Invariant Checklist
- [x] Kernel Freeze: `core/c/weft.{c,h}` and `core/rust/src/lib.rs` unmodified (0 diffs).
- [x] Release packaging pipeline executed and verified (`sha256sum -c` 10/10 OK).
- [x] Tarball reproducibility gate verified bit-identical.
- [x] Auto-filled release notes generated.
- [x] Structural validator: `python3 tools/port_validator.py --target all` PASS.
