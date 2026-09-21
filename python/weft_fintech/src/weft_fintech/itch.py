# itch.py — NASDAQ TotalView-ITCH 5.0 flyweight parser (managed Python side).
#
# Mirrors packages/fintech/src/itch.js byte-for-byte:
#   - framing [u16 BE length][payload], unknown types skipped, E_TRUNC closed
#   - type-aware semantic fields (shares/price/match offsets per message type)
#   - u64 order refs handled as exact Python ints (never floats)
#   - u48 timestamps via hi * 2**32 + lo (exact)
#
# Zero-permanent-allocation contract: every per-message decode uses
# struct.Struct.unpack_from (a temporary tuple, freed by refcount before the
# next message) into a FLYWEIGHT — the view object is reused across the whole
# feed. tracemalloc proves EXACT 0 bytes of permanent growth over 1,000,000
# messages (tests/test_alloc.py):
#   - engine counters (bytes_processed / truncated / resume) live in ONE
#     preallocated array('q') — boxed int attributes would leave their FINAL
#     values as live blocks at measurement time;
#   - process() releases its decode scratch on return: the memoryview window
#     over the caller's buffer is re-pointed to the internal scratch created
#     in __init__, and the flyweight's lazily-cached u48/u64 fields are
#     reset to the -1 sentinel (small int, no heap block).
# Per-message fields are therefore only meaningful DURING the feed / inside
# apply callbacks — the same flyweight lifetime the TS engine documents.

import struct

from array import array

E_TRUNC = 1

T_SYSTEM = 0x53
T_ADD = 0x41
T_ADD_MPID = 0x46
T_EXEC = 0x45
T_EXEC_PRICE = 0x43
T_CANCEL = 0x58
T_DELETE = 0x44
T_REPLACE = 0x55
T_TRADE = 0x50

_U16 = struct.Struct('>H')
_U32 = struct.Struct('>I')


class ItchView:
    """Reusable flyweight over one ITCH message. bind(off) re-points it.

    Prefix fields (ts / ref / ref2) are lazily decoded once per bind and
    cached in __slots__ (sentinel -1: u48/u64 values are never negative),
    so hot messages only unpack the fields they actually use.
    """

    __slots__ = ('mv', 'off', 'type', '_ts', '_ref', '_ref2')

    def __init__(self, buffer):
        self.mv = memoryview(buffer)
        self.off = 0
        self.type = 0
        self._ts = -1
        self._ref = -1
        self._ref2 = -1

    def bind(self, off):
        self.off = off
        self.type = self.mv[off]
        self._ts = -1
        self._ref = -1
        self._ref2 = -1
        return self

    @property
    def locate(self):
        return _U16.unpack_from(self.mv, self.off + 1)[0]

    @property
    def tracking(self):
        return _U16.unpack_from(self.mv, self.off + 3)[0]

    @property
    def ts(self):
        v = self._ts
        if v < 0:
            hi = _U16.unpack_from(self.mv, self.off + 5)[0]
            lo = _U32.unpack_from(self.mv, self.off + 7)[0]
            v = hi * 4294967296 + lo
            self._ts = v
        return v

    # u64 order ref: hi at +11, lo at +15 (BE wire order)
    @property
    def ref(self):
        v = self._ref
        if v < 0:
            hi = _U32.unpack_from(self.mv, self.off + 11)[0]
            lo = _U32.unpack_from(self.mv, self.off + 15)[0]
            v = hi * 4294967296 + lo
            self._ref = v
        return v

    # second ref (replace new-order id): hi at +19, lo at +23
    @property
    def ref2(self):
        v = self._ref2
        if v < 0:
            hi = _U32.unpack_from(self.mv, self.off + 19)[0]
            lo = _U32.unpack_from(self.mv, self.off + 23)[0]
            v = hi * 4294967296 + lo
            self._ref2 = v
        return v

    @property
    def side(self):
        return self.mv[self.off + 19]  # 0x42 buy / 0x53 sell

    @property
    def shares(self):
        t = self.type
        off = self.off
        if t == T_REPLACE:                      # 'U' at +27
            return _U32.unpack_from(self.mv, off + 27)[0]
        if t == T_EXEC or t == T_EXEC_PRICE or t == T_CANCEL:  # 'E','C','X' at +19
            return _U32.unpack_from(self.mv, off + 19)[0]
        return _U32.unpack_from(self.mv, off + 20)[0]          # 'A','F','P'

    @property
    def price(self):
        t = self.type
        off = self.off
        if t == T_REPLACE:                      # 'U' at +31
            return _U32.unpack_from(self.mv, off + 31)[0]
        return _U32.unpack_from(self.mv, off + 32)[0]  # 'A','F','P','C'

    @property
    def match(self):
        t = self.type
        off = self.off
        if t == T_TRADE:                        # 'P' match at +36
            hi = _U32.unpack_from(self.mv, off + 36)[0]
            lo = _U32.unpack_from(self.mv, off + 40)[0]
        else:                                   # 'E','C' match at +23
            hi = _U32.unpack_from(self.mv, off + 23)[0]
            lo = _U32.unpack_from(self.mv, off + 27)[0]
        return hi * 4294967296 + lo

    def event_code(self):
        return chr(self.mv[self.off + 11])

    def stock_into(self, scratch):
        """Raw 8 stock bytes into a caller-owned writable (no string alloc)."""
        off = self.off + 24
        for i in range(8):
            scratch[i] = self.mv[off + i]
        return scratch


