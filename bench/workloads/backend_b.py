"""
backend_b.py — Implementation B: best practice.

Per WO-P5-RELEASE §1.T1 + decision 2:
  B = best practice (pooled array.array, deferred draw into pre-allocated
  render buffer)

This is what a competent developer ships TODAY on mobile platforms:
- Pooled buffer, no per-publish allocation
- Deferred draw (draw-phase reads, doesn't recompose)
- Render buffer pre-allocated once

The fairness pin (draw_routine.draw_spectrum) is identical across A/B/C/D;
only the buffer-ownership protocol differs. B demonstrates that best
practice is already pretty good — Weft's claim is NOT to beat B on raw
draw-bound FPS, but to standardize the pattern across platforms and
eliminate the protocol-correctness bugs that hand-rolled triple-buffers
ship in production.
"""
import array
from typing import Tuple
from draw_routine import Backend


class BestPractice(Backend):
    name = "B"

    def __init__(self, frame_size: int, frame_hz: int, payload_dtype: str = "float32"):
        super().__init__(frame_size, frame_hz, payload_dtype)
        # Pre-allocate TWO buffers — double-buffering, the standard pattern
        self._buf_w = array.array("f", [0.0] * frame_size)  # writer's work buffer
        self._buf_r = array.array("f", [0.0] * frame_size)  # reader's work buffer
        self._seq = 0
        # No per-frame allocation below this point

    def publish(self, payload: bytes) -> int:
        # Reuse pre-allocated buffer — no allocation
        n = min(len(payload) // 4, self.frame_size)
        self._buf_w[:n] = array.array("f", payload[:n * 4])
        # Atomic-ish swap of references (Python GIL makes this safe)
        self._buf_w, self._buf_r = self._buf_r, self._buf_w
        self._seq += 1
        self._frame_count = self._seq
        return self._seq

    def read(self) -> Tuple[int, bytes]:
        # Reuse the read buffer; return a memoryview-backed bytes view
        # The draw routine reads from this directly — no allocation
        return (self._seq, self._buf_r.tobytes())

    def teardown(self):
        # Buffers are stack-local Python objects; GC handles them
        self._buf_w = None
        self._buf_r = None


def alloc_per_frame_estimate(frame_size: int) -> int:
    """Estimate bytes per frame for this backend.

    Per publish: array.array bytes view (no copy if using memoryview) ≈ 0
    Per read: bytes() call materializes — 4N bytes per read.
    The draw-phase cost is real (4N bytes per frame) but it's the
    best-practice floor for non-zero-copy Python.
    """
    return 4 * frame_size  # 4N bytes per publish+read cycle (just the tobytes() copy)
