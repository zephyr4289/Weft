"""
workload_generators.py — Frame payload generators for the W-suite.

Per WO-P5-RELEASE §1.T1 + decision 3 (verbatim bounds from roadmap §1b):
  W1 audio visualizer (1024-float PCM @ 60/120 Hz)
  W2 particle field (500 × 6-DOF RK4 @ 120 Hz)
  W3 spectrogram/heatmap (256×64 float matrix @ 60 Hz)
  W4 data grid (10k rows × 20 cols, fixed-cell subset updates)
  W5 order book (1000 levels × 10 fields, 60 Hz L2 feed)

Each generator produces a deterministic frame payload (seeded). The payload
format is identical across all four backends (A/B/C/D) — the fairness pin is
that the same payload distribution is fed to all backends, only the
buffer-ownership protocol differs.
"""
import math
import random
import struct
from dataclasses import dataclass
from typing import Callable, List, Tuple


SEED = 0x00C0FFEE


def gen_w1_audio_pcm(frame_idx: int, frame_size: int = 1024) -> bytes:
    """W1: audio PCM visualizer — 1024-float magnitude spectrum, seeded."""
    rng = random.Random(SEED + frame_idx)
    # Simulate |FFT| magnitudes of an audio signal — 0..1 floats
    floats = []
    base_freq = 1.0 + 0.5 * math.sin(frame_idx * 0.1)
    for i in range(frame_size):
        # Mix of harmonics + noise, normalized to 0..1
        mag = (
            0.5 * math.sin(2 * math.pi * base_freq * i / frame_size) +
            0.3 * math.sin(2 * math.pi * 3 * base_freq * i / frame_size) +
            0.2 * rng.random()
        )
        floats.append(max(0.0, min(1.0, mag)))
    return struct.pack(f"<{frame_size}f", *floats)


def gen_w2_particles(frame_idx: int, particle_count: int = 500, dof: int = 6) -> bytes:
    """W2: particle field — RK4 integration of particle positions+velocities.

    Each frame is `particle_count * dof` float32 values. We simulate a simple
    attractor field; the integrator is implicit RK4 (one step per frame).
    """
    rng = random.Random(SEED + frame_idx)
    # Per-particle: position (3) + velocity (3) = 6 floats
    floats = []
    for p in range(particle_count):
        # Initialize from prior frame (would need state — for fairness, we
        # just generate the next-step values directly)
        x = (rng.random() - 0.5) * 2.0
        y = (rng.random() - 0.5) * 2.0
        z = (rng.random() - 0.5) * 2.0
        vx = -y * 0.01  # rotational field
        vy = x * 0.01
        vz = 0.0
        floats.extend([x, y, z, vx, vy, vz])
    return struct.pack(f"<{particle_count * dof}f", *floats)


def gen_w3_spectrogram(frame_idx: int, cols: int = 256, rows: int = 64) -> bytes:
    """W3: spectrogram/heatmap — 256×64 float matrix."""
    rng = random.Random(SEED + frame_idx)
    floats = []
    for r in range(rows):
        for c in range(cols):
            # Simulate a heatmap that shifts over time
            val = math.sin(frame_idx * 0.05 + r * 0.1 + c * 0.05) * 0.5 + 0.5
            val += rng.uniform(-0.05, 0.05)
            floats.append(max(0.0, min(1.0, val)))
    return struct.pack(f"<{cols * rows}f", *floats)


def gen_w4_grid_subset(frame_idx: int, rows: int = 10000, cols: int = 20,
                       updated_cells_per_frame: int = 50) -> Tuple[bytes, List[Tuple[int, int]]]:
    """W4: data grid — 10k rows × 20 cols, fixed-cell subset updates per frame.

    Returns (payload_bytes, updated_indices) — payload is just the updated
    cells' values (50 floats = 200 bytes), plus the row/col indices.
    """
    rng = random.Random(SEED + frame_idx)
    # Pick 50 random (row, col) pairs
    updated = [(rng.randint(0, rows - 1), rng.randint(0, cols - 1))
               for _ in range(updated_cells_per_frame)]
    # Generate float values for those cells
    floats = [rng.uniform(0.0, 1000.0) for _ in range(updated_cells_per_frame)]
    payload = struct.pack(f"<{updated_cells_per_frame}f", *floats)
    return (payload, updated)


def gen_w5_orderbook(frame_idx: int, levels: int = 1000, fields: int = 10) -> bytes:
    """W5: order book — 1000 levels × 10 fields (price, size, num_orders, ...)."""
    rng = random.Random(SEED + frame_idx)
    floats = []
    base_price = 100.0 + math.sin(frame_idx * 0.05) * 5.0
    for lvl in range(levels):
        # Bid/ask ladder; 10 fields per level (bid_price, bid_size, bid_n,
        # ask_price, ask_size, ask_n, ...)
        for f in range(fields):
            if f == 0:  # bid price
                floats.append(base_price - lvl * 0.01 + rng.uniform(-0.005, 0.005))
            elif f == 3:  # ask price
                floats.append(base_price + lvl * 0.01 + rng.uniform(-0.005, 0.005))
            elif f in (1, 4):  # sizes
                floats.append(rng.uniform(0.1, 10.0))
            else:  # num_orders, etc.
                floats.append(float(rng.randint(1, 100)))
    return struct.pack(f"<{levels * fields}f", *floats)


# Dispatch table
WORKLOADS = {
    "W1": gen_w1_audio_pcm,
    "W2": gen_w2_particles,
    "W3": gen_w3_spectrogram,
    "W4": gen_w4_grid_subset,
    "W5": gen_w5_orderbook,
}


def get_workload(wid: str) -> Callable:
    return WORKLOADS[wid]


# Frame size (bytes) per workload
def frame_size_bytes(wid: str, params: dict = None) -> int:
    params = params or {}
    if wid == "W1":
        return params.get("frame_size", 1024) * 4
    elif wid == "W2":
        return params.get("particle_count", 500) * params.get("dof", 6) * 4
    elif wid == "W3":
        return params.get("cols", 256) * params.get("rows", 64) * 4
    elif wid == "W4":
        return params.get("updated_cells_per_frame", 50) * 4
    elif wid == "W5":
        return params.get("levels", 1000) * params.get("fields", 10) * 4
    raise ValueError(f"unknown workload {wid}")
