# ring.py — RNG1 robotics record ring reader (managed Python side).
#
# Mirrors docs/adapters/MANAGED-SEAMS-V1.md §5 byte-for-byte:
#   - 128B header (magic/version/slot geometry/seqlocks/topic table)
#   - per-slot seqlock: seq odd = write in flight (torn), re-read after
#     payload = publish check
#   - drop-not-block (Law 4): a reader NEVER blocks, allocates or throws
#     on torn/skipped records — counters only
#   - flyweight acquire: ONE RecordView reused across the whole feed;
#     every per-record temporary is refcount-freed inside the iteration
#
# Zero-residue contract (Law 1): reader counters live in ONE preallocated
# array('q'); the view window is released on close().

import struct
from array import array

RNG1_MAGIC = b'RNG1'
RNG1_VERSION = 1
RNG1_HEADER_SIZE = 128
SLOT_HDR = 64

FMT_IMU6DOF = 1
FMT_POINTS_F32 = 2
FMT_FRAME_DESC = 3
FMT_BOXES_F32 = 4

FMT_NAMES = {
    FMT_IMU6DOF: 'imu6dof',
    FMT_POINTS_F32: 'points_f32',
    FMT_FRAME_DESC: 'frame_desc',
    FMT_BOXES_F32: 'boxes_f32',
}

# header offsets
_OFF_SLOT_SIZE = 8
_OFF_SLOT_COUNT = 12
_OFF_WRITE_SEQ = 16
_OFF_COMMITTED = 24
_OFF_DROP_COUNT = 32
_OFF_OVERWRITE = 40
_OFF_TOPICS = 64

# slot header offsets
_S_SEQ = 0
_S_LEN = 8
_S_TOPIC = 12
_S_TS = 16
_S_FMT = 24
_S_FLAGS = 28

_U16 = struct.Struct('<H')
_U32 = struct.Struct('<I')
_U64 = struct.Struct('<Q')

# counter slots (unboxed — no boxed-int residue, Law 1)
C_ACQUIRES = 0
C_TORN = 1
C_SKIPPED = 2
C_FILTERED = 3
C_COUNT = 4


