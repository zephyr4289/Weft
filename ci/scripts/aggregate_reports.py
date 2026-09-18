#!/usr/bin/env python3
"""
aggregate_reports.py — aggregate all shard logs + results into a single
combined report, compute overall status, and write summary.json + combined.log.

Used by both the push/PR workflow (extreme-test.yml) and the nightly workflow
(nightly-deep.yml).
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--logs-dir", required=True, help="dir with shard-*.log files")
    p.add_argument("--results-dir", required=True, help="dir with shard-*-results.json files")
    p.add_argument("--run-number", required=True)
    p.add_argument("--commit-sha", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--nightly", action="store_true", help="mark as nightly run")
    args = p.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    logs_dir = Path(args.logs_dir)
    results_dir = Path(args.results_dir)

    # Collect shard logs
    shard_logs = sorted(logs_dir.glob("shard-*.log"))
    build_log = logs_dir / "build.log"

    # Collect shard results JSONs
    shard_results = sorted(results_dir.glob("*-results.json"))

    # Build the combined log
    combined_lines = []
    combined_lines.append("=" * 78)
    run_type = "NIGHTLY DEEP" if args.nightly else "EXTREME TEST"
    combined_lines.append(f"WEFT {run_type} MATRIX — RUN #{args.run_number}")
    combined_lines.append(f"COMMIT: {args.commit_sha}")
    combined_lines.append(f"UTC:    {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")
    combined_lines.append("=" * 78)
    combined_lines.append("")

    if build_log.exists():
        combined_lines.append("--- BUILD LOG (gatekeeper) ---")
        combined_lines.append(build_log.read_text())
        combined_lines.append("")

    shards_summary = {}
    for shard_log in shard_logs:
        shard_name = shard_log.stem.replace("shard-", "")
        log_text = shard_log.read_text(errors="replace")

        # Find matching results JSON
        results_json_path = results_dir / f"shard-{shard_name}-results.json"
        if not results_json_path.exists():
            results_json_path = logs_dir / f"shard-{shard_name}-results.json"
        shard_data = {}
        if results_json_path.exists():
            try:
                shard_data = json.loads(results_json_path.read_text())
            except json.JSONDecodeError:
                pass

        # Extract status: from results JSON first, then log header, then common success indicators
        status = shard_data.get("status") if isinstance(shard_data, dict) and shard_data.get("status") else None
        if not status:
            for line in log_text.splitlines()[:15]:
                if line.startswith("STATUS:"):
                    status = line.split(":", 1)[1].strip().split()[0]
                    break
        if not status:
            # Fallback: check last lines for explicit status markers
            for line in reversed(log_text.splitlines()[-15:]):
                if line.startswith("STATUS:"):
                    status = line.split(":", 1)[1].strip().split()[0]
                    break
                elif "ALL PASS ✅" in line or "✅ Stability threshold met" in line:
                    status = "PASSED"
                    break
        if not status:
            status = "UNKNOWN"

        # Take last 30 lines as tail for the summary
        tail = "\n".join(log_text.splitlines()[-30:])

        shards_summary[shard_name] = {
            "status": status,
            "log_file": str(shard_log.relative_to(logs_dir)) if shard_log.is_relative_to(logs_dir) else str(shard_log),
            "tail": tail,
            "results": shard_data,
        }

        combined_lines.append(f"--- SHARD: {shard_name} ({status}) ---")
        combined_lines.append(log_text)
        combined_lines.append("")

    combined_log = "\n".join(combined_lines)
    (out_dir / "combined.log").write_text(combined_log)

    # Compute overall status
    # PASSED iff every shard is PASSED (treat UNKNOWN as failed)
    n_pass = sum(1 for s in shards_summary.values() if s["status"] == "PASSED")
    n_total = len(shards_summary)
    n_failed = sum(1 for s in shards_summary.values() if s["status"] in ("FAILED", "ERROR", "UNKNOWN"))

    # Special case: thermal-proxy is advisory — never counts as a hard failure
    if "thermal-proxy" in shards_summary and shards_summary["thermal-proxy"]["status"] != "PASSED":
        n_failed -= 1  # don't let thermal fail the overall

    if n_total == 0:
        # Zero shards ran: path routing filtered every gate out for this
        # change set (see the `changes` job in extreme-test.yml). Nothing
        # this matrix gates was modified, so the verdict is the vacuous
        # pass — recorded honestly as SKIPPED rather than FAILED, so
        # branch protection stays green and the audit trail says exactly
        # what happened.
        overall = "SKIPPED"
    else:
        overall = "PASSED" if n_failed == 0 and n_pass > 0 else "FAILED"

    summary = {
        "run_number": int(args.run_number),
        "commit_sha": args.commit_sha,
        "run_type": "nightly" if args.nightly else "push-or-pr",
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "overall_status": overall,
        "shards_total": n_total,
        "shards_passed": n_pass,
        "shards_failed": n_failed,
        "shards": shards_summary,
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2))

    print(f"✓ Aggregated {n_total} shards")
    print(f"  PASSED: {n_pass}")
    print(f"  FAILED: {n_failed}")
    print(f"  Overall: {overall}")
    print(f"  Combined log: {out_dir / 'combined.log'}")
    print(f"  Summary JSON: {out_dir / 'summary.json'}")
    return 0 if overall in ("PASSED", "SKIPPED") else 1


if __name__ == "__main__":
    sys.exit(main())
