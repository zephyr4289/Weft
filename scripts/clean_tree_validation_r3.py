#!/usr/bin/env python3
"""
clean_tree_validation_r3.py — Phase 5 / R3: Full clean-tree re-validation.

Per WO-P5-VERIFICATION §4 R3: fresh empty-dir unpack of the FINAL tarball,
all five make targets, full per-target output archived (not tails), single
provenance, every exit code logged.

Expected: litmus 24/24 (or honest EXPOSURE-SHORTFALL), bench green with
16b5c663 restored, site deterministic, validate 4/4.
"""
import hashlib
import os
os.environ["PATH"] = os.path.expanduser("~/.cargo/bin") + ":" + os.environ.get("PATH", "")
import shutil
import subprocess
import sys
import time
from pathlib import Path

TARBALL = Path("/home/z/my-project/download/weft-sandbox-v0.1.tar.gz")
WORK_DIR = Path("/home/z/my-project/scripts/_clean_tree_r3")
LOG_DIR = Path("/home/z/my-project/upload/weft-docs/weft-docs/weft/litmus/evidence/clean-tree-r3")
LOG_DIR.mkdir(parents=True, exist_ok=True)


def run_target(target: str) -> dict:
    """Run `make <target>` in the clean tree, capture FULL output to a log file."""
    print(f"\n→ make {target}")
    start = time.time()
    log_file = LOG_DIR / f"{target}.log"
    with open(log_file, "w") as lf:
        lf.write(f"# make {target}\n")
        lf.write(f"# Start: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")
        lf.write(f"# Working dir: {WORK_DIR / 'weft'}\n")
        lf.write(f"# Tarball: {TARBALL}\n\n")
        lf.flush()
        proc = subprocess.run(
            ["make", target],
            cwd=str(WORK_DIR / "weft"),
            capture_output=True, text=True, timeout=1800
        )
        lf.write(proc.stdout)
        lf.write("\n--- STDERR ---\n")
        lf.write(proc.stderr)
    elapsed = time.time() - start
    last_lines = (proc.stdout + proc.stderr).splitlines()[-5:]
    print(f"  exit {proc.returncode} in {elapsed:.1f}s (full log: {log_file})")
    for line in last_lines:
        print(f"  | {line}")
    return {
        "target": target,
        "exit_code": proc.returncode,
        "elapsed_s": round(elapsed, 1),
        "log_file": str(log_file.relative_to(Path("/home/z/my-project/upload/weft-docs/weft-docs/weft"))),
        "log_size_bytes": log_file.stat().st_size,
    }


def main():
    if not TARBALL.exists():
        print(f"!! Tarball not found: {TARBALL}")
        sys.exit(1)

    if WORK_DIR.exists():
        shutil.rmtree(WORK_DIR)
    WORK_DIR.mkdir(parents=True)

    # Unpack (single provenance: only the tarball)
    print(f"→ unpacking {TARBALL} into {WORK_DIR}")
    r = subprocess.run(
        ["tar", "-xzf", str(TARBALL), "-C", str(WORK_DIR)],
        capture_output=True, text=True
    )
    if r.returncode != 0:
        print(f"!! tar extract failed: {r.stderr}")
        sys.exit(1)
    print(f"  unpacked → {WORK_DIR / 'weft'}")

    # Run all five make targets (single provenance log per target)
    targets = ["build", "litmus", "bench", "site", "validate"]
    results = []
    for t in targets:
        results.append(run_target(t))

    # Site determinism check
    print("\n→ site determinism check (regenerate and compare)")
    site_index = WORK_DIR / "weft" / "bench" / "site" / "index.html"
    if not site_index.exists():
        print("  !! index.html missing — site target failed")
        results.append({"target": "site-determinism", "exit_code": 1, "elapsed_s": 0, "log_file": "n/a"})
    else:
        first_sha = hashlib.sha256(site_index.read_bytes()).hexdigest()
        subprocess.run(["make", "site"], cwd=str(WORK_DIR / "weft"), capture_output=True, text=True)
        second_sha = hashlib.sha256(site_index.read_bytes()).hexdigest()
        match = first_sha == second_sha
        print(f"  first:  {first_sha}")
        print(f"  second: {second_sha}")
        print(f"  match:  {match}")
        results.append({
            "target": "site-determinism",
            "exit_code": 0 if match else 1,
            "elapsed_s": 0,
            "log_file": "inline",
        })

    # Verify canonical bundle hash
    print("\n→ canonical bundle hash check (R1 Path C verification)")
    canon_file = WORK_DIR / "weft" / "bench" / "results.json"
    if canon_file.exists():
        canon_sha = hashlib.sha256(canon_file.read_bytes()).hexdigest()
        match_16b5 = canon_sha.startswith("16b5c663")
        print(f"  bench/results.json sha256: {canon_sha}")
        print(f"  matches 16b5c663: {match_16b5}")
        results.append({
            "target": "canonical-hash-check",
            "exit_code": 0 if match_16b5 else 1,
            "elapsed_s": 0,
            "log_file": "inline",
            "sha256": canon_sha,
        })

    # Print summary
    print("\n" + "=" * 78)
    print("CLEAN-TREE RE-VALIDATION SUMMARY (R3)")
    print("=" * 78)
    all_pass = True
    for r in results:
        status = "PASS" if r["exit_code"] == 0 else "FAIL"
        if r["exit_code"] != 0:
            all_pass = False
        log_size = r.get("log_size_bytes", 0)
        print(f"  {r['target']:25s}  exit={r['exit_code']:3d}  log={log_size:6d}B  [{status}]")
    print()
    print(f"  Overall: {'PASS' if all_pass else 'FAIL'}")
    print(f"  Single provenance: all logs at {LOG_DIR}")
    print(f"  Full per-target output archived (not tails)")

    # Write summary log
    import json
    summary = {
        "directive": "WO-P5-VERIFICATION §4 R3",
        "tarball": str(TARBALL),
        "tarball_sha256": hashlib.sha256(TARBALL.read_bytes()).hexdigest(),
        "work_dir": str(WORK_DIR),
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "results": results,
        "overall_pass": all_pass,
        "provenance": "single — every target ran in the same clean tree, log per target",
    }
    summary_path = LOG_DIR / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2))
    print(f"\n✓ Summary: {summary_path}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
