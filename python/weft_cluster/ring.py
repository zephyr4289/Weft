# ring.py — ShmRing: byte-exact mirror of the TS transport ring layout
# (packages/weft-cluster/src/transport/shm.js), attachable to:
#   * multiprocessing.shared_memory.SharedMemory blocks (live nodes)
#   * mmap()-ed files (cross-language handoff — TS writer, Python reader)
#
# Layout (docs/weft-cluster/WIRE-V1.md §3 style, WCRG):
#   0   4  magic "WCRG"        4  2  version=1      6  2  header_size=128
#   8   4  slot_count         12 4  slot_stride    16 4  payload_max
#   64  4  publish cursor (u32, total frames committed)
#   slot n = [u32 commit_lo (== n, written LAST)][u32 commit_hi]
#            [WCN1 datagram: 64B header + inline payload]
#
# Zero copy: reads return memoryview slices over the attached buffer; NumPy
# wraps them with np.frombuffer without copying (aliasing proven in tests).

import struct

from . import wire
from .errors import WC_OK

RING_MAGIC = 0x47524357  # "WCRG" little-endian
RING_HEADER = 128
DATA_OFF = 16


def _align8(n: int) -> int:
    return (n + 7) & ~7


class ShmRing:
    def __init__(self, buffer, slot_count: int, payload_max: int, stride: int):
        self.buf = buffer  # memoryview over bytearray / mmap / SharedMemory.buf
        self.slot_count = slot_count
        self.payload_max = payload_max
        self.stride = stride
        self._next_write = 0

    # -- constructors -----------------------------------------------------------

    @classmethod
    def create(cls, slot_count: int = 256, payload_max: int = 1024) -> "ShmRing":
        stride = _align8(DATA_OFF + 64 + payload_max)
        buf = bytearray(RING_HEADER + slot_count * stride)
        mv = memoryview(buf)
        struct.pack_into("<I", mv, 0, RING_MAGIC)
        struct.pack_into("<HH", mv, 4, 1, RING_HEADER)
        struct.pack_into("<III", mv, 8, slot_count, stride, payload_max)
        return cls(mv, slot_count, payload_max, stride)

    @classmethod
    def attach(cls, data, copy: bool = False) -> "ShmRing":
        """Attach to an existing ring image (mmap block, SharedMemory.buf,
        bytes). `copy=True` clones into a bytearray (handy for tests)."""
        mv = memoryview(data)
        if copy:
            mv = memoryview(bytearray(mv))
        magic, version, _hs = struct.unpack_from("<IHH", mv, 0)
        if magic != RING_MAGIC:
            from .errors import WeftClusterError
            raise WeftClusterError(1, "not a WCRG ring")
        slot_count, stride, payload_max = struct.unpack_from("<III", mv, 8)
        return cls(mv, slot_count, payload_max, stride)

    @classmethod
    def attach_shared_memory(cls, name: str) -> "ShmRing":
        """Attach a multiprocessing.shared_memory block by name (the TS side
        creates it via a file-backed image with the same layout)."""
        from multiprocessing import shared_memory
        shm = shared_memory.SharedMemory(name=name)
        ring = cls.attach(shm.buf)
        ring._shm = shm  # keep alive
        return ring

    # -- properties -------------------------------------------------------------

    @property
    def cursor(self) -> int:
        return struct.unpack_from("<I", self.buf, 64)[0]

    # -- writer (single producer, commit word written LAST) ----------------------

    def publish(self, n: int, src_mv, src_off: int, total: int) -> int:
        """Copy `total` bytes of a WCN1 datagram into slot for frame `n` and
        commit it. Single writer; reader observes via the cursor/commit word."""
        base = RING_HEADER + ((n - 1) % self.slot_count) * self.stride
        dst = self.buf
        for i in range(total):
            dst[base + DATA_OFF + i] = src_mv[src_off + i]
        struct.pack_into("<II", dst, base, 0, 0)          # hi first
        struct.pack_into("<I", dst, base, n & 0xFFFFFFFF)  # lo == n, commit
        struct.pack_into("<I", dst, 64, n & 0xFFFFFFFF)    # ring cursor
        return n

    # -- reader --------------------------------------------------------------------

    def try_read(self, n: int, frame: wire.Frame):
        """Zero-copy read of frame `n` into `frame`.
        Returns (code, frame): 0 = ok; 1 = torn; -1 = not yet published."""
        cur = self.cursor
        if n > cur:
            return -1, None
        base = RING_HEADER + ((n - 1) % self.slot_count) * self.stride
        lo = struct.unpack_from("<I", self.buf, base)[0]
        if lo != (n & 0xFFFFFFFF):
            return 1, None
        code = wire.decode_wcn1_into(self.buf, base + DATA_OFF, frame)
        if code != WC_OK:
            return code, None
        return WC_OK, frame

    def latest(self) -> wire.Frame | None:
        """Newest committed frame, zero-copy (None when empty)."""
        cur = self.cursor
        if cur == 0:
            return None
        f = wire.Frame()
        code, f = self.try_read(cur, f)
        return f if code == WC_OK else None

    def iter_from(self, start: int = 1):
        """Sequential zero-copy iterator over committed frames."""
        n = start
        while True:
            code, f = self.try_read(n, wire.Frame())
            if code == 1:  # torn — brief spin
                continue
            if code != WC_OK:
                return
            yield f
            n += 1
