"""ring.py — WeftRing: single-producer / multi-consumer seqlock tensor ring.

Python twin of packages/weft-tensor/src/ring.js. Same publish protocol
(slot header COMMITTED-bit-last, then producer_seq via two little-endian u32
stores, LO word LAST), same wait-free acquire with bounded slot-seq
re-validation, same zero-allocation steady state (per-slot precreated
memoryviews + ndarrays at attach; the hot loop allocates nothing).

Buffers: bytearray | mmap.mmap | memoryview | bytes (read-only consumers) |
numpy arrays (via .buffer? use memoryview(arr)). Cross-process sharing via
mmap is the intended Python ingestion story (camera worker -> AI process).
"""
from __future__ import annotations

import struct
import time
from typing import Optional

from .layout import (
    RING_MAGIC, SLOT_MAGIC, LAYOUT_VERSION, RING_HEADER_SIZE, SLOT_HEADER_SIZE,
    OFF_MAGIC, OFF_LAYOUT_VERSION, OFF_HEADER_SIZE, OFF_SLOT_COUNT, OFF_SLOT_STRIDE,
    OFF_DTYPE_CODE, OFF_DTYPE_BITS, OFF_LANES, OFF_ELEM_SIZE, OFF_SHAPE, OFF_STRIDES,
    OFF_SCHEMA_ID, OFF_PRODUCER_SEQ, OFF_TICK_HZ, OFF_FLAGS, OFF_HEADER_CRC,
    RING_FLAG_LITTLE_ENDIAN, RING_FLAG_SHARED_MEMORY,
    SOFF_MAGIC, SOFF_PAYLOAD_LEN, SOFF_SEQ, SOFF_TIMESTAMP_NS, SOFF_DURATION_US,
    SOFF_SLOT_FLAGS, SOFF_FOURCC, SOFF_RANK, SOFF_PLANES, SOFF_PLANE_OFFSET,
    SOFF_PLANE_SIZE, SLOT_FLAG_COMMITTED, MAX_RANK, DLPACK_FLOAT,
    validate_ring_header, read_slot_header, ring_header_crc, LayoutError,
    fourcc_from_str,
)
from .view import WeftTensorView

_TWO32 = 1 << 32


def _as_memoryview(buf) -> memoryview:
    mv = memoryview(buf)
    if mv.format != "B":
        mv = mv.cast("B")
    return mv


def _align64(n: int) -> int:
    return (n + 63) & ~63


_NUMPY_DTYPE = {
    (0, 8): "i1", (0, 16): "i2", (0, 32): "i4", (0, 64): "i8",
    (1, 8): "u1", (1, 16): "u2", (1, 32): "u4", (1, 64): "u8",
    (2, 16): "f2", (2, 32): "f4", (2, 64): "f8",
    (5, 8): "b1",
}


def numpy_dtype_str(code: int, bits: int) -> str:
    key = (code, bits)
    if key not in _NUMPY_DTYPE:
        raise LayoutError("WTR1_BAD_DTYPE", f"dtype {code}/{bits} has no numpy mapping")
    return _NUMPY_DTYPE[key]


