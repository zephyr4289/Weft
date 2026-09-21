# test_mdp1.py — MDP1 wire: CRC vector, packing, view, flags (Python side).

import zlib

from weft_fintech.book import OrderBook
from weft_fintech.mdp1 import (
    pack_snapshot, Mdp1View, mdp1_crc32, MDP1_SIZE,
    F_BOOK_VALID, F_CROSSED, F_LOCKED,
)

from generate_helpers import fake_view

A = 0x41


def test_crc32_matches_standard_vector():
    assert mdp1_crc32(b'123456789', 0, 9) == 0xCBF43926
    assert zlib.crc32(b'123456789') == 0xCBF43926  # zlib IS CRC-32/ISO-HDLC


def test_empty_book_snapshot_valid_crc():
    b = OrderBook()
    buf = bytearray(MDP1_SIZE)
    pack_snapshot(b, buf, [0] * 10, [0] * 10)
    v = Mdp1View(buf)
    assert v.valid()
    assert v.version == 1
    assert v.flags & F_BOOK_VALID == 0
    assert v.best_bid == 0 and v.best_ask == 0
    for i in range(10):
        assert v.bid_price(i) == 0 and v.ask_price(i) == 0
    assert v.crc_ok()


def test_snapshot_levels_best_first():
    b = OrderBook()
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 998000}))
    b.apply_view(fake_view(A, {'refLo': 2, 'side': 0x42, 'shares': 200, 'price': 999000}))
    b.apply_view(fake_view(A, {'refLo': 3, 'side': 0x53, 'shares': 300, 'price': 1001000}))
    b.apply_view(fake_view(A, {'refLo': 4, 'side': 0x53, 'shares': 400, 'price': 1002000}))
    buf = bytearray(MDP1_SIZE)
    pack_snapshot(b, buf, [0] * 10, [0] * 10)
    v = Mdp1View(buf)
    assert v.crc_ok()
    assert v.flags & F_BOOK_VALID
    assert v.best_bid == 999000 and v.best_ask == 1001000
    assert (v.bid_price(0), v.bid_size(0), v.bid_orders(0)) == (999000, 200, 1)
    assert (v.bid_price(1), v.bid_size(1)) == (998000, 100)
    assert (v.ask_price(0), v.ask_size(0), v.ask_orders(0)) == (1001000, 300, 1)
    assert (v.ask_price(1), v.ask_size(1)) == (1002000, 400)
    assert v.ask_price(2) == 0


def test_crossed_and_locked_flags():
    crossed = OrderBook()
    crossed.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 10, 'price': 1002000}))
    crossed.apply_view(fake_view(A, {'refLo': 2, 'side': 0x53, 'shares': 10, 'price': 1000000}))
    cb = bytearray(MDP1_SIZE)
    pack_snapshot(crossed, cb, [0] * 10, [0] * 10)
    assert Mdp1View(cb).flags & F_CROSSED

    locked = OrderBook()
    locked.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 10, 'price': 1000000}))
    locked.apply_view(fake_view(A, {'refLo': 2, 'side': 0x53, 'shares': 10, 'price': 1000000}))
    lb = bytearray(MDP1_SIZE)
    pack_snapshot(locked, lb, [0] * 10, [0] * 10)
    lv = Mdp1View(lb)
    assert lv.flags & F_LOCKED
    assert not (lv.flags & F_CROSSED)


def test_crc_detects_corruption():
    b = OrderBook()
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    buf = bytearray(MDP1_SIZE)
    pack_snapshot(b, buf, [0] * 10, [0] * 10)
    assert Mdp1View(buf).crc_ok()
    buf[40] ^= 0xFF
    assert not Mdp1View(buf).crc_ok()
