#!/usr/bin/env python3
"""
soak_b2.py — Phase 5 / T0 / C2: B2 soak evidence supplement per WO-P4-CLOSURE §3.

Re-runs the 2×30s soaks (C + Rust) with the existing record tools, captures:
  - effective writer rate (Hz)  = frames / secs
  - fresh claim count           = frames - stale
  - stale claim count           = stale returns (printed by tool)
  - RSS before/after (kB)       = VmRSS from /proc/<pid>/status
  - capture/replay sha256       = sha256 of the .weftrec file

Then prints the three-line evidence block per run, disambiguating
World A (rate≈120 Hz, stale≈99% of claims) vs World B (rate≈3.5 M Hz,
stale=0).

This script does NOT modify the kernel or the record tools. Both already
print "capture: N frames, N stale returns" with absolute-schedule pacing
(WO-P2-CLOSURE B2 fix landed in Phase 4 B3). We sample RSS externally.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

WEFT_ROOT = Path("/home/z/my-project/upload/weft-docs/weft-docs/weft")
SECS = 30
HZ = 120
PAYLOAD = 64

EVIDENCE_DIR = WEFT_ROOT / "litmus" / "evidence" / "soak-b2"
EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)


def build_record_tools():
    """Compile the C and Rust record binaries if needed."""
    # C: rebuild via Makefile target in tools/weft-record
    c_bin = WEFT_ROOT / "tools" / "weft-record" / "weft_record"
    c_src = WEFT_ROOT / "tools" / "weft-record" / "weft_record.c"
    c_weft = WEFT_ROOT / "core" / "c" / "weft.c"
    c_hdr = WEFT_ROOT / "core" / "c" / "weft.h"
    if not c_bin.exists() or c_bin.stat().st_mtime < max(c_src.stat().st_mtime, c_weft.stat().st_mtime):
        print("→ building C record tool")
        subprocess.run([
            "gcc", "-O2", "-std=c11", "-Wall", "-Wextra",
            "-pthread", "-D_GNU_SOURCE",
            "-I", str(WEFT_ROOT / "core" / "c"),
            str(c_src), str(c_weft),
            "-o", str(c_bin)
        ], check=True, cwd=str(WEFT_ROOT / "tools" / "weft-record"))

    # Rust: cargo build --release --bin record --bin probe
    rust_bin = WEFT_ROOT / "core" / "rust" / "target" / "release" / "record"
    if not rust_bin.exists():
        print("→ building Rust record tool")
        env = os.environ.copy()
        env["PATH"] = f"{os.path.expanduser('~')}/.cargo/bin:{env.get('PATH','')}"
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "record", "--bin", "probe"],
            cwd=str(WEFT_ROOT / "core" / "rust"), check=True, env=env
        )


def sample_rss(pid, samples):
    """Sample VmRSS from /proc/<pid>/status into samples list (kB)."""
    while not samples["stop"]:
        try:
            with open(f"/proc/{pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        kb = int(line.split()[1])
                        samples["rss"].append(kb)
                        break
        except FileNotFoundError:
            break
        time.sleep(1.0)


def run_soak(lang, runner_path, args, out_file):
    """Run a 30s soak, sample RSS, return (rate_hz, fresh, stale, rss_before, rss_after, sha256).

    With the fixed stale-tracking (fresh iff s > max_seq_seen_so_far),
    `frames` = number of actual fresh publishes captured, and `stale` =
    number of duplicate-seq claims. The writer's effective rate =
    `frames / elapsed_s` (per the C2 disambiguation criterion).
    """
    print(f"\n→ {lang} soak: {runner_path} {' '.join(args)}")
    rss_samples = {"rss": [], "stop": False}

    # Start the runner
    start = time.time()
    proc = subprocess.Popen(
        [runner_path] + args,
        stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
        cwd=str(WEFT_ROOT)
    )

    # Start RSS sampler
    sampler = threading.Thread(target=sample_rss, args=(proc.pid, rss_samples), daemon=True)
    sampler.start()

    rss_before = rss_samples["rss"][0] if rss_samples["rss"] else None
    # Wait for completion
    _, stderr = proc.communicate(timeout=SECS + 30)
    elapsed = time.time() - start
    rss_samples["stop"] = True
    sampler.join(timeout=2)

    rss_after = rss_samples["rss"][-1] if rss_samples["rss"] else None
    rss_peak = max(rss_samples["rss"]) if rss_samples["rss"] else None

    print(stderr.strip())
    # Parse "capture: N frames, N stale returns, ..." (newer format from fixed tools)
    m = re.search(r"capture:\s+(\d+)\s+frames?,\s+(\d+)\s+stale", stderr)
    if not m:
        print(f"!! Could not parse capture line from stderr:\n{stderr}")
        return None
    frames = int(m.group(1))
    stale = int(m.group(2))
    fresh = frames  # with the fixed predicate, frames == fresh publishes captured
    writer_rate = frames / elapsed  # writer's effective publish rate (Hz)

    # SHA-256 of the .weftrec file
    sha = hashlib.sha256(out_file.read_bytes()).hexdigest()

    return {
        "lang": lang,
        "elapsed_s": elapsed,
        "frames": frames,
        "fresh": fresh,
        "stale": stale,
        "writer_rate_hz": writer_rate,
        "rss_before_kb": rss_before,
        "rss_after_kb": rss_after,
        "rss_peak_kb": rss_peak,
        "weftrec_sha256": sha,
        "weftrec_path": str(out_file.relative_to(WEFT_ROOT)),
        "stderr": stderr.strip(),
    }


def main():
    build_record_tools()

    c_runner = WEFT_ROOT / "tools" / "weft-record" / "weft_record"
    rust_runner = WEFT_ROOT / "core" / "rust" / "target" / "release" / "record"

    results = []

    # C run
    c_out = EVIDENCE_DIR / "soak_c_30s.weftrec"
    r = run_soak("C", str(c_runner), ["capture", str(c_out), "--hz", str(HZ), "--payload", str(PAYLOAD), "--secs", str(SECS)], c_out)
    if r: results.append(r)

    # Rust run
    rust_out = EVIDENCE_DIR / "soak_rust_30s.weftrec"
    r = run_soak("Rust", str(rust_runner), ["capture", str(rust_out), "--hz", str(HZ), "--payload", str(PAYLOAD), "--secs", str(SECS)], rust_out)
    if r: results.append(r)

    # Replay validation
    print("\n→ Replay validation (byte-identical CRC check)")
    for r in results:
        runner = c_runner if r["lang"] == "C" else rust_runner
        replay_proc = subprocess.run([str(runner), "replay", str(EVIDENCE_DIR / Path(r["weftrec_path"]).name)],
                                     cwd=str(WEFT_ROOT), capture_output=True, text=True, timeout=60)
        r["replay_exit"] = replay_proc.returncode
        m = re.search(r"replay:\s+(\d+)\s+records?\s+validated", replay_proc.stderr)
        if m:
            r["replay_records"] = int(m.group(1))
        r["replay_stderr"] = replay_proc.stderr.strip()

    # World A vs B disambiguation
    print("\n" + "=" * 78)
    print("B2 SOAK EVIDENCE — World A vs World B disambiguation")
    print("=" * 78)
    print()
    print("World A (fix worked):  writer_rate ≈ 120 Hz, stale ≈ 99% of claims")
    print("World B (fix didn't):   writer_rate ≈ 3.5 M Hz, stale = 0")
    print()
    for r in results:
        # With the fixed stale-tracking, fresh = writer's actual publish count.
        # World A: writer_rate ≈ 120 Hz and stale >> 0 (recorder claims way more than writer publishes).
        # World B: writer_rate >> 120 Hz (unpaced) and stale = 0 (recorder never catches a duplicate).
        rate = r["writer_rate_hz"]
        stale = r["stale"]
        if rate < 1000 and stale > 0:
            world = "A (fix worked)"
        elif rate > 100000 and stale == 0:
            world = "B (no fix)"
        else:
            world = "unclear"
        print(f"  {r['lang']:5s}  writer_rate={rate:.1f} Hz  fresh={r['fresh']}  stale={stale}  "
              f"RSS={r['rss_before_kb']}→{r['rss_after_kb']} kB (peak {r['rss_peak_kb']})  → World {world}")
        print(f"          weftrec sha256: {r['weftrec_sha256']}")
        print(f"          replay: exit {r['replay_exit']}, {r.get('replay_records', 0)} records validated")
        print(f"          run: --hz {HZ} --payload {PAYLOAD} --secs {SECS}")
    print()

    # Write JSON bundle
    import json
    out_json = EVIDENCE_DIR / "evidence.json"
    out_json.write_text(json.dumps({
        "directive": "WO-P4-CLOSURE §3 C2 / WO-P5-RELEASE T0",
        "scope": "Phase 5 / T0 / C2 — B2 soak evidence disambiguation",
        "method": "extant record tools (C + Rust), absolute-schedule pacing landed in Phase 4 B3",
        "params": {"hz": HZ, "payload": PAYLOAD, "secs": SECS},
        "env_label": "x86_64-sandbox",
        "runs": results,
    }, indent=2, default=str))
    print(f"✓ Evidence JSON: {out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
