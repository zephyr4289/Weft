# generate_helpers.py — Python-side ITCH writers mirroring fixtures/generate.mjs.
#
# Byte-exact copies of the JS writers for the message types the unit tests
# need. The GOLDEN STREAM itself is always the committed Node artifact (no
# drift by construction); these helpers only craft small synthetic messages
# for unit tests.

import struct

LOCATE = 7777
TRACKING = 42
STOCK = b'WEFTUSD '


def _prefix(n):
    return struct.pack('>H', n)


def _ts6(p, ts):
    hi = ts >> 32
    lo = ts & 0xFFFFFFFF
    struct.pack_into('>H', p, 5, hi)
    struct.pack_into('>I', p, 7, lo)


def itch_system_event(ts, code):
    p = bytearray(36)
    p[0] = 0x53
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    p[11] = ord(code)
    return _prefix(36) + bytes(p)


def itch_add(ts, ref_hi, ref_lo, side, shares, price):
    p = bytearray(36)
    p[0] = 0x41
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    struct.pack_into('>I', p, 11, ref_hi)
    struct.pack_into('>I', p, 15, ref_lo)
    p[19] = 0x42 if side == 'B' else 0x53
    struct.pack_into('>I', p, 20, shares)
    p[24:32] = STOCK
    struct.pack_into('>I', p, 32, price)
    return _prefix(36) + bytes(p)


def itch_execute(ts, ref_hi, ref_lo, shares, match):
    p = bytearray(31)
    p[0] = 0x45
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    struct.pack_into('>I', p, 11, ref_hi)
    struct.pack_into('>I', p, 15, ref_lo)
    struct.pack_into('>I', p, 19, shares)
    struct.pack_into('>I', p, 23, match >> 32)
    struct.pack_into('>I', p, 27, match & 0xFFFFFFFF)
    return _prefix(31) + bytes(p)


def itch_cancel(ts, ref_hi, ref_lo, cancelled):
    p = bytearray(23)
    p[0] = 0x58
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    struct.pack_into('>I', p, 11, ref_hi)
    struct.pack_into('>I', p, 15, ref_lo)
    struct.pack_into('>I', p, 19, cancelled)
    return _prefix(23) + bytes(p)


def itch_delete(ts, ref_hi, ref_lo):
    p = bytearray(19)
    p[0] = 0x44
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    struct.pack_into('>I', p, 11, ref_hi)
    struct.pack_into('>I', p, 15, ref_lo)
    return _prefix(19) + bytes(p)


def itch_replace(ts, o_hi, o_lo, n_hi, n_lo, shares, price):
    p = bytearray(35)
    p[0] = 0x55
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    struct.pack_into('>I', p, 11, o_hi)
    struct.pack_into('>I', p, 15, o_lo)
    struct.pack_into('>I', p, 19, n_hi)
    struct.pack_into('>I', p, 23, n_lo)
    struct.pack_into('>I', p, 27, shares)
    struct.pack_into('>I', p, 31, price)
    return _prefix(35) + bytes(p)


def itch_trade(ts, ref_hi, ref_lo, side, shares, price, match):
    p = bytearray(44)
    p[0] = 0x50
    struct.pack_into('>H', p, 1, LOCATE)
    struct.pack_into('>H', p, 3, TRACKING)
    _ts6(p, ts)
    struct.pack_into('>I', p, 11, ref_hi)
    struct.pack_into('>I', p, 15, ref_lo)
    p[19] = 0x42 if side == 'B' else 0x53
    struct.pack_into('>I', p, 20, shares)
    p[24:32] = STOCK
    struct.pack_into('>I', p, 32, price)
    struct.pack_into('>I', p, 36, match >> 32)
    struct.pack_into('>I', p, 40, match & 0xFFFFFFFF)
    return _prefix(44) + bytes(p)


class FakeView:
    """Duck-typed ItchView substitute for unit tests (no wire needed).

    Mirrors ItchView's lazy-cached attribute contract (ref/ref2/ts plain
    ints; shares/price/match/side/locate per-message properties).
    """

    __slots__ = ('type', 'mv', 'off', '_ts', '_ref', '_ref2',
                 '_side', '_shares', '_price', '_match', '_locate')

    def __init__(self, type_, fields):
        self.type = type_
        self.mv = None
        self.off = 0
        self._ts = fields.get('ts', 1)
        self._ref = fields.get('refHi', 0) * 4294967296 + fields.get('refLo', 0)
        self._ref2 = fields.get('ref2Hi', 0) * 4294967296 + fields.get('ref2Lo', 0)
        self._side = fields.get('side', 0x42)
        self._shares = fields.get('shares', 0)
        self._price = fields.get('price', 0)
        self._match = fields.get('match', 0)
        self._locate = fields.get('locate', 7777)

    @property
    def ts(self):
        return self._ts

    @property
    def ref(self):
        return self._ref

    @property
    def ref2(self):
        return self._ref2

    @property
    def side(self):
        return self._side

    @property
    def shares(self):
        return self._shares

    @property
    def price(self):
        return self._price

    @property
    def match(self):
        return self._match

    @property
    def locate(self):
        return self._locate


def fake_view(type_, fields):
    return FakeView(type_, fields)
