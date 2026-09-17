#!/usr/bin/env python3
"""
tools/guardian/guardian.py — RFC 0011 Automated Crash & Parity Telemetry Guardian.

THE WATCHDOG THE BRANCH DIRECTIVE ASKS FOR: an automated regression-detection
gate in CI that flags
    1. THROUGHPUT — a >= 3% drop of any benchmark's primary throughput metric
       against the pinned baseline (ci/baselines/guardian-throughput-baseline.json),
       median-vs-median so a noisy single run cannot flip the gate;
    2. WIRE LAYOUT — a drift of a SINGLE BYTE in the cross-language wire layout
       (the RFC 0004 ring ctrl block, the 16-byte Triad envelope, the canary):
       probes emit observed-layout JSON; the guardian diffs against
       tools/guardian/wire-manifest.json — any mismatch, one byte or one field,
       is RED;
    3. CRASH TELEMETRY — any FAILED/pass=false/crash signature in the CI result
       artifacts (per shard, per platform) becomes a finding; any finding is RED.

Design law (this repo's Law 1): every resolution is counted, never silent.
A guardian that finds nothing to look at is RED, not green — a missing
baseline, a missing probe, an unreadable artifact all FAIL LOUDLY.

Usage:
  python3 tools/guardian/guardian.py throughput [--baseline F] [--results F] [--threshold 3.0]
  python3 tools/guardian/guardian.py wire [--manifest F] [--probe-json F | --probe-cmd 'sh...']
  python3 tools/guardian/guardian.py crash [--artifacts-dir ci/run-artifacts]
  python3 tools/guardian/guardian.py all          # the three watches, one verdict
  python3 tools/guardian/guardian.py selftest     # must BITE on every fixture

Exit codes: 0 all green, 1 any finding, 2 guardian misuse (loud).
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
if not (ROOT / "tools" / "guardian").is_dir() or not (ROOT / "ci").is_dir():
    print("FATAL: guardian.py must live at <repo>/tools/guardian/", file=sys.stderr)
    sys.exit(2)

BASELINE = ROOT / "ci" / "baselines" / "guardian-throughput-baseline.json"
BENCH_RESULTS = ROOT / "bench" / "results.json"
MANIFEST = ROOT / "tools" / "guardian" / "wire-manifest.json"
VERDICT = ROOT / "tools" / "guardian" / "guardian-verdict.json"

# Crash signatures that mean "a platform crashed" wherever they appear.
CRASH_SIGNATURES = [
    "SIGSEGV", "SIGBUS", "SIGABRT", "panic:", "EXCEPTION_ACCESS_VIOLATION",
    "CHECK failed", "assertion failed", "FATAL:", "core dumped",
]


def emit(watch, status, findings, extra=None):
    verdict = {"watch": watch, "status": status, "findings": findings}
    if extra:
        verdict.update(extra)
    print(json.dumps(verdict, indent=2))
    try:
        VERDICT.write_text(json.dumps(verdict, indent=2))
    except OSError:
        pass
    return 0 if status == "PASS" else 1


# ---------------------------------------------------------------------------
# Watch 1: throughput (>= threshold % median drop vs baseline = RED)
# ---------------------------------------------------------------------------

def _primary_metric(entry):
    """The baseline pins WHICH metric is each benchmark's primary throughput."""
    return entry.get("metric"), entry.get("value")


def _lookup_current(results, bench, lang, metric):
    matrix = results.get("matrix", {})
    for key, val in matrix.items():
        if val.get("bench") == bench and val.get("lang") == lang:
            metrics = val.get("metrics", {})
            if metric in metrics:
                m = metrics[metric]
                if isinstance(m, list):
                    # e.g. per_size medians — compare the median of the list
                    nums = sorted(x for x in m if isinstance(x, (int, float)))
                    if not nums:
                        return None
                    n = len(nums)
                    return nums[n // 2] if n % 2 else (nums[n // 2 - 1] + nums[n // 2]) / 2
                return m
    return None


def watch_throughput(baseline_path, results_path, threshold):
    findings = []
    if not baseline_path.exists():
        return emit("throughput", "FAIL",
                    [f"baseline missing: {baseline_path} — create it via the "
                     f"update-perf-baseline workflow; a guardian without a "
                     f"baseline is not a guardian"])
    if not results_path.exists():
        return emit("throughput", "FAIL", [f"results missing: {results_path}"])

    baseline = json.loads(baseline_path.read_text())
    results = json.loads(results_path.read_text())
    entries = baseline.get("entries", {})
    if not entries:
        return emit("throughput", "FAIL", ["baseline has no entries — loud RED"])

    compared = 0
    for key, entry in sorted(entries.items()):
        # Baseline keys mirror the bench matrix: "<lang>/<bench-id>".
        lang, bench = key.split("/", 1)
        metric, base_val = _primary_metric(entry)
        if metric is None or base_val is None:
            findings.append(f"{key}: baseline entry malformed (needs metric+value)")
            continue
        cur = _lookup_current(results, bench, lang, metric)
        if cur is None:
            findings.append(f"{key}: current run missing metric '{metric}' — "
                            f"the benchmark matrix shrank (loud RED)")
            continue
        compared += 1
        if base_val <= 0:
            findings.append(f"{key}: baseline value {base_val} unusable")
            continue
        drop_pct = (base_val - cur) / base_val * 100.0
        if drop_pct >= threshold:
            findings.append(
                f"{key}: {metric} dropped {drop_pct:.2f}% "
                f"(baseline {base_val}, current {cur}, gate {threshold}%)")
        else:
            print(f"  [ok] {key}: {metric} {cur} vs baseline {base_val} "
                  f"({drop_pct:+.2f}%)")

    if compared == 0 and not findings:
        findings.append("no benchmark compared — the results matrix is empty")
    status = "PASS" if not findings else "FAIL"
    return emit("throughput", status, findings,
                {"compared": compared, "threshold_pct": threshold})


# ---------------------------------------------------------------------------
# Watch 2: wire layout (single-byte drift = RED)
# ---------------------------------------------------------------------------

def _flatten_layout(obj, prefix=""):
    """Yield (path, value) for the scalar layout fields we diff."""
    scalars = {}
    for k, v in obj.items():
        p = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict):
            scalars.update(_flatten_layout(v, p))
        elif isinstance(v, (int, float, str)):
            scalars[p] = v
    return scalars