def frame_offsets(buffer, max_count=1 << 20):
    """View-relative payload-start offset of every framed message (cold)."""
    mv = memoryview(buffer)
    n = len(mv)
    out = []
    off = 0
    while off + 2 <= n and len(out) < max_count:
        (length,) = _U16.unpack_from(mv, off)
        out.append(off + 2)
        off += 2 + length
    return out


class ItchEngine:
    """Feed engine over a book. One view, reused. Resume-capable.

    Resume applies ONLY within one underlying buffer object (checkpoint
    pumping over one stream); a new chunk buffer restarts at 0 — same
    semantics as the TS engine.

    Zero-residue contract: counters are unboxed into _estats; decode scratch
    (buffer window + cached u48/u64 fields) is released when process()
    returns, so a traced run retains EXACTLY 0 bytes (Law 1 / Stage 3).
    """

    __slots__ = ('book', 'view', '_estats', '_last_buffer', '_scratch_mv')

    _E_TRUNCATED = 0
    _E_BYTES = 1
    _E_RESUME = 2

    def __init__(self, book):
        self.book = book
        scratch = bytearray(64)
        self.view = ItchView(scratch)
        self._scratch_mv = self.view.mv  # same object: restore target
        self._estats = array('q', bytes(8 * 3))
        self._last_buffer = None

    @property
    def truncated(self):
        return self._estats[ItchEngine._E_TRUNCATED]

    @property
    def bytes_processed(self):
        return self._estats[ItchEngine._E_BYTES]

    def process(self, buffer, end_byte=None):
        estats = self._estats
        if buffer is not self._last_buffer:
            estats[ItchEngine._E_RESUME] = 0
            self._last_buffer = buffer
        view = self.view
        mv = view.mv = memoryview(buffer)
        n = len(mv) if end_byte is None else end_byte
        view_bind = view.bind
        book_apply = self.book.apply_view
        off = estats[ItchEngine._E_RESUME]
        result = 0
        u16 = _U16.unpack_from
        trunc = ItchEngine._E_TRUNCATED
        bytes_e = ItchEngine._E_BYTES
        while off + 2 <= n:
            (msg_len,) = u16(mv, off)
            if off + 2 + msg_len > n:
                estats[trunc] += 1
                result = E_TRUNC
                break
            view_bind(off + 2)
            book_apply(view)
            estats[bytes_e] += 2 + msg_len
            off += 2 + msg_len
        estats[ItchEngine._E_RESUME] = off
        # release per-run decode scratch: next process() re-binds anyway
        view.mv = self._scratch_mv
        view.off = 0
        view.type = 0
        view._ts = -1
        view._ref = -1
        view._ref2 = -1
        return result
