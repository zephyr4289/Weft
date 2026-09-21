# mdp1.py — MDP1 snapshot wire (304B LE, CRC-32/ISO-HDLC) + zero-copy views.
#
# Byte-for-byte compatible with packages/fintech/src/mdp1.js. Python's
# zlib.crc32 IS CRC-32/ISO-HDLC (poly 0xEDB88320, init/xorout 0xFFFFFFFF) —
# the same algorithm the TS side implements by hand; the standard vector
# ("123456789" -> 0xCBF43926) is asserted in tests.

import struct
import zlib
from hashlib import sha256

MDP1_SIZE = 304
MDP1_VERSION = 1
MDP1_TOP_LEVELS = 10

F_BOOK_VALID = 1 << 0
F_CROSSED = 1 << 1
F_LOCKED = 1 << 2

_HDR = struct.Struct('<4sHHQQII')       # through best_ask (ends at 32)
_LVL = struct.Struct('<III')
_U64 = struct.Struct('<Q')
_U32 = struct.Struct('<I')
_MDP1_MAGIC = b'MDP1'


def mdp1_crc32(u8, start=0, end=296):
    return zlib.crc32(memoryview(u8)[start:end]) & MASK32


MASK32 = 0xFFFFFFFF


def pack_snapshot(book, out, scratch_bids, scratch_asks):
    """Pack the book state into a 304-byte writable. All-integer path."""
    mv = memoryview(out)
    flags = 0
    if book.msgs_applied > 0:
        flags |= F_BOOK_VALID
    bb = book.best_bid()
    ba = book.best_ask()
    if bb > 0 and ba > 0:
        if bb > ba:
            flags |= F_CROSSED
        elif bb == ba:
            flags |= F_LOCKED
    _HDR.pack_into(mv, 0, _MDP1_MAGIC, MDP1_VERSION, flags,
                   book.msgs_applied, book.last_ts, bb, ba)
    buy = book.buy
    sell = book.sell
    base = book.base_tick
    nb = book.top_levels_of(0, scratch_bids)
    na = book.top_levels_of(1, scratch_asks)
    for i in range(MDP1_TOP_LEVELS):
        o = 32 + i * 12
        if i < nb:
            t = scratch_bids[i] - base
            _LVL.pack_into(mv, o, scratch_bids[i], buy.agg_size[t], buy.order_count[t])
        else:
            mv[o:o + 12] = b'\x00' * 12
        a = 152 + i * 12
        if i < na:
            t = scratch_asks[i] - base
            _LVL.pack_into(mv, a, scratch_asks[i], sell.agg_size[t], sell.order_count[t])
        else:
            mv[a:a + 12] = b'\x00' * 12
    _U64.pack_into(mv, 272, book.msgs_applied)
    _U64.pack_into(mv, 280, book.trade_count)
    _U64.pack_into(mv, 288, book.last_match)
    mv[300:304] = b'\x00' * 4
    _U32.pack_into(mv, 296, mdp1_crc32(out))
    return out


class Mdp1View:
    """Read-only flyweight over an MDP1 record (UI binding side)."""

    __slots__ = ('mv',)

    def __init__(self, buffer):
        self.mv = memoryview(buffer)

    def valid(self):
        return bytes(self.mv[0:4]) == _MDP1_MAGIC

    @property
    def version(self):
        return struct.unpack_from('<H', self.mv, 4)[0]

    @property
    def flags(self):
        return struct.unpack_from('<H', self.mv, 6)[0]

    @property
    def seq(self):
        return struct.unpack_from('<I', self.mv, 8)[0]

    @property
    def last_ts(self):
        return struct.unpack_from('<Q', self.mv, 16)[0]

    @property
    def best_bid(self):
        return struct.unpack_from('<I', self.mv, 24)[0]

    @property
    def best_ask(self):
        return struct.unpack_from('<I', self.mv, 28)[0]

    def bid_price(self, i):
        return struct.unpack_from('<I', self.mv, 32 + i * 12)[0]

    def bid_size(self, i):
        return struct.unpack_from('<I', self.mv, 36 + i * 12)[0]

    def bid_orders(self, i):
        return struct.unpack_from('<I', self.mv, 40 + i * 12)[0]

    def ask_price(self, i):
        return struct.unpack_from('<I', self.mv, 152 + i * 12)[0]

    def ask_size(self, i):
        return struct.unpack_from('<I', self.mv, 156 + i * 12)[0]

    def ask_orders(self, i):
        return struct.unpack_from('<I', self.mv, 160 + i * 12)[0]

    @property
    def msg_count(self):
        return struct.unpack_from('<Q', self.mv, 272)[0]

    @property
    def trade_count(self):
        return struct.unpack_from('<Q', self.mv, 280)[0]

    def crc_ok(self):
        stored = struct.unpack_from('<I', self.mv, 296)[0]
        return stored == mdp1_crc32(self.mv)


def parity_of(snapshots):
    """sha256 over concatenated checkpoint snapshots (the parity artifact)."""
    h = sha256()
    for s in snapshots:
        h.update(s)
    return h.hexdigest()
