#!/usr/bin/env python3
"""
Weft Studio — webapp embed sync (Pillar 7).

Copies the canonical managed sources from packages/studio/src/ into the live
webapp (src/studio/) and emits a SHA-256 manifest of every synced file.
Stage 1 of the managed suite verifies byte parity between the two trees, so
the live web demonstrably embeds the exact reviewed sources.

Usage: python3 tools/studio/sync_webapp.py [--webapp /home/z/my-project]
"""

import argparse
import hashlib
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "packages" / "studio" / "src"
DEFAULT_WEBAPP = REPO.parent  # the webapp IS the parent of the Weft repo checkout


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--webapp", default=str(DEFAULT_WEBAPP))
    ap.add_argument("--check", action="store_true",
                    help="verify parity only (no writes); exit 2 on drift")
    args = ap.parse_args()

    webapp = Path(args.webapp)
    dst = webapp / "src" / "studio"
    if not SRC.is_dir():
        print(f"E_SEAM: canonical source missing: {SRC}", file=sys.stderr)
        return 2

    wanted: dict[str, Path] = {}
    for p in sorted(SRC.rglob("*")):
        if p.is_file():
            wanted[str(p.relative_to(SRC))] = p

    if args.check:
        bad = []
        for rel, src_path in wanted.items():
            dp = dst / rel
            if not dp.is_file():
                bad.append(f"missing: {rel}")
            elif sha256(src_path) != sha256(dp):
                bad.append(f"drift:   {rel}")
        extra = [str(p.relative_to(dst)) for p in dst.rglob("*")
                 if p.is_file() and str(p.relative_to(dst)) not in wanted
                 and p.name != "EMBED-MANIFEST.sha256"]
        for e in extra:
            bad.append(f"extra:   {e}")
        if bad:
            print("EMBED PARITY FAIL:")
            for b in bad:
                print("  " + b)
            return 2
        print(f"EMBED PARITY OK — {len(wanted)} files byte-identical to packages/studio/src")
        return 0

    # sync
    if dst.exists():
        shutil.rmtree(dst)
    for rel, src_path in wanted.items():
        dp = dst / rel
        dp.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src_path, dp)
    lines = [f"{sha256(src_path)}  {rel}" for rel, src_path in wanted.items()]
    (dst / "EMBED-MANIFEST.sha256").write_text("\n".join(lines) + "\n")
    print(f"synced {len(wanted)} files -> {dst}")
    print(f"manifest -> {dst / 'EMBED-MANIFEST.sha256'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