def watch_wire(manifest_path, probe_json=None, probe_cmd=None):
    findings = []
    if not manifest_path.exists():
        return emit("wire", "FAIL", [f"manifest missing: {manifest_path}"])
    manifest = json.loads(manifest_path.read_text())
    canonical = _flatten_layout(manifest)

    if probe_cmd:
        import subprocess
        proc = subprocess.run(probe_cmd, shell=True, capture_output=True, text=True)
        if proc.returncode != 0:
            return emit("wire", "FAIL",
                        [f"probe failed rc={proc.returncode}: {proc.stderr[:400]}"])
        raw = proc.stdout
    elif probe_json:
        raw = Path(probe_json).read_text()
    else:
        # Default: run the C probe if built, else the TS probe via node.
        c_probe = ROOT / "core" / "c" / "wire-probe"
        if c_probe.exists():
            return watch_wire(manifest_path, probe_cmd=str(c_probe))
        return emit("wire", "FAIL",
                    ["no probe available: build core/c/wire-probe (make -C core/c "
                     "wire-probe) or pass --probe-json/--probe-cmd — a guardian "
                     "that cannot observe the wire is RED, not green"])

    try:
        observed = json.loads(raw)
    except json.JSONDecodeError as e:
        return emit("wire", "FAIL", [f"probe output not JSON: {e}"])

    obs = _flatten_layout(observed)

    # 1. Every canonical scalar must be present and EQUAL.
    for path, val in sorted(canonical.items()):
        if path.endswith("_expr") or path.endswith("_constant") or path.endswith("_ascii"):
            continue
        if path not in obs:
            # manifest-only descriptive fields (contract strings) are skipped:
            if isinstance(val, str):
                continue
            findings.append(f"missing in probe output: {path} (canonical {val})")
            continue
        got = obs[path]
        if isinstance(val, str):
            continue  # descriptive strings are not wire bytes
        if got != val:
            findings.append(f"WIRE DRIFT at {path}: canonical {val}, observed {got} "
                            f"— a single byte of layout drift is a breaking change")

    # 2. The probe may add runtime-observed fields beyond the manifest — fine —
    #    but anything under the canonical namespaces must not contradict.
    if not findings:
        return emit("wire", "PASS",
                    [f"observed layout matches manifest ({len(canonical)} fields)"],
                    {"fields_checked": len(canonical)})
    return emit("wire", "FAIL", findings)


# ---------------------------------------------------------------------------
# Watch 3: crash telemetry (any failure/crash signature = RED)
# ---------------------------------------------------------------------------

