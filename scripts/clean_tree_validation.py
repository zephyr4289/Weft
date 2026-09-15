#!/usr/bin/env python3
"""
clean_tree_validation.py — Phase 5 / T7: Clean-tree validation per
WO-P5-RELEASE §1.T7.

Unpack the tarball into an empty directory; run all five make targets in
sequence; log every exit code. Gate: litmus 24/24 exit 0; bench green
(both suites); site regenerates byte-stable; validate 4/4 exit 0. Any
RED → failure protocol, no workaround.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

TARBALL = Path("/home/z/my-project/download/weft-sandbox-v0.1.tar.gz")
WORK_DIR = Path("/home/z/my-project/scripts/_clean_tree")
LOG_FILE = Path("/home/z/my-project/upload/weft-docs/weft-docs/weft/litmus/evidence/clean-tree-validation.log")


def run_target(target: str) -> dict:
    """Run `make <target>` in the clean tree, return exit code + output."""
    print(f"\n→ make {target}")
    start = time.time()
    proc = subprocess.run(
        ["make", target],
        cwd=str(WORK_DIR / "weft"),
        capture_output=True, text=True, timeout=1800  # 30 min per target max
    )
    elapsed = time.time() - start
    last_lines = (proc.stdout + proc.stderr).splitlines()[-10:]
    print(f"  exit {proc.returncode} in {elapsed:.1f}s")
    for line in last_lines:
        print(f"  | {line}")
    return {
        "target": target,
        "exit_code": proc.returncode,
        "elapsed_s": round(elapsed, 1),
        "tail_output": "\n".join(last_lines),
    }


def main():
    if not TARBALL.exists():
        print(f"!! Tarball not found: {TARBALL}")
        sys.exit(1)

    if WORK_DIR.exists():
        shutil.rmtree(WORK_DIR)
    WORK_DIR.mkdir(parents=True)

    # Unpack
    print(f"→ unpacking {TARBALL} into {WORK_DIR}")
    r = subprocess.run(
        ["tar", "-xzf", str(TARBALL), "-C", str(WORK_DIR)],
        capture_output=True, text=True
    )
    if r.returncode != 0:
        print(f"!! tar extract failed: {r.stderr}")
        sys.exit(1)
    print(f"  unpacked → {WORK_DIR / 'weft'}")

    # Run all five make targets
    targets = ["build", "litmus", "bench", "site", "validate"]
    results = []
    for t in targets:
        results.append(run_target(t))

    # Site determinism check (regenerate and compare)
    print("\n→ site determinism check (regenerate and compare)")
    site_index = WORK_DIR / "weft" / "bench" / "site" / "index.html"
    if not site_index.exists():
        print("  !! index.html missing — site target failed")
        results.append({"target": "site-determinism", "exit_code": 1, "elapsed_s": 0, "tail_output": "index.html missing"})
    else:
        first_sha = hashlib.sha256(site_index.read_bytes()).hexdigest()
        subprocess.run(["make", "site"], cwd=str(WORK_DIR / "weft"), capture_output=True, text=True)
        second_sha = hashlib.sha256(site_index.read_bytes()).hexdigest()
        match = first_sha == second_sha
        print(f"  first:  {first_sha[:16]}...")
        print(f"  second: {second_sha[:16]}...")
        print(f"  match:  {match}")
        results.append({
            "target": "site-determinism",
            "exit_code": 0 if match else 1,
            "elapsed_s": 0,
            "tail_output": f"first={first_sha} second={second_sha} match={match}"
        })

    # Print summary
    print("\n" + "=" * 78)
    print("CLEAN-TREE VALIDATION SUMMARY")
    print("=" * 78)
    all_pass = True
    for r in results:
        status = "PASS" if r["exit_code"] == 0 else "FAIL"
        if r["exit_code"] != 0:
            all_pass = False
        print(f"  {r['target']:25s}  exit={r['exit_code']:3d}  elapsed={r['elapsed_s']:6.1f}s  [{status}]")
    print()
    print(f"  Overall: {'PASS' if all_pass else 'FAIL'}")

    # Write log
    LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
    import json
    LOG_FILE.write_text(
        f"# Clean-tree validation log\n"
        f"# Tarball: {TARBALL}\n"
        f"# Tarball sha256: {hashlib.sha256(TARBALL.read_bytes()).hexdigest()}\n"
        f"# Work dir: {WORK_DIR}\n"
        f"# UTC: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n\n"
        + json.dumps({"results": results, "overall_pass": all_pass}, indent=2)
    )
    print(f"\n✓ Log: {LOG_FILE}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
