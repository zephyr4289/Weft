"""
wsuite_runner.py — W-suite runner per WO-P5-RELEASE §1.T2.

Per workload × per implementation, collect:
  - P50/P99/P100 FPS (P99 is the headline — same rule as B-suite)
  - alloc bytes/frame (asserted 0 for C and D — RED on violation)
  - GC pauses (count + total ms via gc.get_stats())
  - CPU% via psutil
  - cold-start overhead (ms)

Output: one JSON line per cell, same contract pattern as B-cells.
Bundles to bench/results/wsuite-<env>.json (decision 1: separate from
canonical results.json; canonical bundle 16b5c663 ships untouched).
"""
import argparse
import gc
import json
import os
import sys
import time
import tracemalloc
from pathlib import Path

# Add the workloads dir to path
WEFT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(WEFT_ROOT / "bench" / "workloads"))

# Use absolute imports (not relative) since we're not running as a package
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

# psutil is optional; degrade gracefully
try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False


BACKEND_CLASSES = {
    "A": backend_a.ReactiveNaive,
    "B": backend_b.BestPractice,
    "C": backend_c.WeftCTypes,
    "D": backend_d.HandRolledTriple,
}

WORKLOADS = ["W1", "W2", "W3", "W4", "W5"]
WORKLOAD_PARAMS = {
    "W1": {"frame_size": 1024, "frame_hz": 120, "payload_dtype": "float32"},
    "W2": {"particle_count": 500, "dof": 6, "frame_hz": 120, "payload_dtype": "float32"},
    "W3": {"cols": 256, "rows": 64, "frame_hz": 60, "payload_dtype": "float32"},
    "W4": {"rows": 10000, "cols": 20, "updated_cells_per_frame": 50, "frame_hz": 60, "payload_dtype": "float32"},
    "W5": {"levels": 1000, "fields": 10, "frame_hz": 60, "payload_dtype": "float32"},
}


def make_backend(backend_id: str, workload_id: str):
    """Instantiate the backend for the given workload."""
    params = WORKLOAD_PARAMS[workload_id].copy()
    frame_size_bytes = workload_generators.frame_size_bytes(workload_id, params)
    # Convert bytes to "frame_size" (number of payload units) per backend
    if workload_id == "W1":
        frame_size = params["frame_size"]
    elif workload_id == "W2":
        frame_size = params["particle_count"] * params["dof"]
    elif workload_id == "W3":
        frame_size = params["cols"] * params["rows"]
    elif workload_id == "W4":
        frame_size = params["updated_cells_per_frame"]
    elif workload_id == "W5":
        frame_size = params["levels"] * params["fields"]
    cls = BACKEND_CLASSES[backend_id]
    return cls(frame_size=frame_size, frame_hz=params["frame_hz"],
               payload_dtype=params["payload_dtype"])


def make_payload(workload_id: str, frame_idx: int) -> bytes:
    """Generate the workload's payload for frame_idx."""
    if workload_id == "W1":
        return workload_generators.gen_w1_audio_pcm(frame_idx)
    elif workload_id == "W2":
        return workload_generators.gen_w2_particles(frame_idx)
    elif workload_id == "W3":
        return workload_generators.gen_w3_spectrogram(frame_idx)
    elif workload_id == "W4":
        payload, _ = workload_generators.gen_w4_grid_subset(frame_idx)
        return payload
    elif workload_id == "W5":
        return workload_generators.gen_w5_orderbook(frame_idx)


