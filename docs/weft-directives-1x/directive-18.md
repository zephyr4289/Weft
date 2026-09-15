# DIRECTIVE-18 — Release engineering: tag → artifact matrix → GitHub Releases

- Wave: 3 · Depends: D-10…D-14 (D-15/16 optional inputs) · Effort: ~6 h · Status: ISSUED

## 1. Context

D-10's CI skeleton proves the tree; this directive makes releases boring and reproducible. The P5-W5 lesson is encoded: **artifact self-hashes are stale by construction** — all hashes live in release-time-generated `SHA256SUMS`, never inside artifacts.

## 2. Tasks

- **T18.1 Tag-triggered workflow.** `.github/workflows/release.yml` on `v*` tags:
  - linux: `spike`, `weft-record`, `weft-probe` binaries (static, release strips) + tarball via `make dist` (D-10);
  - android: AAR bundle from D-12 (`publishToMavenLocal` artifacts collected; Central upload remains credential-gated);
  - ios: `Weft` xcframework zip from D-13;
  - npm: `changesets version` + `npm publish --provenance` on tag (credential-gated; dry-run mode until staff go);
  - flutter: `weft_flutter` pub artifact from D-14 (same gating);
  - pdf: compile whitepaper/report PDFs from sources if the toolchain is in CI; otherwise attach the adjudicated canonical PDFs (v1.0.4 `6f3a3211…` etc. — never rebuilt silently).
- **T18.2 Manifest.** Release-time `SHA256SUMS` over all artifacts + `minisign` signature (key held by release owner). Release notes generated from `RELEASE-NOTES.md` + per-package changelogs; the tarball's hash line in the notes is filled by the workflow, not by a human.
- **T18.3 Reproducibility gate.** Two CI runs of the same tag → byte-identical tarball (the D-10 `make dist` property, now proven in CI); any non-reproducible artifact is listed with its nondeterminism source declared (e.g. PDF timestamps) or excluded.
- **T18.4 Dry-run release.** Tag `v0.1.0-rc1`: full pipeline, artifacts attached to a pre-release, nothing published to registries.

## 3. Non-goals

No registry publishes without staff go + credentials. No code-signing beyond minisign (platform store signing is post-v0.1.0). No website (weft.dev stays deferred per WO-P5 ruling).

## 4. Acceptance criteria (mechanical)

1. `v0.1.0-rc1` dry-run: complete artifact matrix attached (list printed), `SHA256SUMS` present and matching every artifact (`sha256sum -c` clean).
2. Tarball reproducibility: two runs → identical sha256 (both printed in evidence).
3. Release notes: hash line auto-filled == actual tarball hash (diff check).
4. minisign signature verifies against the shipped public key.
5. Workflow runs green end-to-end ≤ 45 min (`linux-ci` tag) or slow steps are declared with durations.

## 5. Evidence to return

`evidence/D-18/`: workflow logs, artifact listing + SHA256SUMS, signature file + verification output, reproducibility pair hashes, release-notes diff.

## 6. Report

`reports/D-18-REPORT.md` per index §4.
