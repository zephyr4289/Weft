"""
draw_routine.py — The W-suite fairness pin (WO-P5-RELEASE §1.T1 + decision 2).

One `drawBar`-class function, byte-identical logic across A/B/C/D, parameterized
only by backend. Backend selection at runtime; workload code identical regardless
of backend.

The fairness pin is mechanical: this module is imported by all four backends
(A, B, C, D). The draw routine produces a deterministic visual output from
a frame's payload — same output bytes regardless of which backend produced
the frame. This is what makes A/B/C/D comparable: they all run the same draw
code on the same input distribution, only the buffer-ownership protocol
differs.

Per decision 2: D carries the envelope + I6 contract hand-rolled (its purpose
is to show a competent hand-rolled triple-buffer still loses to a specified
protocol on the metrics that matter — if it doesn't lose, that is a finding
to publish, not to bury).
"""
import struct
from typing import Callable

# ---------------------------------------------------------------------------
# Shared draw routine — the fairness pin
# ---------------------------------------------------------------------------

def draw_bar(height: float, max_height: float, width: int = 1) -> bytes:
    """Render a single vertical bar of `height` (0..max_height) to `width` bytes.

    Pure function: same input → same output bytes, regardless of caller.
    Used by all four backends (A, B, C, D) identically.
    """
    if max_height <= 0:
        return b"\x00" * width
    h = max(0, min(255, int(255 * height / max_height)))
    return bytes([h]) * width


def draw_spectrum(spectrum: memoryview, out_buf: memoryview) -> int:
    """Render a spectrum (float32 PCM magnitudes) to a byte buffer.

    Pure function: same input spectrum → same output bytes, regardless of caller.
    Used by W1 (audio visualizer) and W3 (spectrogram) workloads.
    """
    if len(spectrum) == 0:
        return 0
    # Take magnitudes — assume already-computed |FFT|/|PCM|
    floats = spectrum.tobytes()
    n_floats = len(floats) // 4
    out = bytearray(n_floats)  # 1 byte per float (downsample to 8-bit)
    for i in range(n_floats):
        # Read float32 LE
        f = struct.unpack_from("<f", floats, i * 4)[0]
        # Normalize to 0..255 (audio magnitude is typically -1..1)
        mag = abs(f)
        h = min(255, int(mag * 255))
        out[i] = h
    out_buf[:n_floats] = bytes(out)
    return n_floats


def draw_particles(particles: memoryview, count: int, dof: int, out_buf: memoryview) -> int:
    """Render particle positions (count × dof floats) to out_buf as packed bytes.

    Pure function. Used by W2 (particle field).
    """
    bytes_per_particle = dof * 4
    n_bytes = count * bytes_per_particle
    if n_bytes > len(out_buf):
        n_bytes = len(out_buf)
    out_buf[:n_bytes] = particles.tobytes()[:n_bytes]
    return n_bytes


def draw_grid_subset(grid: memoryview, rows: int, cols: int, updated: list, out_buf: memoryview) -> int:
    """Render a subset of grid cells to out_buf.

    Pure function. Used by W4 (data grid).
    """
    bytes_per_cell = 4  # float32
    out = bytearray(len(updated) * bytes_per_cell)
    for i, (r, c) in enumerate(updated):
        if r < rows and c < cols:
            offset = (r * cols + c) * bytes_per_cell
            if offset + 4 <= len(grid):
                out[i*4:(i+1)*4] = bytes(grid[offset:offset+4])
    n = len(out)
    out_buf[:n] = bytes(out)
    return n


def draw_orderbook(levels: memoryview, n_levels: int, n_fields: int, out_buf: memoryview) -> int:
    """Render order book levels to out_buf.

    Pure function. Used by W5 (order book).
    """
    bytes_per_level = n_fields * 4
    n_bytes = n_levels * bytes_per_level
    if n_bytes > len(out_buf):
        n_bytes = len(out_buf)
    out_buf[:n_bytes] = levels.tobytes()[:n_bytes]
    return n_bytes


# ---------------------------------------------------------------------------
# Backend contract — every backend implements this
# ---------------------------------------------------------------------------

class Backend:
    """Backend protocol: A, B, C, D all conform to this interface."""

    name: str = "base"

    def __init__(self, frame_size: int, frame_hz: int, payload_dtype: str = "float32"):
        self.frame_size = frame_size
        self.frame_hz = frame_hz
        self.payload_dtype = payload_dtype
        self._frame_count = 0

    def publish(self, payload: bytes) -> int:
        """Publish a frame. Returns frame_count after publish."""
        raise NotImplementedError

    def read(self) -> tuple:
        """Read the latest frame. Returns (seq, payload_bytes)."""
        raise NotImplementedError

    def draw(self, out_buf: memoryview) -> int:
        """Read latest frame and render it via the shared draw routine."""
        seq, payload = self.read()
        mv = memoryview(payload)
        return draw_spectrum(mv, out_buf)

    def teardown(self):
        """Release resources."""
        pass

    @property
    def frame_count(self):
        return self._frame_count