class WeftRing:
    """Attach to (or create) a WTR1 ring and produce/consume tensor frames."""

    def __init__(self, mv: memoryview, layout):
        self._mv = mv
        self.layout = layout
        self._meta: dict = {}
        self._view = WeftTensorView(self)
        self.stats = {"commits": 0, "acquire_calls": 0, "acquire_retries": 0,
                      "torn_reads": 0, "overruns": 0}
        self._writable = mv.readonly is False

        # Precreated per-slot payload views + data addresses + ndarrays
        # (Law 1: the acquire/as_numpy hot path returns these — zero alloc).
        self._slot_mv = []
        self._slot_addr = []
        self._slot_ndarray = []
        self._default_fourcc = fourcc_from_str("RAW ")
        try:
            import numpy as np
            self._np = np
        except ImportError:  # pragma: no cover - numpy is a declared dep
            self._np = None

        import numpy as np  # guaranteed above; local alias for clarity
        dt = np.dtype(numpy_dtype_str(layout.dtype_code, layout.dtype_bits))
        shape = tuple(int(layout.shape[d]) for d in range(layout.rank))
        strides_bytes = tuple(int(layout.strides[d]) * layout.elem_size
                              for d in range(layout.rank))
        for s in range(layout.slot_count):
            payload_base = layout.header_size + s * layout.slot_stride + SLOT_HEADER_SIZE
            self._slot_mv.append(mv[payload_base: payload_base + layout.payload_cap])
            # Data address of this slot's payload (stable for the buffer's life).
            probe = np.frombuffer(mv, dtype=np.uint8, count=1, offset=payload_base)
            self._slot_addr.append(probe.__array_interface__["data"][0])
            arr = np.ndarray(shape, dtype=dt, buffer=mv, offset=payload_base,
                             strides=strides_bytes)
            arr.setflags(write=self._writable)
            self._slot_ndarray.append(arr)

    # -- properties ----------------------------------------------------------

    @property
    def buffer(self):
        return self._mv.obj

    @property
    def slot_count(self) -> int:
        return self.layout.slot_count

    @property
    def slot_stride(self) -> int:
        return self.layout.slot_stride

    @property
    def payload_cap(self) -> int:
        return self.layout.payload_cap

    @property
    def byte_length(self) -> int:
        return len(self._mv)

    @property
    def producer_seq(self) -> int:
        return struct.unpack_from("<Q", self._mv, OFF_PRODUCER_SEQ)[0]

    def describe(self) -> str:
        L = self.layout
        shape = "x".join(str(L.shape[d]) for d in range(L.rank))
        return (f"WeftRing(v{L.version}, slots={L.slot_count}, stride={L.slot_stride}, "
                f"dtype={L.dtype_code}/{L.dtype_bits}, shape=[{shape}], "
                f"schema={L.schema_id:#x}, readonly={not self._writable})")

    # -- construction ----------------------------------------------------------

    @classmethod
    def create(cls, slot_count: int = 4, payload_cap: int = 4096,
               dtype_code: int = DLPACK_FLOAT, dtype_bits: int = 32,
               shape=None, schema_id: int = 0, tick_hz: int = 0,
               shared: bool = False, fourcc: str = "RAW ") -> "WeftRing":
        """Create a new ring in a bytearray (shared=True reserved for mmap use)."""
        if slot_count < 2:
            raise LayoutError("WTR1_BAD_SLOT_COUNT", f"slot_count {slot_count} < 2")
        if payload_cap < 1:
            raise LayoutError("WTR1_BAD_PAYLOAD_CAP", f"payload_cap {payload_cap} < 1")
        if not shape or len(shape) > MAX_RANK or any(d < 1 for d in shape):
            raise LayoutError("WTR1_BAD_RANK", f"shape {shape} invalid")
        elem_size = dtype_bits // 8
        elems = 1
        for d in shape:
            elems *= d
        if elems * elem_size > payload_cap:
            raise LayoutError("WTR1_TOO_SMALL",
                              f"shape needs {elems * elem_size}B > payload_cap {payload_cap}B")
        slot_stride = _align64(SLOT_HEADER_SIZE + payload_cap)
        total = RING_HEADER_SIZE + slot_count * slot_stride
        buf = bytearray(total)
        mv = memoryview(buf)
        struct.pack_into("<4s", mv, OFF_MAGIC, RING_MAGIC)
        struct.pack_into("<H", mv, OFF_LAYOUT_VERSION, LAYOUT_VERSION)
        struct.pack_into("<H", mv, OFF_HEADER_SIZE, RING_HEADER_SIZE)
        struct.pack_into("<I", mv, OFF_SLOT_COUNT, slot_count)
        struct.pack_into("<I", mv, OFF_SLOT_STRIDE, slot_stride)
        struct.pack_into("<B", mv, OFF_DTYPE_CODE, dtype_code)
        struct.pack_into("<B", mv, OFF_DTYPE_BITS, dtype_bits)
        struct.pack_into("<H", mv, OFF_LANES, 1)
        struct.pack_into("<I", mv, OFF_ELEM_SIZE, elem_size)
        for d in range(MAX_RANK):
            struct.pack_into("<I", mv, OFF_SHAPE + 4 * d, shape[d] if d < len(shape) else 0)
        acc = 1
        for d in range(len(shape) - 1, -1, -1):
            struct.pack_into("<I", mv, OFF_STRIDES + 4 * d, acc)
            acc *= shape[d]
        if schema_id < 0 or schema_id >= (1 << 64):
            raise LayoutError("WTR1_BAD_U64", f"schema_id {schema_id} not u64")
        struct.pack_into("<Q", mv, OFF_SCHEMA_ID, schema_id)
        struct.pack_into("<I", mv, OFF_TICK_HZ, tick_hz & 0xFFFFFFFF)
        flags = RING_FLAG_LITTLE_ENDIAN | (RING_FLAG_SHARED_MEMORY if shared else 0)
        struct.pack_into("<I", mv, OFF_FLAGS, flags)
        struct.pack_into("<I", mv, OFF_HEADER_CRC, ring_header_crc(mv))
        ring = cls(mv, validate_ring_header(mv))
        ring._default_fourcc = fourcc_from_str(fourcc)
        ring._own_buf = buf  # keep alive
        return ring

    @classmethod
    def attach(cls, buf) -> "WeftRing":
        """Adopt EXISTING memory (fixture file, mmap, bytes, ndarray). Law 4 runs here."""
        mv = _as_memoryview(buf)
        layout = validate_ring_header(mv)
        ring = cls(mv, layout)
        ring._own_buf = buf if isinstance(buf, (bytearray, memoryview)) else None
        return ring

    # -- producer ----------------------------------------------------------------

    def _slot_base(self, seq: int) -> int:
        return self.layout.header_size + ((seq - 1) % self.layout.slot_count) * self.layout.slot_stride

    def commit(self, payload, *, ts: Optional[int] = None, timestamp_ns: Optional[int] = None,
               duration_us: int = 0, fourcc=None, rank: Optional[int] = None,
               planes: int = 1) -> int:
        """Copy `payload` (bytes-like, len <= payload_cap) into the next slot and publish.

        Exact-length payloads are a single memcpy (zero temporary buffers);
        short payloads slice once (documented small temporary view).
        Returns the committed sequence number.
        """
        mv = memoryview(payload)
        if mv.format != "B":
            mv = mv.cast("B")
        n = len(mv)
        if n > self.layout.payload_cap:
            raise LayoutError("WTR1_COMMIT_RANGE",
                              f"payload {n}B > cap {self.layout.payload_cap}B")
        if not self._writable:
            raise LayoutError("WTR1_READONLY", "ring buffer is read-only (attach to a writable buffer to produce)")
        seq = self.producer_seq + 1
        base = self._slot_base(seq)
        dst = self._slot_mv[(seq - 1) % self.layout.slot_count]
        if n == len(dst):
            dst[:] = mv  # single memcpy
        else:
            dst[:n] = mv[:n]  # short payload: one small temp slice (documented)

        fc = self._default_fourcc if fourcc is None else (
            fourcc_from_str(fourcc) if isinstance(fourcc, str) else int(fourcc))
        ts_val = ts if ts is not None else (timestamp_ns if timestamp_ns is not None else 0)
        if ts_val < 0 or ts_val >= (1 << 64):
            raise LayoutError("WTR1_BAD_U64", f"timestamp {ts_val} not u64")
        r = self.layout.rank if rank is None else rank

        # Slot header: COMMITTED bit CLEAR first, all fields, bit SET, publish.
        struct.pack_into("<I", self._mv, base + SOFF_SLOT_FLAGS, 0)
        self._mv[base + SOFF_MAGIC: base + SOFF_MAGIC + 4] = SLOT_MAGIC
        struct.pack_into("<I", self._mv, base + SOFF_PAYLOAD_LEN, n)
        struct.pack_into("<Q", self._mv, base + SOFF_SEQ, seq)
        struct.pack_into("<Q", self._mv, base + SOFF_TIMESTAMP_NS, ts_val)
        struct.pack_into("<I", self._mv, base + SOFF_DURATION_US, duration_us & 0xFFFFFFFF)
        struct.pack_into("<I", self._mv, base + SOFF_FOURCC, fc)
        struct.pack_into("<B", self._mv, base + SOFF_RANK, r)
        struct.pack_into("<B", self._mv, base + SOFF_PLANES, planes)
        struct.pack_into("<I", self._mv, base + SOFF_PLANE_OFFSET, 0)
        struct.pack_into("<I", self._mv, base + SOFF_PLANE_SIZE, n)
        struct.pack_into("<I", self._mv, base + SOFF_SLOT_FLAGS, SLOT_FLAG_COMMITTED)

        # Publish: producer_seq = seq. Python has no cross-process atomics on
        # mmap; the seqlock slot-seq re-validation makes this safe (parity with
        # the TS two-store protocol: hi word first, LO word LAST).
        struct.pack_into("<I", self._mv, OFF_PRODUCER_SEQ + 4, (seq >> 32) & 0xFFFFFFFF)
        struct.pack_into("<I", self._mv, OFF_PRODUCER_SEQ, seq & 0xFFFFFFFF)
        self.stats["commits"] += 1
        return seq

    # -- consumer ----------------------------------------------------------------

    def acquire_latest(self, last_seq: Optional[int] = None) -> Optional[WeftTensorView]:
        """Wait-free acquire of the latest committed frame into a REUSED view.

        Returns None when the ring is empty / nothing newer than `last_seq` /
        the overrun window was hit (bounded retries exhausted).
        """
        self.stats["acquire_calls"] += 1
        L = self.layout
        for _ in range(L.slot_count):
            s = self.producer_seq
            if s == 0:
                return None
            if last_seq is not None and s <= last_seq:
                return None
            slot = (s - 1) % L.slot_count
            base = L.header_size + slot * L.slot_stride
            if read_slot_header(self._mv, base, self._meta) and self._meta["seq"] == s:
                self._view._bind(base, slot, self._meta)
                return self._view
            self.stats["acquire_retries"] += 1
            self.stats["torn_reads"] += 1
        self.stats["overruns"] += 1
        return None

    def acquire_frame(self, seq: int) -> Optional[WeftTensorView]:
        """Acquire an EXACT sequence number, or None (not yet / lapped)."""
        if seq < 1:
            return None
        self.stats["acquire_calls"] += 1
        L = self.layout
        latest = self.producer_seq
        if seq > latest:
            return None
        if latest - seq >= L.slot_count:
            self.stats["overruns"] += 1
            return None
        slot = (seq - 1) % L.slot_count
        base = L.header_size + slot * L.slot_stride
        if read_slot_header(self._mv, base, self._meta) and self._meta["seq"] == seq:
            self._view._bind(base, slot, self._meta)
            return self._view
        return None

    def wait_for_new_frame(self, last_seq: int, timeout_s: float = 1.0,
                           poll_s: float = 0.0005) -> Optional[int]:
        """Poll until producer_seq advances past `last_seq` (bridge-friendly)."""
        deadline = time.monotonic() + timeout_s
        while True:
            cur = self.producer_seq
            if cur > last_seq:
                return cur
            if time.monotonic() >= deadline:
                return None
            time.sleep(poll_s)

    # -- numpy / dlpack bridge ----------------------------------------------------

    def _slot_ndarray_for(self, slot: int):
        """Precreated zero-copy ndarray over slot `slot` (read-only if buffer ro)."""
        return self._slot_ndarray[slot]

    def _slot_addr_for(self, slot: int) -> int:
        return self._slot_addr[slot]