class Rng1Error(Exception):
    """Typed RNG1 attach failure (never raised during the feed)."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


E_SHORT = 1
E_MAGIC = 2
E_VERSION = 3
E_HEADER = 4
E_GEOMETRY = 5


def _validate_header(mv):
    n = len(mv)
    if n < RNG1_HEADER_SIZE:
        return E_SHORT
    if bytes(mv[0:4]) != RNG1_MAGIC:
        return E_MAGIC
    if _U16.unpack_from(mv, 4)[0] != RNG1_VERSION:
        return E_VERSION
    if _U16.unpack_from(mv, 6)[0] != RNG1_HEADER_SIZE:
        return E_HEADER
    slot_size = _U32.unpack_from(mv, _OFF_SLOT_SIZE)[0]
    slot_count = _U32.unpack_from(mv, _OFF_SLOT_COUNT)[0]
    if slot_size < 256 or (slot_size & (slot_size - 1)) != 0:
        return E_GEOMETRY
    if slot_count == 0 or (slot_count & (slot_count - 1)) != 0:
        return E_GEOMETRY
    if n < RNG1_HEADER_SIZE + slot_size * slot_count:
        return E_SHORT
    return 0


class RecordView:
    """Flyweight over one RNG1 slot. Rebinds per acquire; payload bytes
    are only meaningful until the next acquire (documented lifetime)."""

    __slots__ = ('_mv', '_base', '_seq', 'topic_id', 'ts_ns', 'fmt',
                 'flags', 'payload_off', 'payload_len')

    def __init__(self):
        self._mv = None
        self._base = 0
        self._seq = 0

    def _bind(self, mv, slot_base, seq):
        self._mv = mv
        self._base = slot_base
        self._seq = _U64.unpack_from(mv, slot_base + _S_SEQ)[0]
        self.topic_id = _U32.unpack_from(mv, slot_base + _S_TOPIC)[0]
        self.ts_ns = _U64.unpack_from(mv, slot_base + _S_TS)[0]
        self.fmt = _U32.unpack_from(mv, slot_base + _S_FMT)[0]
        self.flags = _U32.unpack_from(mv, slot_base + _S_FLAGS)[0]
        self.payload_off = slot_base + SLOT_HDR
        self.payload_len = _U32.unpack_from(mv, slot_base + _S_LEN)[0]

    def fmt_name(self):
        return FMT_NAMES.get(self.fmt, f'fmt{self.fmt}')

    def points_view(self):
        """POINTS_F32 payload as a 0-copy Float32 trio view.

        Returns (memoryview, count) — np.frombuffer(view, dtype='<f4')
        shares the SAME memory (pointer identity holds).
        """
        n = self.payload_len // 12
        return self._mv[self.payload_off:self.payload_off + n * 12], n

    def boxes_view(self):
        """BOXES_F32 payload as a 0-copy 6-float-stride view."""
        n = self.payload_len // 24
        return self._mv[self.payload_off:self.payload_off + n * 24], n

    def imu(self):
        """IMU6DOF -> (ts_ns, qw, qx, qy, qz, gx, gy, gz) — 8 x f64 LE."""
        return struct.unpack_from('<8d', self._mv, self.payload_off)

    def frm1(self):
        """FRAME_DESC payload -> dict with FRM1 fields (fail-closed)."""
        if self.payload_len < 32:
            return None
        off = self.payload_off
        magic = bytes(self._mv[off:off + 4])
        if magic != b'FRM1':
            return None
        return {
            'width': _U32.unpack_from(self._mv, off + 4)[0],
            'height': _U32.unpack_from(self._mv, off + 8)[0],
            'stride': _U32.unpack_from(self._mv, off + 12)[0],
            'format': _U32.unpack_from(self._mv, off + 16)[0],
            'handle': (_U32.unpack_from(self._mv, off + 20)[0],
                       _U32.unpack_from(self._mv, off + 24)[0]),
            'flags': _U32.unpack_from(self._mv, off + 28)[0],
        }


class RingReader:
    """Drop-not-block RNG1 consumer. One instance per attached ring."""

    def __init__(self, buffer):
        self._mv = memoryview(buffer)
        code = _validate_header(self._mv)
        if code != 0:
            raise Rng1Error(code)
        self.slot_size = _U32.unpack_from(self._mv, _OFF_SLOT_SIZE)[0]
        self.slot_count = _U32.unpack_from(self._mv, _OFF_SLOT_COUNT)[0]
        self.view = RecordView()
        self.stats = array('q', bytes(8 * C_COUNT))
        self._next_seq = 1
        self._closed = False

    # -- introspection -------------------------------------------------------
    def committed_seq(self):
        return _U64.unpack_from(self._mv, _OFF_COMMITTED)[0]

    def write_seq(self):
        return _U64.unpack_from(self._mv, _OFF_WRITE_SEQ)[0]

    def header_drop_count(self):
        return _U64.unpack_from(self._mv, _OFF_DROP_COUNT)[0]

    def topic_table(self):
        out = []
        for i in range(8):
            v = _U32.unpack_from(self._mv, _OFF_TOPICS + i * 4)[0]
            if v:
                out.append(v)
        return out

    @property
    def torn(self):
        return self.stats[C_TORN]

    @property
    def skipped(self):
        return self.stats[C_SKIPPED]

    @property
    def filtered(self):
        return self.stats[C_FILTERED]

    @property
    def acquires(self):
        return self.stats[C_ACQUIRES]

    # -- hot path --------------------------------------------------------------
    def acquire(self, topic_id=None):
        """Acquire the newest continuous record (or None).

        topic_id=None accepts any topic; an int filters on it. Returns the
        REUSED RecordView or None — callers must not retain it.
        """
        self.stats[C_ACQUIRES] += 1
        mv = self._mv
        committed = _U64.unpack_from(mv, _OFF_COMMITTED)[0]
        # continuous window: keep the NEWEST record per acquire
        # (drop-not-block; old frames are irrelevant to live consumers)
        if committed == 0 or self._next_seq > committed:
            return None
        slot = (committed - 1) % self.slot_count
        base = RNG1_HEADER_SIZE + slot * self.slot_size
        seq1 = _U64.unpack_from(mv, base + _S_SEQ)[0]
        # torn = seq does not match the expected continuous window value
        # (an in-flight write publishes an odd transient; the re-read below
        # catches it). Odd COMMITTED seqs are legal — seqs start at 1.
        if seq1 != committed:
            self.stats[C_TORN] += 1
            self._next_seq = committed + 1
            return None
        view = self.view
        view._bind(mv, base, seq1)
        if topic_id is not None and view.topic_id != topic_id:
            self.stats[C_FILTERED] += 1
            self._next_seq = committed + 1
            return None
        seq2 = _U64.unpack_from(mv, base + _S_SEQ)[0]
        if seq2 != seq1:
            self.stats[C_TORN] += 1
            self._next_seq = committed + 1
            return None
        self._next_seq = committed + 1
        return view

    def drain(self):
        """Yield EVERY continuous committed record seq 1..committed
        (audit / parity lane). Torn seqs are counted and skipped.
        Yields the reused view — consume within the loop."""
        mv = self._mv
        committed = _U64.unpack_from(mv, _OFF_COMMITTED)[0]
        for s in range(1, committed + 1):
            slot = (s - 1) % self.slot_count
            base = RNG1_HEADER_SIZE + slot * self.slot_size
            seq1 = _U64.unpack_from(mv, base + _S_SEQ)[0]
            if seq1 != s:
                self.stats[C_TORN] += 1
                continue
            view = self.view
            view._bind(mv, base, seq1)
            seq2 = _U64.unpack_from(mv, base + _S_SEQ)[0]
            if seq2 != seq1:
                self.stats[C_TORN] += 1
                continue
            self.stats[C_ACQUIRES] += 1
            yield view

    def close(self):
        """Release the ring window (zero-residue contract)."""
        if not self._closed:
            self._closed = True
            self._mv.release()
