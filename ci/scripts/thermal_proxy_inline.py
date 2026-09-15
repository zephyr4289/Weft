#!/usr/bin/env python3
"""
thermal_proxy_inline.py — inline copy of the thermal proxy runner.
Ships in the repo so the CI doesn't depend on /home/z/my-project/scripts/.

Per WO-P5-RELEASE §1.T3 + WO-P5-VERIFICATION P5-W6 (waived).
"""
import argparse
import gc
import json
import os
import sys
import time
import tracemalloc
from pathlib import Path

WEFT_ROOT = Path(__file__).resolve().parent.parent.parent  # ci/scripts/ -> repo root
sys.path.insert(0, str(WEFT_ROOT / "bench" / "workloads"))

import importlib.util

def _import_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

draw_routine = _import_module("draw_routine", WEFT_ROOT / "bench" / "workloads" / "draw_routine.py")
backend_a = _import_module("backend_a", WEFT_ROOT / "bench" / "workloads" / "backend_a.py")
backend_b = _import_module("backend_b", WEFT_ROOT / "bench" / "workloads" / "backend_b.py")
backend_c = _import_module("backend_c", WEFT_ROOT / "bench" / "workloads" / "backend_c.py")
backend_d = _import_module("backend_d", WEFT_ROOT / "bench" / "workloads" / "backend_d.py")
workload_generators = _import_module("workload_generators", WEFT_ROOT / "bench" / "workloads" / "workload_generators.py")

BACKENDS = {
    "A": backend_a.ReactiveNaive,
    "B": backend_b.BestPractice,
    "C": backend_c.WeftCTypes,
    "D": backend_d.HandRolledTriple,
}


def run_thermal(backend_id: str, duration_s: int, sample_hz: float = 1.0) -> dict:
    print(f"  → {backend_id}: {duration_s}s sustained run, {sample_hz}Hz sampling")
    backend = BACKENDS[backend_id](frame_size=500 * 6, frame_hz=120, payload_dtype="float32")
    samples = []
    frame_idx = 0
    start = time.perf_counter()
    deadline = start + duration_s
    next_sample = start + 1.0 / sample_hz
    sample_window_frames = 0
    sample_window_start = start

    while time.perf_counter() < deadline:
        backend.publish(workload_generators.gen_w2_particles(frame_idx))
        seq, payload = backend.read()
        if payload:
            n_floats = len(payload) // 4
            out_buf = bytearray(n_floats)
            draw_routine.draw_spectrum(memoryview(payload), memoryview(out_buf))
        frame_idx += 1
        sample_window_frames += 1
        now = time.perf_counter()
        if now >= next_sample:
            elapsed = now - sample_window_start
            fps = sample_window_frames / elapsed if elapsed > 0 else 0
            samples.append({
                "t_s": round(now - start, 2),
                "fps": round(fps, 1),
                "frame_idx": frame_idx,
            })
            sample_window_frames = 0
            sample_window_start = now
            next_sample = now + 1.0 / sample_hz

    backend.teardown()
    fps_values = [s["fps"] for s in samples]
    if fps_values:
        first_quarter = fps_values[:max(1, len(fps_values)//4)]
        last_quarter = fps_values[-max(1, len(fps_values)//4):]
        first_avg = sum(first_quarter) / len(first_quarter)
        last_avg = sum(last_quarter) / len(last_quarter)
        decay_pct = ((first_avg - last_avg) / first_avg * 100) if first_avg > 0 else 0
    else:
        first_avg = last_avg = decay_pct = 0
    return {
        "backend": backend_id, "duration_s": duration_s, "sample_hz": sample_hz,
        "samples": samples, "fps_first_quarter_avg": round(first_avg, 1),
        "fps_last_quarter_avg": round(last_avg, 1), "decay_pct": round(decay_pct, 2),
        "flat": abs(decay_pct) < 5.0,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--duration", type=int, default=300)
    p.add_argument("--backends", default="A,B,C,D")
    p.add_argument("--env", default="x86_64-sandbox")
    args = p.parse_args()

    backends = args.backends.split(",")
    results = []
    for bid in backends:
        r = run_thermal(bid, args.duration)
        results.append(r)
        flat_label = "FLAT" if r["flat"] else f"DECAY {r['decay_pct']}%"
        print(f"    first-quarter FPS={r['fps_first_quarter_avg']}, last-quarter FPS={r['fps_last_quarter_avg']}, {flat_label}")

    bundle = {
        "directive": "WO-P5-RELEASE §1.T3 (CI inline)",
        "scope": "thermal-proxy W2 × all four backends × sustained",
        "duration_s": args.duration,
        "env": {
            "env_label": args.env,
            "python3_version": ".".join(str(v) for v in sys.version_info[:3]),
            "harness_version": "thermal_proxy_inline.py v1 (CI)",
            "utc_timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        },
        "env_label": args.env,
        "honesty_label": (
            "Headless server has no meaningful thermal envelope; if the FPS curve is flat, "
            "the report says 'no thermal decay observable on headless server — expected; "
            "device thermal is Phase 6+.' No invented throttling (WO-P5-RELEASE decision 4)."
        ),
        "runs": results,
    }
    out_path = WEFT_ROOT / "bench" / "results" / f"wsuite-thermal-{args.env}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(bundle, indent=2))
    print(f"\n✓ Thermal bundle: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
