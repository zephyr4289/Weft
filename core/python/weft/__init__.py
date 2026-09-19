"""Weft — Python bindings for the Weft C core (issue #18-6).

A pythonic, zero-copy surface over the frozen C kernel (weft.{h,c}) and the
RFC-0004 fan-out driver (fanout.{h,c} + fanout_batch.{h,c}):
  - `Weft`: the Triad kernel (1 writer, 1 reader, 3 buffers, one atomic).
  - `Fanout` / `FanoutReader`: the multi-consumer ring (M slots, N readers).
  - `Fanout.publish_batch(...)`: the single-flip batch publisher (#17-3).

Zero-copy discipline: payloads are exchanged through `memoryview`s over the
C-owned buffers — a claim does NOT copy into Python objects. NumPy arrays
view the same memory (np.frombuffer) — the data-science/ML integration the
issue asks for. cffi releases the GIL around every C call, so concurrent
reader threads are real threads (PL6's torture).

Layout: every object crossing the boundary is an opaque C handle allocated
and released by ONE C call each (the FFI-finalizer discipline; see
weft_pyshim.c). No struct layouts are exposed.
"""
from __future__ import annotations

import threading
from typing import Optional, Sequence

from ._weft_c import ffi, lib

__version__ = "0.1.0"
__all__ = ["Weft", "Fanout", "FanoutReader", "BatchFrame", "active_copy_impl"]


def active_copy_impl() -> str:
    """The SIMD claim-copy implementation the C dispatcher resolved
    ('scalar' | 'sse2' | 'avx2' | 'avx512' | 'neon') — issue #17-1."""
    return ffi.string(lib.weft_fanout_copy_active_impl()).decode()


class Weft:
    """The Triad kernel: one writer, one reader, three buffers, one atomic.

    Usage: `w = Weft(payload_max)`; writer side `w.publish(seq, payload)`;
    reader side `w.claim()` -> claimed seq (0 = nothing new), then
    `w.payload()` -> zero-copy memoryview of the claimed frame.
    """

    def __init__(self, payload_max: int):
        self._w = ffi.gc(lib.weft_py_new(payload_max), lib.weft_py_free)
        if not self._w:
            raise MemoryError(f"weft_py_new({payload_max}) failed")

    # ---- writer ----
    def publish(self, seq: int, payload) -> bool:
        """Publish `payload` (bytes-like, len <= payload_max) as frame `seq`.
        Returns True on WEFT_PUB_OK, False when a revoke dropped the frame."""
        mv = memoryview(payload).cast("B")
        if len(mv) and lib.weft_w_write_payload(
                self._w, ffi.from_buffer("const uint8_t*", mv), len(mv)) != 0:
            raise ValueError("payload write failed (len > payload_max?)")
        rc = lib.weft_publish(self._w, seq, len(mv))
        return rc == 0  # WEFT_PUB_OK

    # ---- reader ----
    def claim(self) -> int:
        """Claim the freshest complete frame and return its seq (the C
        r_claim returns a buffer index — this wrapper surfaces the seq,
        the useful identity; the buffer index is claim_index())."""
        lib.weft_r_claim(self._w)
        return lib.weft_r_seq(self._w)

    def claim_index(self) -> int:
        """Claim and return the raw Triad buffer index (0..2) — the C ABI
        surface, for parity tests against other ports."""
        return lib.weft_r_claim(self._w)

    def seq(self) -> int:
        return lib.weft_r_seq(self._w)

    def payload(self) -> memoryview:
        """Zero-copy view of the claimed frame's payload bytes (past the
        envelope; the canary tail stays out of view)."""
        n = lib.weft_r_payload_len(self._w)
        p = lib.weft_r_live_ptr(self._w, lib.weft_r_header_size(self._w))
        return memoryview(ffi.buffer(p, n))

    def payload_into(self, dst) -> int:
        """Copy the claimed payload into a preallocated buffer (the r_read_slice
        path); returns bytes copied."""
        mv = memoryview(dst).cast("B")
        return lib.weft_r_read_slice(self._w, mv, 0, len(mv))

    # ---- revocation (I6) ----
    def revoke(self) -> int:
        """Revoke the kernel; returns the pre-revoke epoch."""
        lib.weft_revoke(self._w)
        return lib.weft_epoch(self._w)

    def reclaim(self, pre_revoke_epoch: int, timeout_ms: int = 1000) -> bool:
        return lib.weft_reclaim(self._w, pre_revoke_epoch, timeout_ms) == 0

    @property
    def revoked(self) -> bool:
        return bool(lib.weft_revoked(self._w))

    # ---- telemetry (advisory) ----
    @property
    def telemetry(self) -> dict:
        return {
            "publish": lib.weft_t_publish(self._w),
            "claim": lib.weft_t_claim(self._w),
            "drop": lib.weft_t_drop(self._w),
            "wsteps": lib.weft_t_wsteps(self._w),
            "rsteps": lib.weft_t_rsteps(self._w),
        }


