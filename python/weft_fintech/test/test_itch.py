# test_itch.py — ITCH 5.0 flyweight parser semantics (Python side).

import struct

from weft_fintech.itch import (
    ItchView, ItchEngine, frame_offsets, E_TRUNC, T_ADD, T_REPLACE,
)


def test_view_decodes_big_endian_wire_fields_exactly():
    from generate_helpers import itch_add
    buf = itch_add(123456789, 0x00C0FFEE, 77, 'B', 500, 999900)
    view = ItchView(buf)
    view.bind(2)  # after the u16 BE length prefix
    assert view.type == T_ADD
    assert view.locate == 7777
    assert view.tracking == 42
    assert view.ts == 123456789
    assert view.ref == 0x00C0FFEE * 4294967296 + 77  # exact u64, never float
    assert view.side == 0x42
    assert view.shares == 500
    assert view.price == 999900
    scratch = bytearray(8)
    view.stock_into(scratch)
    assert bytes(scratch).rstrip(b' ') == b'WEFTUSD'


def test_execute_cancel_replace_trade_offsets_are_per_spec():
    from generate_helpers import (itch_execute, itch_cancel, itch_replace,
                                  itch_trade)
    ex = itch_execute(1, 0, 42, 300, 9000)
    v = ItchView(ex); v.bind(2)
    assert v.shares == 300  # 'E' shares at +19
    assert v.match == 9000

    ca = itch_cancel(2, 0, 42, 150)
    v = ItchView(ca); v.bind(2)
    assert v.shares == 150

    re = itch_replace(3, 0, 42, 0x00C0FFEE, 43, 250, 999000)
    v = ItchView(re); v.bind(2)
    assert v.shares == 250  # 'U' at +27
    assert v.price == 999000  # 'U' at +31
    assert v.ref2 == 0x00C0FFEE * 4294967296 + 43

    tr = itch_trade(4, 0, 42, 'S', 700, 1000500, 4294967296 + 5)
    v = ItchView(tr); v.bind(2)
    assert v.match == 4294967301  # u64 via hi*2^32+lo


def test_golden_stream_message_count_and_census(golden_stream, scenario):
    book = __import__('weft_fintech.book', fromlist=['OrderBook']).OrderBook()
    engine = ItchEngine(book)
    assert engine.process(golden_stream) == 0
    assert book.msgs_applied == scenario['message_count']
    assert book.skipped == 0
    assert book.adds == scenario['counts']['A']
    assert book.executes == scenario['counts']['E']
    assert book.cancels == scenario['counts']['X']
    assert book.deletes == scenario['counts']['D']
    assert book.replaces == scenario['counts']['U']
    assert book.trade_count == scenario['counts']['P'] + scenario['counts']['E']


def test_unknown_message_types_skipped_by_length_prefix():
    from generate_helpers import itch_add
    a1 = itch_add(1, 0, 1, 'B', 100, 999000)
    unk = bytearray(22)
    struct.pack_into('>H', unk, 0, 20)
    unk[2] = 0x7F
    a2 = itch_add(2, 0, 2, 'S', 100, 1000000)
    feed = bytes(a1) + bytes(unk) + bytes(a2)
    book = __import__('weft_fintech.book', fromlist=['OrderBook']).OrderBook()
    engine = ItchEngine(book)
    assert engine.process(feed) == 0
    assert book.skipped == 1
    assert book.adds == 2
    assert book.live_orders == 2


def test_corrupt_framing_fails_closed_with_e_trunc():
    from generate_helpers import itch_add
    a1 = itch_add(1, 0, 1, 'B', 100, 999000)
    liar = bytearray(2)
    struct.pack_into('>H', liar, 0, 500)
    feed = bytes(a1) + bytes(liar) + b'\x00' * 10
    book = __import__('weft_fintech.book', fromlist=['OrderBook']).OrderBook()
    engine = ItchEngine(book)
    assert engine.process(feed) == E_TRUNC
    assert engine.truncated == 1
    assert book.adds == 1


def test_incremental_pumping_same_buffer_produces_identical_state(golden_stream):
    from weft_fintech.book import OrderBook
    offs = frame_offsets(golden_stream)
    a_book = OrderBook()
    a_engine = ItchEngine(a_book)
    a_engine.process(golden_stream, offs[1000])
    a_engine.process(golden_stream)
    b_book = OrderBook()
    b_engine = ItchEngine(b_book)
    b_engine.process(golden_stream)
    assert a_book.msgs_applied == b_book.msgs_applied
    assert a_book.live_orders == b_book.live_orders
    assert a_book.best_bid() == b_book.best_bid()
    assert a_book.best_ask() == b_book.best_ask()
    assert a_book.trade_count == b_book.trade_count