def run_cell(workload_id: str, backend_id: str, measure_s: float = 10.0,
             warmup_s: float = 1.0) -> dict:
    """Run one (workload × backend) cell. Returns metrics dict."""
    import struct

    # Cold-start measurement: time from backend init to first publish
    cold_start_start = time.perf_counter()
    backend = make_backend(backend_id, workload_id)
    # First publish
    payload0 = make_payload(workload_id, 0)
    backend.publish(payload0)
    cold_start_ms = (time.perf_counter() - cold_start_start) * 1000.0

    # Warmup
    warmup_deadline = time.perf_counter() + warmup_s
    frame_idx = 1
    while time.perf_counter() < warmup_deadline:
        backend.publish(make_payload(workload_id, frame_idx))
        frame_idx += 1

    # Measurement window
    gc.collect()
    gc_stats_before = gc.get_stats().copy()
    tracemalloc.start()
    tracemalloc.reset_peak()
    snapshot_before = tracemalloc.take_snapshot()
    cpu_before = time.process_time()
    if HAS_PSUTIL:
        cpu_pct_before = psutil.Process().cpu_percent()

    fps_samples = []
    alloc_bytes_total = 0
    frame_count_measure = 0

    measure_deadline = time.perf_counter() + measure_s
    frame_idx_measure = 0
    last_t = time.perf_counter()
    while time.perf_counter() < measure_deadline:
        backend.publish(make_payload(workload_id, frame_idx_measure))
        # Draw the frame (uses the shared draw routine — fairness pin)
        seq, payload = backend.read()
        if payload:
            # Simulate a draw: read payload via memoryview into a 1-byte-per-float out buf
            n_floats = len(payload) // 4
            out_buf = bytearray(n_floats)
            mv = memoryview(out_buf)
            payload_mv = memoryview(payload)
            draw_routine.draw_spectrum(payload_mv, mv)
        now = time.perf_counter()
        if now - last_t > 0:
            fps = 1.0 / (now - last_t)
            fps_samples.append(fps)
        last_t = now
        frame_idx_measure += 1
        frame_count_measure += 1

    cpu_after = time.process_time()
    if HAS_PSUTIL:
        cpu_pct_after = psutil.Process().cpu_percent()
    else:
        cpu_pct_after = 0.0
    snapshot_after = tracemalloc.take_snapshot()
    tracemalloc.stop()
    gc_stats_after = gc.get_stats().copy()

    # Compute metrics
    if fps_samples:
        fps_samples.sort()
        n = len(fps_samples)
        p50 = fps_samples[n // 2]
        # P99 tail frame rate (1% low — 99th percentile worst-case frame duration floor):
        p99 = fps_samples[max(0, int(n * 0.01))]
        p100 = fps_samples[-1]
    else:
        p50 = p99 = p100 = 0.0

    # Allocation delta (tracemalloc — Python-side only)
    stats_diff = [after - before for before, after in
                  zip(snapshot_before.statistics("filename"),
                      snapshot_after.statistics("filename"))]
    alloc_bytes_total = sum(s.size_diff for s in stats_diff if s.size_diff > 0)
    alloc_bytes_per_frame = alloc_bytes_total / max(1, frame_count_measure)

    # GC pause count + total ms (gc.get_stats() returns list of dicts in py3.12+)
    # Each gen dict has keys: collections, collected, uncollectable
    gc_collections_before = sum(s["collections"] for s in gc_stats_before)
    gc_collections_after = sum(s["collections"] for s in gc_stats_after)
    gc_pause_count = gc_collections_after - gc_collections_before
    # gc doesn't track pause duration directly; we estimate via CPU% delta
    cpu_seconds = (cpu_after - cpu_before)
    wall_seconds = measure_s
    cpu_percent = (cpu_seconds / wall_seconds) * 100.0 if wall_seconds > 0 else 0.0
    # GC pause ms is approximate: total_cpu - (frame_count * per_frame_cpu)
    # For W-suite we just report count + cpu%; the gate is alloc bytes/frame
    gc_pause_ms_total = 0.0  # gc.get_stats() doesn't give ms; report 0 + a note

    # The W-suite assertion: alloc_bytes_per_frame == 0 for C and D (RED on violation)
    assert_zero_alloc = (backend_id in ("C", "D"))
    alloc_violation = assert_zero_alloc and (alloc_bytes_per_frame > 0.5)

    result = {
        "workload": workload_id,
        "backend": backend_id,
        "backend_name": BACKEND_CLASSES[backend_id].__name__,
        "fps_p50": round(p50, 1),
        "fps_p99": round(p99, 1),
        "fps_p100": round(p100, 1),
        "alloc_bytes_per_frame": round(alloc_bytes_per_frame, 2),
        "alloc_assert_zero": assert_zero_alloc,
        "alloc_violation": alloc_violation,
        "gc_pause_count": gc_pause_count,
        "gc_pause_ms_total": round(gc_pause_ms_total, 2),
        "gc_pause_ms_note": "gc.get_stats() does not track pause duration; count is via collection delta, ms total approximated as 0",
        "cpu_percent": round(cpu_percent, 1),
        "cold_start_ms": round(cold_start_ms, 2),
        "frame_count": frame_count_measure,
        "measure_s": measure_s,
        "warmup_s": warmup_s,
        "pass": (p99 > 0) and not alloc_violation,
    }
    backend.teardown()
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--workloads", default="W1,W2,W3,W4,W5")
    p.add_argument("--backends", default="A,B,C,D")
    p.add_argument("--measure-s", type=float, default=10.0)
    p.add_argument("--warmup-s", type=float, default=1.0)
    p.add_argument("--env", default="x86_64-sandbox")
    args = p.parse_args()

    workloads = args.workloads.split(",")
    backends = args.backends.split(",")

    cells = []
    for wid in workloads:
        for bid in backends:
            print(f"→ {wid}/{bid} ...", end="", flush=True)
            try:
                r = run_cell(wid, bid, measure_s=args.measure_s, warmup_s=args.warmup_s)
                cells.append(r)
                status = "PASS" if r["pass"] else "FAIL"
                print(f" P99={r['fps_p99']:.1f} alloc/frame={r['alloc_bytes_per_frame']:.1f} [{status}]")
            except Exception as e:
                print(f" ERROR: {e}")
                cells.append({
                    "workload": wid, "backend": bid, "error": str(e), "pass": False
                })

    # Write bundle (decision 1: separate from canonical results.json)
    out_path = WEFT_ROOT / "bench" / "results" / f"wsuite-{args.env}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    bundle = {
        "catalog": "weft-wsuite",
        "catalog_version": 1,
        "env": {
            "env_label": args.env,
            "python3_version": ".".join(str(v) for v in sys.version_info[:3]),
            "psutil": HAS_PSUTIL,
            "harness_version": "wsuite_runner.py v1",
            "utc_timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        },
        "env_label": args.env,
        "cells": cells,
        "honesty_labels": {
            "fps_note": "P99 is the headline metric per WO-P5-RELEASE decision 4. FPS samples are 1/dt between successive publish+draw cycles; sandbox Python is interpreted so absolute FPS is low; the COMPARISON across backends is the signal.",
            "alloc_note": "alloc_bytes_per_frame measured via tracemalloc (Python-side only; ctypes-level copies are not counted). C and D assert 0; RED on violation.",
            "gc_note": "gc.get_stats() returns collection counts per generation; pause duration is not tracked by Python's gc module. gc_pause_ms_total is approximated as 0 with a note; gc_pause_count is the real metric.",
            "cpu_note": "cpu_percent = (process_time delta / wall_time delta) * 100. Captures interpreter + GC overhead.",
            "cold_start_note": "cold_start_ms = time from backend __init__ to first publish return. C includes ctypes.CDLL load + weft_init; D includes ctypes buffer allocation; A/B are pure-Python allocations.",
        },
    }
    out_path.write_text(json.dumps(bundle, indent=2))
    print(f"\n✓ W-suite bundle: {out_path}")
    print(f"  cells: {len(cells)} (target: 20 = 5 workloads × 4 backends)")
    n_pass = sum(1 for c in cells if c.get("pass"))
    print(f"  pass: {n_pass}/{len(cells)}")
    return 0 if n_pass == len(cells) else 1


if __name__ == "__main__":
    sys.exit(main())