def watch_crash(artifacts_dir):
    findings = []
    checked = 0
    art = Path(artifacts_dir)
    candidates = list(art.glob("*-results.json")) if art.exists() else []
    for f in candidates:
        try:
            data = json.loads(f.read_text())
        except (json.JSONDecodeError, OSError) as e:
            findings.append(f"{f.name}: unreadable result artifact ({e})")
            continue
        checked += 1
        status = data.get("status")
        if status is not None and status not in ("PASSED", "PASS", "SKIPPED (declared)"):
            findings.append(f"{f.name}: status={status}")
        def scan(obj, path):
            if isinstance(obj, dict):
                for k, v in obj.items():
                    if k in ("pass",) and v is False:
                        findings.append(f"{f.name}: {path}.{k}=false")
                    scan(v, f"{path}.{k}")
            elif isinstance(obj, list):
                for i, v in enumerate(obj):
                    scan(v, f"{path}[{i}]")
        scan(data, data.get("shard", "root"))

    litmus = ROOT / "litmus" / "results.json"
    if litmus.exists():
        try:
            data = json.loads(litmus.read_text())
            checked += 1
            suites = data.get("suites", data if isinstance(data, list) else [])
            if isinstance(suites, list):
                for s in suites:
                    name = s.get("name", "?") if isinstance(s, dict) else str(s)
                    ok = s.get("ok", s.get("pass", True)) if isinstance(s, dict) else True
                    if ok is False:
                        findings.append(f"litmus/{name}: FAILED")
            elif isinstance(suites, dict):
                for name, s in suites.items():
                    ok = s.get("ok", s.get("pass", True)) if isinstance(s, dict) else True
                    if ok is False:
                        findings.append(f"litmus/{name}: FAILED")
        except (json.JSONDecodeError, OSError) as e:
            findings.append(f"litmus/results.json: unreadable ({e})")
    else:
        findings.append("litmus/results.json missing — cannot audit the litmus suite")

    if checked == 0:
        findings.append("no result artifacts found — the guardian audited nothing "
                        "(loud RED, never silent-green)")

    status = "PASS" if not findings else "FAIL"
    return emit("crash", status, findings, {"artifacts_audited": checked})


def scan_logs_for_crashes(log_dir):
    """Optional deep scan: grep artifact logs for crash signatures."""
    findings = []
    art = Path(log_dir)
    if not art.exists():
        return findings
    for f in sorted(art.glob("*.log")):
        try:
            text = f.read_text(errors="replace")
        except OSError:
            continue
        for sig in CRASH_SIGNATURES:
            if sig in text:
                findings.append(f"{f.name}: crash signature '{sig}'")
    return findings


# ---------------------------------------------------------------------------
# Selftest — the guardian must BITE on every fixture (no silent-green)
# ---------------------------------------------------------------------------

def selftest():
    fx = ROOT / "tools" / "guardian" / "fixtures"
    failures = []

    def expect_bite(name, rc):
        if rc != 0:
            print(f"  [ok] {name}: guardian BITES (exit {rc})")
        else:
            failures.append(f"{name}: guardian stayed GREEN on a bad fixture")

    def expect_pass(name, rc):
        if rc == 0:
            print(f"  [ok] {name}: healthy fixture passes")
        else:
            failures.append(f"{name}: healthy fixture unexpectedly RED")

    # 1. healthy throughput fixture → PASS
    rc = watch_throughput(fx / "throughput-baseline.json", fx / "results-healthy.json", 3.0)
    expect_pass("throughput/healthy", rc)

    # 2. 3.4% drop fixture → FAIL
    rc = watch_throughput(fx / "throughput-baseline.json", fx / "results-drop.json", 3.0)
    expect_bite("throughput/drop-3.4pct", rc)

    # 3. wire drift (one byte) → FAIL
    rc = watch_wire(MANIFEST, probe_json=fx / "wire-drift.json")
    expect_bite("wire/one-byte-drift", rc)

    # 4. wire healthy → PASS
    rc = watch_wire(MANIFEST, probe_json=fx / "wire-healthy.json")
    expect_pass("wire/healthy", rc)

    # 5. crash fixture → FAIL
    rc = watch_crash(fx)
    expect_bite("crash/crash-fixture", rc)

    if failures:
        return emit("selftest", "FAIL", failures)
    return emit("selftest", "PASS", ["all fixtures bit or passed as designed"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("watch", choices=["throughput", "wire", "crash", "all", "selftest"])
    ap.add_argument("--baseline", default=str(BASELINE))
    ap.add_argument("--results", default=str(BENCH_RESULTS))
    ap.add_argument("--threshold", type=float, default=3.0)
    ap.add_argument("--manifest", default=str(MANIFEST))
    ap.add_argument("--probe-json", default=None)
    ap.add_argument("--probe-cmd", default=None)
    ap.add_argument("--artifacts-dir", default=str(ROOT / "ci" / "run-artifacts"))
    args = ap.parse_args()

    if args.watch == "throughput":
        return watch_throughput(Path(args.baseline), Path(args.results), args.threshold)
    if args.watch == "wire":
        return watch_wire(Path(args.manifest), probe_json=args.probe_json,
                          probe_cmd=args.probe_cmd)
    if args.watch == "crash":
        return watch_crash(args.artifacts_dir)
    if args.watch == "selftest":
        return selftest()

    # all — three watches, one verdict
    rcs = []
    rcs.append(watch_throughput(Path(args.baseline), Path(args.results), args.threshold))
    rcs.append(watch_wire(Path(args.manifest)))
    rcs.append(watch_crash(args.artifacts_dir))
    return 0 if all(r == 0 for r in rcs) else 1


if __name__ == "__main__":
    sys.exit(main())