class Fanout:
    """The RFC-0004 fan-out ring: one writer, N readers, M slots."""

    def __init__(self, payload_bytes: int, slot_count: int = 4):
        if payload_bytes % 4:
            raise ValueError("payload_bytes must be a multiple of 4")
        if not 2 <= slot_count <= 64:
            raise ValueError("slot_count must be in [2, 64]")
        self._f = ffi.gc(
            lib.weft_fanout_new(payload_bytes, slot_count), lib.weft_fanout_free
        )
        if not self._f:
            raise MemoryError("weft_fanout_new failed")
        self.payload_bytes = payload_bytes
        self.slot_count = slot_count

    @property
    def ring(self):
        """The raw ring as a memoryview (the byte-layout contract; for
        cross-runtime sharing and the FFI attach path)."""
        base = lib.weft_fanout_ring(self._f)
        n = lib.weft_fanout_ring_bytes(self.payload_bytes, self.slot_count)
        return ffi.buffer(ffi.cast("char*", base), n)

    def publish(self, payload) -> int:
        """Publish one frame; returns its seq."""
        mv = memoryview(payload).cast("B")
        if len(mv) > self.payload_bytes:
            raise ValueError("payload exceeds payload_bytes")
        if not lib.weft_fanout_begin(self._f):
            raise RuntimeError("fanout begin failed")
        if lib.weft_fanout_fill(self._f, ffi.from_buffer("const void*", mv),
                                len(mv)) < 0:
            raise RuntimeError("fanout fill failed")
        return lib.weft_fanout_publish(self._f)

    def publish_batch(self, frames: Sequence) -> int:
        """Publish a burst as ONE batch (single publication flip — issue
        #17-3). `frames` is a sequence of bytes-like payloads. Returns the
        last frame's seq (the new latestSeq), 0 on refusal."""
        n = len(frames)
        if n == 0:
            return 0
        srcs = ffi.new("const void*[]", n)
        lens = ffi.new("size_t[]", n)
        keepalive = []
        for i, fr in enumerate(frames):
            mv = memoryview(fr).cast("B")
            keepalive.append(mv)
            if len(mv) > self.payload_bytes or len(mv) % 4:
                raise ValueError(f"frame {i}: bad length {len(mv)}")
            srcs[i] = ffi.from_buffer("const void*", mv)
            lens[i] = len(mv)
        return lib.weft_py_publish_batch(self._f, srcs, lens, n)


class FanoutReader:
    """A reader bound to a fan-out ring (any ring — same process or shared).

    Construct from a `Fanout` (its own ring), or from a raw C ring pointer
    (cffi cdata) + ring_bytes + geometry — the cross-runtime/IPC path.
    """

    def __init__(self, source, ring_bytes: Optional[int] = None,
                 payload_bytes: Optional[int] = None,
                 slot_count: Optional[int] = None):
        if isinstance(source, Fanout):
            f = source
            ring = lib.weft_fanout_ring(f._f)
            ring_bytes = lib.weft_fanout_ring_bytes(f.payload_bytes, f.slot_count)
            payload_bytes, slot_count = f.payload_bytes, f.slot_count
        else:
            ring = source  # a cffi cdata pointer to foreign ring memory
            if ring_bytes is None or payload_bytes is None or slot_count is None:
                raise ValueError("raw-ring construction needs ring_bytes, "
                                 "payload_bytes and slot_count")
        self._r = ffi.gc(
            lib.weft_fanout_reader_new(ring, ring_bytes, payload_bytes,
                                       slot_count),
            lib.weft_fanout_reader_free,
        )
        if not self._r:
            raise MemoryError("weft_fanout_reader_new failed")
        self.payload_bytes = payload_bytes
        self.slot_count = slot_count

    def claim(self):
        """Claim the freshest completed frame. Returns (fresh: bool, seq,
        dropped: int); on fresh, `view()` is the zero-copy payload."""
        c = lib.weft_fanout_claim(self._r)
        return (bool(c.fresh), int(c.seq), int(c.dropped))

    def view(self) -> memoryview:
        """Zero-copy memoryview of the reader's buffer (the fixed-width
        slot; payload_bytes wide — slice to the published length)."""
        p = lib.weft_fanout_view(self._r)
        return memoryview(ffi.buffer(ffi.cast("uint32_t*", p),
                                     self.payload_bytes))
