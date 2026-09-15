"""
backend_a.py — Implementation A: reactive naive.

Per WO-P5-RELEASE §1.T1 + decision 2:
  A = reactive naive (Python list, redraw every frame)

This is the pathological case the whitepaper §1 documents. Every publish
allocates a fresh Python list; every read allocates a snapshot copy. The draw
routine redraws the entire frame every iteration. This is what reactive UI
frameworks do internally with `MutableState<FloatArray>` at 120 Hz — it
collapses on real hardware, and the sandbox simulation shows the same
allocation pattern (just faster because Python is interpreted).

The fairness pin (draw_routine.draw_spectrum) is identical across A/B/C/D;
only the buffer-ownership protocol differs.
"""
import array
import struct
import tracemalloc
from typing import Tuple
from draw_routine import Backend


class ReactiveNaive(Backend):
    name = "A"

    def __init__(self, frame_size: int, frame_hz: int, payload_dtype: str = "float32"):
        super().__init__(frame_size, frame_hz, payload_dtype)
        # Allocated fresh per publish — the pathological pattern
        self._latest = None
        self._seq = 0

    def publish(self, payload: bytes) -> int:
        # Allocate a fresh Python list (most pathological form)
        # The list comprehension forces Python to materialize a list object.
        n_floats = len(payload) // 4
        floats = [struct.unpack_from("<f", payload, i * 4)[0] for i in range(n_floats)]
        self._latest = floats  # full rebind, no pooling
        self._seq += 1
        self._frame_count = self._seq
        return self._seq

    def read(self) -> Tuple[int, bytes]:
        if self._latest is None:
            return (0, b"")
        # Snapshot copy — every read allocates
        snapshot = list(self._latest)  # full copy
        # Re-pack to bytes for the draw routine
        buf = array.array("f", snapshot)
        return (self._seq, buf.tobytes())

    def teardown(self):
        self._latest = None


def alloc_per_frame_estimate(frame_size: int) -> int:
    """Estimate bytes per frame for this backend.

    Per publish: 1 list of N floats (8B pointer each = 8N) + N float objects
    (~24B each = 24N) = 32N bytes per frame (very approximate).
    Per read: 1 list copy (8N) + 1 array (4N) + 1 bytes (4N) = 16N bytes per read.
    """
    return 32 * frame_size + 16 * frame_size  # 48N bytes per publish+read cycle
